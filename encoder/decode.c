/* decode.c - decoder de referencia de A500VP.
 *
 *   a500vp-dec --in work\video.a5v [--preview work\preview.mp4]
 *
 * Hace exactamente lo que va a hacer el reproductor de la Amiga: dos
 * framebuffers planares, delta contra el oculto, repeticion sin intercambio,
 * paleta que llega con el delta. De ahi sale el preview y la verificacion.
 *
 * Si existe <in>.crc, compara frame por frame contra lo que simulo el
 * encoder. Un solo frame distinto es un error: el formato no esta bien
 * definido o una de las dos implementaciones se aparta de docs/FORMAT.md.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "a500vp.h"
#include "stream.h"

static void die(const char *msg)
{
    fprintf(stderr, "error: %s\n", msg);
    exit(1);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static unsigned be16(const uint8_t *p) { return (unsigned)(p[0] << 8) | p[1]; }

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long n;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); rewind(f);
    if (n <= 0) { fclose(f); return NULL; }
    b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(b); return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static uint32_t frame_crc(const uint8_t *vis, size_t fsz,
                          const A5Color *pal, int ncolors)
{
    static uint8_t pb[A5_MAX_BANDS * A5_MAX_COLORS * 2];   /* todas las franjas */
    int c;

    for (c = 0; c < ncolors; c++) {
        pb[c * 2]     = (uint8_t)(pal[c] >> 8);
        pb[c * 2 + 1] = (uint8_t)pal[c];
    }
    return a5_crc32(pb, (size_t)ncolors * 2, a5_crc32(vis, fsz, 0));
}

/* --- mediciones del Hito 4 ---------------------------------------------
 * El disco de medicion (player.s con BENCH) graba cuanto tardo en
 * decodificar cada DELTA, en color clocks, y un sector de resumen (formato
 * en FORMAT.md). Aca se cruzan con las estadisticas del mismo delta y se
 * ajusta el modelo de costo por minimos cuadrados. */
#define MEAS_TIMING_SECTOR 1754
#define MEAS_TIMING_MAX    640
#define MEAS_INFO_SECTOR   1759

typedef struct { int n, rows, cols, bytes; double cycles; } MeasPoint;

/* Resuelve A x = b de 3x3 por eliminacion con pivoteo parcial. */
static int solve3(double A[3][3], double b[3], double x[3])
{
    int i, j, k;

    for (i = 0; i < 3; i++) {
        int p = i;
        for (j = i + 1; j < 3; j++)
            if (fabs(A[j][i]) > fabs(A[p][i])) p = j;
        if (fabs(A[p][i]) < 1e-12) return -1;
        if (p != i) {
            double t;
            for (k = 0; k < 3; k++) { t = A[p][k]; A[p][k] = A[i][k]; A[i][k] = t; }
            t = b[p]; b[p] = b[i]; b[i] = t;
        }
        for (j = i + 1; j < 3; j++) {
            double f = A[j][i] / A[i][i];
            for (k = i; k < 3; k++) A[j][k] -= f * A[i][k];
            b[j] -= f * b[i];
        }
    }
    for (i = 2; i >= 0; i--) {
        double s = b[i];
        for (k = i + 1; k < 3; k++) s -= A[i][k] * x[k];
        x[i] = s / A[i][i];
    }
    return 0;
}

/* Recorre los paquetes y decodifica todo el audio como lo hace el lector de
 * audio del reproductor: un flujo continuo, independiente del video. */
static int8_t *extract_audio(const uint8_t *data, size_t len, uint32_t nframes,
                             int palwords, int afmt, size_t *nout)
{
    const uint8_t *p = data + A5V_HEADER_SIZE, *end = data + len;
    int8_t *out = NULL;
    size_t n = 0, cap = 0;
    int acc = 0;
    uint32_t i;

    for (i = 0; i < nframes; i++) {
        unsigned plen, alen;
        const uint8_t *a;
        size_t ns;

        if (end - p < 6) die("el bitstream se corta antes de tiempo");
        plen = be16(p);
        alen = be16(p + 4);
        if (plen < 6 || (size_t)(end - p) < plen) die("paquete invalido");
        a = p + 6 + ((p[3] & A5V_F_PALETTE) ? palwords * 2 : 0);
        if (a + alen > p + plen) die("audio incompleto");
        ns = afmt == A5V_AUDIO_FIB4 ? (size_t)alen * 2 : alen;
        if (n + ns > cap) {
            cap = (n + ns) * 2 + 4096;
            out = realloc(out, cap);
            if (!out) die("sin memoria");
        }
        if (afmt == A5V_AUDIO_FIB4) a5_fib4_decode(a, alen, &acc, out + n);
        else                        memcpy(out + n, a, alen);
        n += ns;
        p += plen;
    }
    *nout = n;
    return out;
}

/* WAV de 16 bits mono, para mezclar el audio decodificado en el preview. */
static void put_le(FILE *f, uint32_t v, int bytes)
{
    while (bytes--) { fputc((int)(v & 0xFF), f); v >>= 8; }
}

static void write_wav(const char *path, const int8_t *s, size_t n, int rate)
{
    FILE *f = fopen(path, "wb");
    uint32_t datalen = (uint32_t)n * 2;
    size_t i;

    if (!f) die("no pude escribir el WAV del preview");
    fwrite("RIFF", 1, 4, f); put_le(f, 36 + datalen, 4);
    fwrite("WAVEfmt ", 1, 8, f);
    put_le(f, 16, 4); put_le(f, 1, 2); put_le(f, 1, 2);
    put_le(f, (uint32_t)rate, 4); put_le(f, (uint32_t)rate * 2, 4);
    put_le(f, 2, 2); put_le(f, 16, 2);
    fwrite("data", 1, 4, f); put_le(f, datalen, 4);
    for (i = 0; i < n; i++) put_le(f, (uint16_t)(int16_t)(s[i] * 256), 2);
    fclose(f);
}

#define MEAS_AUD_SAMPLES 512      /* muestras por buffer de Paula (player.s) */
#define MEAS_CCK_FRAME   (A5_PAL_LINES * A5_CCK_PER_LINE)

static double stamp_cck(const uint8_t *s)
{
    return (double)be32(s) * MEAS_CCK_FRAME + be16(s + 4) * A5_CCK_PER_LINE
         + be16(s + 6);
}

static void report_measure(const uint8_t *adf, const MeasPoint *pt, int np,
                           int planes, int aper, size_t asamples,
                           uint32_t nframes)
{
    const uint8_t *in = adf + MEAS_INFO_SECTOR * 512;
    uint32_t bytes = be32(in + 12), tod = be32(in + 8);
    double load_s = tod / A5_VBL_HZ;
    double A[3][3] = { { 0 } }, b[3] = { 0 }, x[3] = { 0 };
    double sum = 0, maxm = 0, sse = 0, sst = 0, mean, maxres = 0;
    double ratio = 0, worst_model = 0, ysum = 0, ymean;
    double fill_cpu = 0, a_t0 = 0, a_per = 1;  /* audio medido */
    uint32_t a_cnt = 0;
    long fillc = a5_audio_fill_cost((int)be32(in + 100));  /* del modelo */
    double *y = NULL;        /* medido menos los llenados de audio */
    int *fills = NULL;       /* llenados de audio dentro de cada delta */
    int i, j, k, maxn = -1;

    printf("\n--- mediciones de la Amiga ---\n");
    printf("carga      : %lu bytes en %.1f s = %.1f KB/s (%lu VSYNC)\n",
           (unsigned long)bytes, load_s,
           load_s > 0 ? bytes / 1024.0 / load_s : 0, (unsigned long)tod);
    printf("memoria    : bloque 1 (slow) en $%08lX con %lu bytes, "
           "bloque 2 (Chip) en $%08lX con %lu bytes\n",
           (unsigned long)be32(in + 16), (unsigned long)be32(in + 20),
           (unsigned long)be32(in + 24), (unsigned long)be32(in + 28));
    printf("reproduccion: %lu VBL para %lu esperados; %lu frames tarde, el "
           "peor por %lu VBL\n",
           (unsigned long)be32(in + 40), (unsigned long)be32(in + 44),
           (unsigned long)be32(in + 32), (unsigned long)be32(in + 36));
    if (be32(in + 72))
        printf("AVISO: la tabla de tiempos se grabo con error %lu\n",
               (unsigned long)be32(in + 72));

    /* Sincronia: cada interrupcion de audio es el principio de un buffer de
     * 512 muestras. La primera es la muestra 0; de ahi sale cuando tendria
     * que terminar el audio, y se compara con cuando termina el video. */
    if (be32(in + 100) && be32(in + 76) >= 2) {
        uint32_t cnt = be32(in + 76);
        double t0 = stamp_cck(in + 80), t1 = stamp_cck(in + 88);
        double vstart = (double)be32(in + 96) * MEAS_CCK_FRAME;
        double per = (double)(aper ? aper : (int)be32(in + 104));
        double want = MEAS_AUD_SAMPLES * per;
        double got = (t1 - t0) / (cnt - 1);
        double drift = (t1 - t0) - (cnt - 1) * want;
        double aend = t0 + asamples * per;
        double vend = vstart + 2.0 * nframes * MEAS_CCK_FRAME;

        printf("audio      : %lu buffers; arranco %.2f ms despues del VBL "
               "del frame 0\n", (unsigned long)cnt,
               (t0 - vstart) * 1000 / A5_CCK_PAL);
        printf("             un buffer cada %.1f color clocks (esperado "
               "%.0f): deriva de %.3f ms en %.1f s\n", got, want,
               drift * 1000 / A5_CCK_PAL, (t1 - t0) / A5_CCK_PAL);
        printf("             el audio termina %.2f ms %s que el video\n",
               fabs(aend - vend) * 1000 / A5_CCK_PAL,
               aend >= vend ? "despues" : "antes");
        printf("             %.0f buffers caben entre el primero y el final "
               "del video\n", floor((vend - t0) / want) + 1);
        a_cnt = cnt; a_t0 = t0; a_per = want;
        if (be32(in + 108)) {
            double mean_cck = (double)be32(in + 108) / cnt;
            fill_cpu = 2.0 * mean_cck;
            printf("             llenar un buffer: %.3f ms de media "
                   "(%.0f ciclos), %.3f ms el peor; %.1f%% de la CPU\n",
                   mean_cck * 1000 / A5_CCK_PAL, fill_cpu,
                   be32(in + 112) * 1000.0 / A5_CCK_PAL,
                   100 * mean_cck / want);
            /* Un llenado de mas de un frame es una estampa rota (una resta
             * negativa): entonces la suma, y la media, tampoco sirven. */
            if (be32(in + 112) > MEAS_CCK_FRAME)
                printf("  AVISO: el peor llenado es imposible: hay una "
                       "estampa rota y la media no es confiable\n");
        }
    }

    /* H12: cuanto tarda el Blitter en copiar el area activa de un
     * framebuffer al otro, que es lo que costaria predecir el delta desde
     * el buffer visible. Medido con la imagen en pantalla, o sea con la
     * contencion de DMA real, y con la CPU esperando al Blitter. */
    if (be32(in + 132)) {
        uint32_t it = be32(in + 132);
        double m  = (double)be32(in + 116) / it;
        double mn = (double)be32(in + 124) / it;

        printf("blit (H12) : copiar el area activa, %u veces\n", (unsigned)it);
        printf("             normal: %.3f ms de media, %.3f el peor "
               "(%.0f ciclos de CPU)\n", m * 1000 / A5_CCK_PAL,
               be32(in + 120) * 1000.0 / A5_CCK_PAL, 2.0 * m);
        printf("             nasty : %.3f ms de media, %.3f el peor "
               "(%.0f ciclos de CPU)\n", mn * 1000 / A5_CCK_PAL,
               be32(in + 128) * 1000.0 / A5_CCK_PAL, 2.0 * mn);
    }
    if (np < 3) {
        printf("decodif.   : muy pocos tiempos para ajustar el modelo (%d)\n",
               np);
        return;
    }

    /* Cuantos llenados de audio cayeron dentro de cada delta. Se reconstruye
     * la linea de tiempo del reproductor con los tiempos medidos: el primer
     * delta empieza 2 VBL antes del frame 0, y cada uno de los siguientes en
     * el VBL en que se intercambio el anterior (el que le tocaba, o el
     * primero despues de terminar si llego tarde). Contar los frames tarde
     * de la reconstruccion y compararlos con los de la Amiga la valida. */
    y = malloc((size_t)np * sizeof *y);
    fills = calloc((size_t)np, sizeof *fills);
    if (!y || !fills) die("sin memoria");
    {
        double F = MEAS_CCK_FRAME, vs = (double)be32(in + 96);
        double start = (vs - 2) * F;
        int late = 0, nf = 0;

        for (i = 0; i < np; i++) {
            double due = vs + 2.0 * pt[i].n;           /* VBL en que se ve */
            double end = start + pt[i].cycles / 2;     /* en color clocks */
            double sw = ceil(end / F);
            if (sw < due) sw = due;
            if (sw > due) late++;
            if (fill_cpu > 0) {
                double k0 = ceil((start - a_t0) / a_per);
                double k1 = ceil((end - a_t0) / a_per) - 1;
                if (k0 < 0) k0 = 0;
                if (k1 > a_cnt - 1.0) k1 = a_cnt - 1.0;
                fills[i] = k1 >= k0 ? (int)(k1 - k0 + 1) : 0;
                nf += fills[i];
            }
            y[i] = pt[i].cycles - fills[i] * fill_cpu;
            start = sw * F;
            /* Si al intercambiar habia un llenado en curso, el lazo
             * principal no vuelve hasta que termina: el VBL lo interrumpe,
             * pero despues sigue el llenado. */
            if (fill_cpu > 0 && start > a_t0) {
                double kk = floor((start - a_t0) / a_per);
                double fend = a_t0 + kk * a_per + fill_cpu / 2;
                if (kk < a_cnt && fend > start) start = fend;
            }
        }
        printf("linea de tiempo: reconstruida con lo medido, %d frames tarde "
               "(la Amiga conto %lu)\n", late, (unsigned long)be32(in + 32));
        if (fill_cpu > 0)
            printf("             %d llenados de audio cayeron dentro de "
                   "deltas; el ajuste los descuenta\n", nf);
    }

    for (i = 0; i < np; i++) {
        double f[3];
        A5DeltaStats s;
        f[0] = 1; f[1] = pt[i].rows; f[2] = pt[i].cols;
        for (j = 0; j < 3; j++) {
            for (k = 0; k < 3; k++) A[j][k] += f[j] * f[k];
            b[j] += f[j] * y[i];
        }
        sum += pt[i].cycles;
        ysum += y[i];
        if (pt[i].cycles > maxm) { maxm = pt[i].cycles; maxn = i; }
        s.rows = pt[i].rows; s.cols = pt[i].cols; s.bytes = pt[i].bytes;
        s.cycles = 0;
        ratio += (a5_delta_cost(&s) + fills[i] * fillc) / pt[i].cycles;
    }
    mean = sum / np;
    ymean = ysum / np;
    ratio /= np;

    if (solve3(A, b, x) != 0) {
        printf("decodif.   : el ajuste no tiene solucion (datos degenerados)\n");
        free(y); free(fills);
        return;
    }
    for (i = 0; i < np; i++) {
        double pred = x[0] + x[1] * pt[i].rows + x[2] * pt[i].cols;
        double r = y[i] - pred;
        sse += r * r;
        sst += (y[i] - ymean) * (y[i] - ymean);
        if (fabs(r) > maxres) maxres = fabs(r);
    }
    {
        A5DeltaStats s;
        s.rows = pt[maxn].rows; s.cols = pt[maxn].cols;
        s.bytes = pt[maxn].bytes; s.cycles = 0;
        worst_model = (double)a5_delta_cost(&s) + fills[maxn] * fillc;
    }

    printf("decodif.   : %d deltas medidos, media %.2f ms, peor %.2f ms "
           "(frame %d: %d filas, %d columnas, %d llenados de audio)\n",
           np, mean * 1000 / A5_CPU_HZ, maxm * 1000 / A5_CPU_HZ,
           pt[maxn].n, pt[maxn].rows, pt[maxn].cols, fills[maxn]);
    printf("modelo     : el actual (con %ld ciclos por llenado) predice en "
           "promedio el %.1f%% de lo medido; para el peor frame, %.2f ms\n",
           fillc, ratio * 100, worst_model * 1000 / A5_CPU_HZ);
    printf("ajuste     : ciclos = %.0f + %.1f x filas + %.1f x columnas  "
           "(R2 = %.4f)%s\n", x[0], x[1], x[2], sst > 0 ? 1 - sse / sst : 0,
           fill_cpu > 0 ? ", sin los llenados de audio" : "");
    printf("             residuo maximo %.3f ms\n", maxres * 1000 / A5_CPU_HZ);
    printf("             con %d planos: %.1f ciclos por byte literal si se "
           "le carga todo el costo de la columna\n", planes, x[2] / planes);
    free(y); free(fills);
}

/* --- el disco de prueba del Hito 3 (player\still.s) ----------------------
 * Vuelca el framebuffer, el copper list y un sector de informacion (formato
 * en FORMAT.md). --check-still los compara byte por byte con lo que tendria
 * que haber armado el reproductor. */
#define STILL_FB_SECTOR    1680
#define STILL_COP_SECTOR   1720
#define STILL_INFO_SECTOR  1759
#define STILL_COP_MAX      8192     /* COPPER_SIZE de player\video.i */
#define DIW_FIRST          0x2c     /* primera linea de pantalla */

/* El copper list que arman build_copper + write_palette (video_code.i),
 * con la misma receta instruccion por instruccion. Devuelve su largo. */
static size_t ref_copper(uint8_t *o, uint32_t fb, int planes, int brows,
                         int y0, int y1, const A5Color *pal)
{
    int n = 1 << planes, nb = a5v_nbands(brows, y0, y1), b = 1, p, c, d;
    int next = nb > 1 ? DIW_FIRST + 2 * (y0 + brows) : 0x7fff;
    size_t k = 0;

#define P16(v) (o[k] = (uint8_t)((unsigned)(v) >> 8), \
                o[k + 1] = (uint8_t)(v), k += 2)
    P16(0x008e); P16(0x2c81); P16(0x0090); P16(0x2cc1);   /* DIWSTRT/STOP */
    P16(0x0092); P16(0x0038); P16(0x0094); P16(0x00d0);   /* DDFSTRT/STOP */
    P16(0x0102); P16(0);      P16(0x0104); P16(0);        /* BPLCON1/2 */
    P16(0x0108); P16(0xffd8); P16(0x010a); P16(0xffd8);   /* modulos -40 */
    for (p = 0; p < planes; p++) {
        uint32_t a = fb + (uint32_t)p * A5_H * 2 * A5_ROWBYTES;
        P16(0xe0 + 4 * p); P16(a >> 16);
        P16(0xe2 + 4 * p); P16(a & 0xffff);
    }
    for (c = 0; c < n; c++) { P16(0x180 + 2 * c); P16(pal[c]); }
    P16(0x0100); P16((planes << 12) | 0x0200);            /* BPLCON0 */
    for (d = DIW_FIRST; d < DIW_FIRST + 2 * A5_H; d++) {
        unsigned mod = (d & 1) ? 0 : 0xffd8;
        if (d == 0x100) { P16(0xffdf); P16(0xfffe); }     /* linea 256 */
        P16(((d & 0xff) << 8) | 0x07); P16(0xfffe);       /* WAIT hpos $06 */
        if (d == next) {                                   /* franja nueva */
            for (c = 1; c < n; c++) {
                P16(0x180 + 2 * c); P16(pal[b * n + c]);
            }
            b++;
            next = b < nb ? next + 2 * brows : 0x7fff;
        }
        P16(0x0108); P16(mod); P16(0x010a); P16(mod);
    }
    P16(0xffff); P16(0xfffe);
#undef P16
    return k;
}

/* Compara el volcado de still.s con el frame decodificado. 1 si coincide. */
static int check_still(const char *path, const uint8_t *fb, int planes,
                       int brows, int y0, int y1, const A5Color *pal,
                       long delta_used)
{
    static uint8_t cop[STILL_COP_MAX + 64];
    size_t dl = 0, cl, k;
    uint8_t *d = slurp(path, &dl);
    const uint8_t *info, *fbd, *copd;
    long fbad = 0, ffirst = -1, cbad = 0, cfirst = -1;
    int p, y, b, ok;

    if (!d || dl < (STILL_INFO_SECTOR + 1) * 512u)
        die("no pude leer el disco del frame fijo");
    info = d + STILL_INFO_SECTOR * 512;
    if (memcmp(info, "FBDM", 4)) {
        printf("\nEl reproductor no grabo el volcado. Mira la captura: el "
               "color del borde dice donde se trabo.\n");
        free(d);
        return 0;
    }
    fbd = d + STILL_FB_SECTOR * 512;
    copd = d + STILL_COP_SECTOR * 512;

    for (p = 0; p < planes; p++)
        for (y = 0; y < A5_H; y++)
            for (b = 0; b < A5_ROWBYTES; b++) {
                uint16_t w = a5_double_byte(
                    fb[((size_t)p * A5_H + y) * A5_ROWBYTES + b]);
                size_t o = ((size_t)p * A5_H + y) * A5_ROWBYTES * 2 + b * 2;
                if (fbd[o] != (w >> 8) || fbd[o + 1] != (w & 0xff)) {
                    if (ffirst < 0) ffirst = (long)o;
                    fbad++;
                }
            }

    cl = ref_copper(cop, be32(info + 8), planes, brows, y0, y1, pal);
    for (k = 0; k < cl; k++)
        if (copd[k] != cop[k]) {
            if (cfirst < 0) cfirst = (long)k;
            cbad++;
        }

    printf("\n--- volcado del frame fijo ---\n");
    printf("reproductor: %lu planos, framebuffer en $%08lX, copper en "
           "$%08lX\n", (unsigned long)be32(info + 4),
           (unsigned long)be32(info + 8), (unsigned long)be32(info + 12));
    printf("delta      : consumio %lu bytes (esperado %ld) -> %s\n",
           (unsigned long)be32(info + 16), delta_used,
           (long)be32(info + 16) == delta_used ? "OK" : "MAL");
    if (be32(info + 20) || be32(info + 24))
        printf("AVISO: trackdisk devolvio error al volcar (%lu, %lu)\n",
               (unsigned long)be32(info + 20),
               (unsigned long)be32(info + 24));
    if (fbad)
        printf("framebuffer: %ld bytes distintos, el primero en el %ld "
               "(plano %ld, fila %ld)\n", fbad, ffirst,
               ffirst / (A5_H * A5_ROWBYTES * 2),
               ffirst % (A5_H * A5_ROWBYTES * 2) / (A5_ROWBYTES * 2));
    else
        printf("framebuffer: %d bytes, identico al decoder de referencia "
               "-> OK\n", planes * A5_H * A5_ROWBYTES * 2);
    if (cbad)
        printf("copper list: %ld bytes distintos de %lu, el primero en el "
               "%ld (instruccion %ld)\n", cbad, (unsigned long)cl, cfirst,
               cfirst / 4);
    else
        printf("copper list: %lu bytes, %d franjas, identico a la "
               "referencia -> OK\n", (unsigned long)cl,
               a5v_nbands(brows, y0, y1));

    ok = be32(info + 4) == (uint32_t)planes && !fbad && !cbad &&
         (long)be32(info + 16) == delta_used &&
         !be32(info + 20) && !be32(info + 24);
    free(d);
    return ok;
}

/* Exporta el frame visible como un bitstream de un solo paquete: un DELTA
 * desde negro con su paleta. Es lo que muestra el reproductor del Hito 3.
 * Escribe ademas lo que el reproductor tiene que dejar en su framebuffer
 * (<out>.fb: planos doblados a 40 bytes por fila, plano 0 primero) y una
 * imagen de referencia de 320x256 (<out>.ppm). */
static void write_still(const char *out, int planes, int ncolors, int y0,
                        int y1, int brows, const uint8_t *idx,
                        const A5Color *pal)
{
    int nb = a5v_nbands(brows, y0, y1);
    size_t fsz = (size_t)A5_W * A5_H, plen, raw;
    uint8_t *black = calloc(fsz, 1);
    uint8_t planar[A5_MAX_PLANES * A5_ROWBYTES];
    A5Buf delta, file;
    A5DeltaStats ds;
    char path[1024];
    FILE *f;
    int c, p, y, b;

    if (!black) die("sin memoria");
    a5buf_init(&delta);
    a5buf_init(&file);
    a5_delta_encode(&delta, black, idx, planes, &ds);

    raw = 6 + (size_t)nb * ncolors * 2 + delta.len;
    plen = (raw + 1) & ~(size_t)1;
    a5v_put_header(&file, planes, ncolors, A5V_AUDIO_NONE, 0, y0, y1, brows,
                   1, (uint32_t)plen);
    a5buf_put16(&file, (unsigned)plen);
    a5buf_put8(&file, A5V_OP_DELTA);
    a5buf_put8(&file, A5V_F_PALETTE);
    a5buf_put16(&file, 0);
    for (c = 0; c < nb * ncolors; c++) a5buf_put16(&file, pal[c]);
    a5buf_write(&file, delta.p, delta.len);
    if (raw != plen) a5buf_put8(&file, 0);

    f = fopen(out, "wb");
    if (!f || fwrite(file.p, 1, file.len, f) != file.len) die("no pude escribir el frame fijo");
    fclose(f);

    snprintf(path, sizeof path, "%s.fb", out);
    f = fopen(path, "wb");
    if (!f) die("no pude escribir el framebuffer esperado");
    for (p = 0; p < planes; p++) {
        for (y = 0; y < A5_H; y++) {
            a5_planarize_row(idx + (size_t)y * A5_W, planes, planar);
            for (b = 0; b < A5_ROWBYTES; b++) {
                uint16_t w = a5_double_byte(planar[p * A5_ROWBYTES + b]);
                fputc(w >> 8, f);
                fputc(w & 0xFF, f);
            }
        }
    }
    fclose(f);

    snprintf(path, sizeof path, "%s.ppm", out);
    f = fopen(path, "wb");
    if (!f) die("no pude escribir la imagen de referencia");
    fprintf(f, "P6\n%d %d\n255\n", A5_DISP_W, A5_DISP_H);
    for (y = 0; y < A5_DISP_H; y++) {
        int x;
        for (x = 0; x < A5_DISP_W; x++) {
            uint8_t r, g, bl;
            a5_rgb444_to_srgb(pal[a5v_band_of(y / 2, brows, y0, nb) * ncolors
                                  + idx[(y / 2) * A5_W + x / 2]], &r, &g, &bl);
            fputc(r, f); fputc(g, f); fputc(bl, f);
        }
    }
    fclose(f);

    printf("frame fijo : %s (%lu bytes; delta de %lu bytes, %d filas, "
           "%d columnas)\n", out, (unsigned long)file.len,
           (unsigned long)delta.len, ds.rows, ds.cols);
    printf("             %s.fb (framebuffer esperado), %s.ppm (referencia)\n",
           out, out);

    a5buf_free(&delta);
    a5buf_free(&file);
    free(black);
}

int main(int argc, char **argv)
{
    const char *in = NULL, *preview = NULL, *audio_src = NULL;
    double audio_start = 0, audio_dur = 0;
    int pscale = 2, i;
    long still_k = -1;
    const char *still_out = NULL;
    const char *measure = NULL;
    const char *still_check = NULL;
    long delta_used = -1;              /* bytes que consumio el ultimo delta */
    uint8_t *adf = NULL;
    MeasPoint *mpts = NULL;
    int nmp = 0;

    uint8_t *data, *crcdata = NULL;
    size_t len, crclen = 0;
    const uint8_t *p, *end;
    int planes, ncolors, w, h, y0, y1, afmt, aper;
    int brows, nbands, npalw;          /* franjas de paleta */
    int8_t *asamples = NULL;
    size_t nasamples = 0;
    char wavpath[1024];
    uint32_t nframes;

    uint8_t *fb[2], *idxbuf;
    static A5Color pal[A5_MAX_BANDS * A5_MAX_COLORS];  /* franja por franja */
    int visible = 0;
    size_t fbsize, fsz;
    FILE *pre = NULL;
    uint8_t *row = NULL;

    long total_bytes = 0, maxcycles = 0;
    long total_rows = 0, total_cols = 0;
    int nrepeat = 0, ndelta = 0, npal = 0, bad = 0, maxcyc_frame = 0;
    uint32_t n;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        int has = i + 1 < argc;
        if (!strcmp(a, "--in") && has)                in = argv[++i];
        else if (!strcmp(a, "--preview") && has)      preview = argv[++i];
        else if (!strcmp(a, "--audio") && has)        audio_src = argv[++i];
        else if (!strcmp(a, "--audio-start") && has)  audio_start = atof(argv[++i]);
        else if (!strcmp(a, "--audio-duration") && has) audio_dur = atof(argv[++i]);
        else if (!strcmp(a, "--preview-scale") && has) pscale = atoi(argv[++i]);
        else if (!strcmp(a, "--still") && has)        still_k = atol(argv[++i]);
        else if (!strcmp(a, "--still-out") && has)    still_out = argv[++i];
        else if (!strcmp(a, "--measure") && has)      measure = argv[++i];
        else if (!strcmp(a, "--check-still") && has)  still_check = argv[++i];
        else {
            printf("uso: a500vp-dec --in <video.a5v> [--preview <out.mp4>]\n"
                   "                [--audio <fuente>] [--audio-start S]\n"
                   "                [--audio-duration S] [--preview-scale N]\n"
                   "                [--still K --still-out <frame.a5v>]\n"
                   "                [--measure <disco_de_medicion.adf>]\n"
                   "                [--check-still <disco_del_frame_fijo.adf>]\n");
            return 2;
        }
    }
    if (!in) { fprintf(stderr, "falta --in\n"); return 2; }

    data = slurp(in, &len);
    if (!data) die("no pude leer el bitstream");
    if (len < A5V_HEADER_SIZE || memcmp(data, A5V_MAGIC, 4))
        die("no es un archivo A5VP");
    if (be16(data + 4) != A5V_VERSION)
        die("version de formato distinta");

    w        = (int)be16(data + 8);
    h        = (int)be16(data + 10);
    planes   = data[12];
    ncolors  = data[13];
    y0       = (int)be16(data + 16);
    y1       = (int)be16(data + 18);
    nframes  = be32(data + 20);
    afmt     = data[A5V_HDR_AUDIOFMT];
    aper     = (int)be16(data + 14);
    if (afmt > A5V_AUDIO_PCM8) die("formato de audio desconocido");
    if (afmt && aper < 124) die("periodo de audio invalido");

    if (w != A5_W || h != A5_H) die("geometria inesperada");
    if (planes < 1 || planes > A5_MAX_PLANES || ncolors != 1 << planes)
        die("planos o colores invalidos");
    if (y0 < 0 || y1 > A5_H || y0 >= y1) die("filas activas invalidas");

    brows  = data[A5V_HDR_BANDROWS];
    nbands = a5v_nbands(brows, y0, y1);
    npalw  = nbands * ncolors;
    if (nbands > A5_MAX_BANDS) die("demasiadas franjas");
    if (nbands > 1 && planes > 3)
        die("franjas con mas de 3 planos: el Copper no llega (FORMAT.md)");

    printf("bitstream  : %s (%lu bytes)\n", in, (unsigned long)len);
    printf("             %dx%d, %d planos, %d colores, filas activas %d..%d\n",
           w, h, planes, ncolors, y0, y1 - 1);
    if (nbands > 1)
        printf("             paleta en %d franjas de %d filas\n", nbands,
               brows);
    printf("             %lu frames, %.3f s\n", (unsigned long)nframes,
           nframes / A5_VIDEO_FPS);

    if (afmt) {
        asamples = extract_audio(data, len, nframes, npalw, afmt,
                                 &nasamples);
        printf("             audio %s, periodo %d = %.3f Hz, %lu muestras "
               "(%.3f s)\n", afmt == A5V_AUDIO_FIB4 ? "fib4" : "pcm8", aper,
               A5_CCK_PAL / aper, (unsigned long)nasamples,
               nasamples / (A5_CCK_PAL / aper));
    } else {
        printf("             sin audio\n");
    }

    {
        char crcpath[1024];
        snprintf(crcpath, sizeof crcpath, "%s.crc", in);
        crcdata = slurp(crcpath, &crclen);
        if (crcdata && crclen < nframes * 4u) {
            free(crcdata); crcdata = NULL;
            printf("             (el archivo .crc esta corto, se ignora)\n");
        }
    }

    if (measure) {
        size_t adflen = 0;
        adf = slurp(measure, &adflen);
        if (!adf || adflen < (MEAS_INFO_SECTOR + 1) * 512u)
            die("no pude leer el disco de medicion");
        if (memcmp(adf + MEAS_INFO_SECTOR * 512, "PLAY", 4)) {
            fprintf(stderr, "error: %s no tiene mediciones (el reproductor "
                    "no llego a grabarlas)\n", measure);
            return 1;
        }
        mpts = malloc((size_t)nframes * sizeof *mpts);
        if (!mpts) die("sin memoria");
    }

    fsz    = (size_t)A5_W * A5_H;
    fbsize = (size_t)planes * A5_H * A5_ROWBYTES;
    fb[0] = calloc(fbsize, 1);
    fb[1] = calloc(fbsize, 1);
    idxbuf = malloc(fsz);
    if (!fb[0] || !fb[1] || !idxbuf) die("sin memoria");
    memset(pal, 0, sizeof pal);

    if (preview) {
        int arate = 0;
        row = malloc((size_t)A5_DISP_W * 3);
        if (audio_src) {
            arate = 44100;                 /* la fuente, tal cual */
        } else if (asamples) {
            /* El audio del bitstream, decodificado: lo que va a sonar. El
             * WAV lleva frecuencia entera y Paula toca a 3546895/periodo;
             * para el preview la diferencia (0,006% con 443) no importa. */
            arate = (int)(A5_CCK_PAL / aper + 0.5);
            snprintf(wavpath, sizeof wavpath, "%s.wav", preview);
            write_wav(wavpath, asamples, nasamples, arate);
            audio_src = wavpath;
            audio_start = audio_dur = 0;
        }
        pre = a5_open_preview(preview, A5_DISP_W, A5_DISP_H, A5_VIDEO_FPS,
                              pscale, audio_src, audio_start, audio_dur,
                              1.0, arate);
        if (!pre || !row) die("no pude arrancar ffmpeg para el preview");
    }

    p = data + A5V_HEADER_SIZE;
    end = data + len;

    for (n = 0; n < nframes; n++) {
        unsigned plen, op, flags, alen;
        const uint8_t *pk, *pkend;

        if (end - p < 6) die("el bitstream se corta antes de tiempo");
        plen  = be16(p);
        op    = p[2];
        flags = p[3];
        alen  = be16(p + 4);
        if (plen < 6 || (size_t)(end - p) < plen) die("paquete invalido");
        pk = p + 6;
        pkend = p + plen;

        if (flags & A5V_F_PALETTE) {
            int c;
            if (pkend - pk < npalw * 2) die("paleta incompleta");
            for (c = 0; c < npalw; c++) pal[c] = (A5Color)be16(pk + c * 2);
            for (c = 1; c < nbands; c++)
                if (pal[c * ncolors] != pal[0])
                    die("el color 0 cambia entre franjas (FORMAT.md)");
            pk += npalw * 2;
            npal++;
        }
        pk += alen;                     /* el audio llega en el Hito 5 */
        if (pk > pkend) die("audio incompleto");

        if (op == A5V_OP_DELTA) {
            /* El delta se dibuja en el buffer oculto y despues se intercambia:
             * exactamente lo que hace el reproductor escribiendo COP1LC en el
             * vertical blank. */
            A5DeltaStats ds;
            const uint8_t *q = a5_delta_apply(fb[visible ^ 1], pk, pkend,
                                              planes, &ds);
            if (!q) die("delta corrupto");
            delta_used = (long)(q - pk);
            visible ^= 1;
            ndelta++;
            total_bytes += ds.bytes;
            total_rows += ds.rows;
            total_cols += ds.cols;
            if (adf && n < MEAS_TIMING_MAX) {
                uint32_t cck = be32(adf + MEAS_TIMING_SECTOR * 512 + n * 4);
                if (cck) {                /* CPU = 2 x reloj de color */
                    mpts[nmp].n = (int)n;
                    mpts[nmp].rows = ds.rows;
                    mpts[nmp].cols = ds.cols;
                    mpts[nmp].bytes = ds.bytes;
                    mpts[nmp].cycles = 2.0 * cck;
                    nmp++;
                }
            }
            if (ds.cycles > maxcycles) { maxcycles = ds.cycles;
                                         maxcyc_frame = (int)n; }
        } else if (op == A5V_OP_REPEAT) {
            nrepeat++;                  /* no se toca nada: se ve lo mismo */
        } else {
            die("op desconocido");
        }

        /* --- lo que se ve --- */
        {
            int y;
            for (y = 0; y < A5_H; y++) {
                uint8_t planar[A5_MAX_COLORS * A5_ROWBYTES];
                int pi, x;
                for (pi = 0; pi < planes; pi++)
                    memcpy(planar + pi * A5_ROWBYTES,
                           fb[visible] + ((size_t)pi * A5_H + y) * A5_ROWBYTES,
                           A5_ROWBYTES);
                a5_depack_row(planar, planes, idxbuf + (size_t)y * A5_W);
                if (!pre) continue;
                for (x = 0; x < A5_W; x++) {
                    uint8_t r, g, b;
                    a5_rgb444_to_srgb(pal[a5v_band_of(y, brows, y0, nbands)
                                          * ncolors + idxbuf[y * A5_W + x]],
                                      &r, &g, &b);
                    row[x * 6 + 0] = r; row[x * 6 + 1] = g; row[x * 6 + 2] = b;
                    row[x * 6 + 3] = r; row[x * 6 + 4] = g; row[x * 6 + 5] = b;
                }
                fwrite(row, 1, (size_t)A5_DISP_W * 3, pre);
                fwrite(row, 1, (size_t)A5_DISP_W * 3, pre);
            }
        }

        if (crcdata) {
            uint32_t want = be32(crcdata + n * 4);
            uint32_t got  = frame_crc(idxbuf, fsz, pal, npalw);
            if (want != got) {
                if (bad < 5)
                    fprintf(stderr, "frame %lu: crc %08lX, el encoder decia "
                            "%08lX\n", (unsigned long)n,
                            (unsigned long)got, (unsigned long)want);
                bad++;
            }
        }

        if (still_k >= 0 && (long)n == still_k) {
            if (!still_out) die("--still necesita --still-out");
            write_still(still_out, planes, ncolors, y0, y1, brows, idxbuf,
                        pal);
        }

        p = pkend;
    }

    if (pre) { a5_pclose(pre); free(row); }

    if (still_check &&
        !check_still(still_check, fb[visible], planes, brows, y0, y1, pal,
                     delta_used))
        return 1;

    printf("\npaquetes   : %lu (%d delta, %d repeticion, %d paletas)\n",
           (unsigned long)nframes, ndelta, nrepeat, npal);
    printf("filas      : %ld modificadas, %.1f columnas por fila\n",
           total_rows, total_rows ? (double)total_cols / total_rows : 0);
    printf("donde van los bytes:\n");
    printf("  cabecera de paquete  %8ld  (%4.1f%%)\n",
           (long)nframes * 6, 100.0 * nframes * 6 / len);
    printf("  paletas              %8ld  (%4.1f%%)\n",
           (long)npal * npalw * 2, 100.0 * npal * npalw * 2 / len);
    printf("  mapa de filas        %8ld  (%4.1f%%)\n",
           (long)ndelta * A5_ROWMASK_SIZE,
           100.0 * ndelta * A5_ROWMASK_SIZE / len);
    printf("  mascara de columnas  %8ld  (%4.1f%%)\n",
           total_rows * A5_COLMASK_SIZE,
           100.0 * total_rows * A5_COLMASK_SIZE / len);
    printf("  datos literales      %8ld  (%4.1f%%)\n",
           total_bytes, 100.0 * total_bytes / len);
    printf("costo est. : peor frame %ld ciclos = %.1f ms (frame %d)\n",
           maxcycles, maxcycles * 1000.0 / A5_CPU_HZ, maxcyc_frame);
    if (p != end)
        printf("AVISO: sobraron %ld bytes al final del bitstream\n",
               (long)(end - p));
    if (preview)
        printf("preview    : %s\n", preview);
    if (adf)
        report_measure(adf, mpts, nmp, planes, aper, nasamples, nframes);

    if (crcdata && asamples && crclen >= nframes * 4u + 4) {
        uint32_t want = be32(crcdata + nframes * 4);
        uint32_t got = a5_crc32(asamples, nasamples, 0);
        if (want != got) {
            printf("\nVERIFICACION: FALLA. El audio decodificado no coincide "
                   "con el del encoder.\n");
            return 1;
        }
        printf("audio      : %lu muestras, identicas a las que simulo el "
               "encoder\n", (unsigned long)nasamples);
    }

    if (crcdata) {
        if (bad) {
            printf("\nVERIFICACION: FALLA. %d frames de %lu no coinciden con "
                   "el encoder.\n", bad, (unsigned long)nframes);
            return 1;
        }
        printf("\nVERIFICACION: OK. Los %lu frames coinciden exactamente con "
               "lo que simulo el encoder.\n", (unsigned long)nframes);
    } else {
        printf("\n(sin archivo .crc: no se verifico contra el encoder)\n");
    }

    free(fb[0]); free(fb[1]); free(idxbuf); free(data); free(crcdata);
    free(adf); free(mpts); free(asamples);
    return 0;
}
