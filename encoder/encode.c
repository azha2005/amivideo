/* encode.c - encoder de A500VP.
 *
 * Lee el video con ffmpeg, lo mete en el frame logico de 160x128 con la
 * relacion de aspecto elegida, detecta cortes de escena, cuantiza a una
 * paleta RGB444 por escena y arma el bitstream: delta contra el buffer
 * oculto, repeticiones, calidad con perdida y control de tasa.
 *
 * El preview de como se ve lo genera el decoder de referencia a partir del
 * bitstream, no este programa: asi el preview valida el formato.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "a500vp.h"
#include "stream.h"

/* Un pixel logico es 2x2 pixeles de pantalla, y la pantalla de 320x256 se ve
 * en un tubo 4:3. O sea que el pixel logico es 1,0667 veces mas ancho que
 * alto (la SAR 16:15 clasica de PAL lowres). */
#define A5_PAR   ((4.0 / 3.0) * (A5_DISP_H / (double)A5_DISP_W))

typedef enum { ASPECT_LETTERBOX, ASPECT_CROP, ASPECT_STRETCH } Aspect;

typedef struct {
    int    start, count;       /* primer frame y cantidad */
    A5Palette pal;
    size_t unique;
    double error;
} Scene;

static void die(const char *msg)
{
    fprintf(stderr, "error: %s\n", msg);
    exit(1);
}

static int even(int v) { return v & ~1; }

/* ====================================================================
 * Bitstream
 * ==================================================================== */

typedef struct {
    A5Buf  buf;             /* solo los paquetes, sin cabecera */
    int    npackets, nrepeat, ndelta, npalette;
    size_t maxpacket;
    int    maxpacket_frame;
    long   maxcycles;
    int    maxcycles_frame;
    int    over_budget;     /* frames que pasan el tope fijo (--frame-ms) */
    int    degraded;        /* frames degradados para no llegar tarde */
    int    late_frames;     /* frames que el reproductor va a mostrar tarde */
    int    max_late;        /* mayor atraso previsto, en VBL */
    int    max_late_frame;
    double err_sum;         /* error perceptual medio contra el frame ideal */
    long   bad_pixels;      /* pixeles activos con error visible, sumados */
    uint32_t *crc;          /* uno por frame, del buffer visible */
} A5Stream;

/* Checksum de un frame tal como se ve: los indices mas la paleta con la que
 * se muestran. El decoder de referencia recalcula esto y tiene que dar lo
 * mismo, byte por byte. */
static uint32_t frame_crc(const uint8_t *vis, size_t fsz,
                          const A5Palette *pal, int ncolors)
{
    uint8_t pb[A5_MAX_COLORS * 2];
    int c;

    for (c = 0; c < ncolors; c++) {
        pb[c * 2]     = (uint8_t)(pal->rgb444[c] >> 8);
        pb[c * 2 + 1] = (uint8_t)pal->rgb444[c];
    }
    return a5_crc32(pb, (size_t)ncolors * 2, a5_crc32(vis, fsz, 0));
}

/* Error perceptual medio entre dos mapas de indices, solo en el area activa. */
static double idx_error(const uint8_t *a, const uint8_t *b,
                        const A5Palette *pa, const A5Palette *pb,
                        int y0, int y1)
{
    size_t n = (size_t)(y1 - y0) * A5_W, i;
    const uint8_t *pa_ = a + (size_t)y0 * A5_W;
    const uint8_t *pb_ = b + (size_t)y0 * A5_W;
    double sum = 0;

    if (!n) return 0;
    for (i = 0; i < n; i++)
        if (pa_[i] != pb_[i])
            sum += sqrt((double)a5_oklab_dist2(pa->lab[pa_[i]],
                                               pb->lab[pb_[i]]));
    return sum / n;
}

/* Pixeles del area activa cuyo error se ve. El error medio esconde el
 * salpicado: 0,047 de media puede ser un 20% de pixeles con 0,2 de error.
 * 0,1 en Oklab es un cambio de color que se nota sin buscarlo. */
#define A5_VISIBLE_ERR  0.1

static long idx_bad(const uint8_t *a, const uint8_t *b, const A5Palette *pal,
                    int y0, int y1)
{
    size_t n = (size_t)(y1 - y0) * A5_W, i;
    const uint8_t *pa = a + (size_t)y0 * A5_W;
    const uint8_t *pb = b + (size_t)y0 * A5_W;
    long bad = 0;

    for (i = 0; i < n; i++)
        if (pa[i] != pb[i] &&
            a5_oklab_dist2(pal->lab[pa[i]], pal->lab[pb[i]]) >
                A5_VISIBLE_ERR * A5_VISIBLE_ERR)
            bad++;
    return bad;
}

/* Calidad con perdida: se deja sin actualizar todo pixel cuyo error, medido
 * contra el estado YA DECODIFICADO, quede por debajo del umbral. Despues, si
 * lo que queda de una fila es despreciable, se descarta la fila entera para
 * ahorrarse tambien su cabecera. */
static void apply_quality(uint8_t *target, const uint8_t *hidden,
                          const A5Palette *pal, int y0, int y1,
                          double pixel_thr, double row_thr)
{
    int y, x;

    if (pixel_thr <= 0) return;

    for (y = y0; y < y1; y++) {
        uint8_t *t = target + (size_t)y * A5_W;
        const uint8_t *h = hidden + (size_t)y * A5_W;
        double rowsum = 0;
        int changed = 0;

        for (x = 0; x < A5_W; x++) {
            double d;
            if (t[x] == h[x]) continue;
            d = sqrt((double)a5_oklab_dist2(pal->lab[t[x]], pal->lab[h[x]]));
            if (d < pixel_thr) { t[x] = h[x]; continue; }
            rowsum += d;
            changed = 1;
        }
        /* La fila se descarta entera solo si lo que queda es de verdad
         * despreciable. Se compara la SUMA de errores, no la suma dividida
         * por el ancho de la fila: dividir por 160 hacia parecer chico un
         * cambio grande concentrado en tres pixeles, y se tiraban filas con
         * detalle real. */
        if (changed && rowsum < row_thr * 4)
            memcpy(t, h, A5_W);
    }
}

/* VBL que llega tarde un delta que empieza a decodificarse en start y cuesta
 * cost ciclos, si le toca verse en due. due esta sobre la grilla de VBL. */
static int late_vbls(double start, long cost, double due, double P)
{
    double end = start + (double)cost;
    if (end <= due) return 0;
    return (int)ceil((end - due) / P);
}

static void put_packet(A5Stream *st, int op, const A5Palette *pal, int ncolors,
                       const A5Buf *video, const uint8_t *audio,
                       size_t audio_len)
{
    size_t start = st->buf.len;
    size_t len = 6 + (pal ? (size_t)ncolors * 2 : 0) + audio_len
               + (video ? video->len : 0);
    int c;

    if (len & 1) len++;                 /* los paquetes quedan pares */

    a5buf_put16(&st->buf, (unsigned)len);
    a5buf_put8(&st->buf, (unsigned)op);
    a5buf_put8(&st->buf, pal ? A5V_F_PALETTE : 0);
    a5buf_put16(&st->buf, (unsigned)audio_len);

    if (pal)
        for (c = 0; c < ncolors; c++)
            a5buf_put16(&st->buf, pal->rgb444[c]);
    if (audio_len)
        a5buf_write(&st->buf, audio, audio_len);
    if (video && video->len)
        a5buf_write(&st->buf, video->p, video->len);

    while (st->buf.len - start < len)
        a5buf_put8(&st->buf, 0);

    st->npackets++;
    if (len > st->maxpacket) {
        st->maxpacket = len;
        st->maxpacket_frame = st->npackets - 1;
    }
}

/* Arma el bitstream entero. idx tiene nframes mapas de A5_W*A5_H indices. */
static void build_stream(A5Stream *st, const uint8_t *idx, size_t nframes,
                         const Scene *scenes, int nscenes, int planes,
                         int ncolors, int y0, int y1,
                         double pixel_thr, double repeat_boost, long cyc_limit,
                         int min_hold, int max_late)
{
    size_t fsz = (size_t)A5_W * A5_H;
    uint8_t *vis = calloc(fsz, 1);
    uint8_t *hid = calloc(fsz, 1);
    uint8_t *tgt = malloc(fsz);
    A5Buf video;
    size_t n;
    int s = 0;
    const A5Palette *vispal = NULL;
    size_t last_delta = 0;
    /* Linea de tiempo del reproductor, en ciclos de CPU, con el origen en el
     * VBL del frame 0. El reproductor empieza a decodificar 2 VBL antes. */
    double P = A5_CYC_PER_VBL;
    double t_free = -2.0 * P;

    if (!vis || !hid || !tgt) die("sin memoria");
    memset(st, 0, sizeof *st);
    a5buf_init(&st->buf);
    a5buf_init(&video);
    st->crc = malloc(nframes * sizeof *st->crc);
    if (!st->crc) die("sin memoria");

    for (n = 0; n < nframes; n++) {
        const A5Palette *pal;
        const uint8_t *ideal = idx + n * fsz;
        int newscene;
        A5DeltaStats ds = {0, 0, 0, 0};
        double thr = pixel_thr;
        double due = 2.0 * (double)n * P;    /* VBL en que se ve este frame */
        int attempt, late = 0;

        while (s + 1 < nscenes && (int)n >= scenes[s + 1].start) s++;
        pal = &scenes[s].pal;
        newscene = ((int)n == scenes[s].start);

        /* Tope de cadencia: la imagen se actualiza como mucho cada min_hold
         * huecos. Un corte de escena siempre pasa. */
        if (!newscene && vispal == pal && min_hold > 1 &&
            n - last_delta < (size_t)min_hold) {
            put_packet(st, A5V_OP_REPEAT, NULL, ncolors, NULL, NULL, 0);
            st->nrepeat++;
            st->err_sum += idx_error(vis, ideal, pal, pal, y0, y1);
            st->bad_pixels += idx_bad(vis, ideal, pal, y0, y1);
            st->crc[n] = frame_crc(vis, fsz, pal, ncolors);
            continue;
        }

        /* Repeticion: mostrar de nuevo lo que ya esta en pantalla. No se
         * puede si cambia la paleta, porque la paleta viaja con el delta.
         *
         * El criterio es el mismo que el de la calidad con perdida, aplicado
         * contra el buffer VISIBLE: si a esta calidad no queda nada que valga
         * la pena actualizar, el frame se repite. Tenerlo como umbral aparte
         * y fijo era un error: al subir la perdida, el buffer visible se
         * alejaba del ideal y las repeticiones desaparecian justo cuando mas
         * falta hacian. */
        if (!newscene && vispal == pal) {
            memcpy(tgt, ideal, fsz);
            apply_quality(tgt, vis, pal, y0, y1, thr * repeat_boost,
                          thr * repeat_boost);
            if (!memcmp(tgt, vis, fsz)) {
                put_packet(st, A5V_OP_REPEAT, NULL, ncolors, NULL, NULL, 0);
                st->nrepeat++;
                st->err_sum += idx_error(vis, ideal, pal, pal, y0, y1);
                st->bad_pixels += idx_bad(vis, ideal, pal, y0, y1);
                st->crc[n] = frame_crc(vis, fsz, pal, ncolors);
                continue;
            }
        }

        /* Delta contra el buffer oculto, que es el penultimo frame distinto.
         *
         * Linea de tiempo del reproductor, medida en el Hito 4: el delta se
         * empieza a decodificar cuando el intercambio anterior libera el
         * buffer oculto (t_free) y tiene que estar listo para el VBL en que
         * le toca verse. Si no llega, se ve tarde y las repeticiones que
         * siguen absorben el atraso. Se degrada (subiendo el umbral) solo si
         * se pasaria de max_late VBL: un frame degradado deja salpicado, que
         * se ve bastante peor que un frame que llega 20 o 40 ms tarde. */
        for (attempt = 0; attempt < 6; attempt++) {
            int capped;
            memcpy(tgt, ideal, fsz);
            apply_quality(tgt, hid, pal, y0, y1, thr, thr);
            video.len = 0;
            a5_delta_encode(&video, hid, tgt, planes, &ds);
            late = late_vbls(t_free, ds.cycles, due, P);
            capped = !cyc_limit || ds.cycles <= cyc_limit;
            if (capped && late <= max_late) break;
            /* Si ni un delta vacio llega a tiempo, degradar no arregla nada. */
            if (capped && late_vbls(t_free, A5_CYC_FRAME, due, P) > max_late)
                break;
            thr = thr > 0 ? thr * 2 : 0.01;
        }
        if (attempt > 0) st->degraded++;
        if (cyc_limit && ds.cycles > cyc_limit) st->over_budget++;
        if (ds.cycles > st->maxcycles) {
            st->maxcycles = ds.cycles;
            st->maxcycles_frame = (int)n;
        }
        if (late > 0) {
            st->late_frames++;
            if (late > st->max_late) {
                st->max_late = late;
                st->max_late_frame = (int)n;
            }
        }
        t_free = due + late * P;              /* el intercambio de este delta */

        put_packet(st, A5V_OP_DELTA, newscene ? pal : NULL, ncolors,
                   &video, NULL, 0);
        st->ndelta++;
        if (newscene) st->npalette++;
        last_delta = n;

        memcpy(hid, tgt, fsz);
        { uint8_t *t = vis; vis = hid; hid = t; }
        vispal = pal;

        st->err_sum += idx_error(vis, ideal, pal, pal, y0, y1);
        st->bad_pixels += idx_bad(vis, ideal, pal, y0, y1);
        st->crc[n] = frame_crc(vis, fsz, pal, ncolors);
    }

    a5buf_free(&video);
    free(vis); free(hid); free(tgt);
}

/* ------------------------------------------------------------------ */

static void usage(void)
{
    printf(
"uso: a500vp-enc --in <video> [opciones]\n"
"\n"
"  --in PATH               video fuente (cualquier cosa que lea ffmpeg)\n"
"  --out PATH              bitstream de salida (work\\video.a5v)\n"
"  --preview PATH          mp4 opcional con la cuantizacion sin comprimir\n"
"                          (el preview de verdad lo hace a500vp-dec)\n"
"  --quality N             perdida permitida, 0 = sin perdida (8)\n"
"  --budget BYTES          tope de tamano del video; 0 = sin tope\n"
"                          (por defecto: 883712 menos el audio)\n"
"  --audio-period N        periodo de Paula del audio (443 = 8006,5 Hz)\n"
"  --repeat-boost F        cuanto mas permisivo es repetir que actualizar (1.5)\n"
"  --stability F           histeresis temporal del cuantizador (0.07)\n"
"  --max-late N            VBL de atraso que se toleran antes de degradar\n"
"                          un frame (2); el atraso se simula con el modelo\n"
"                          de costo medido en el Hito 4\n"
"  --frame-ms F            tope fijo de decodificacion por frame, ademas de\n"
"                          la simulacion; 0 = sin tope (0)\n"
"  --min-hold N            actualizar la imagen como mucho cada N huecos;\n"
"                          2 = 12,5 fps de imagenes distintas (2)\n"
"  --start SEG             desde donde cortar la fuente (0)\n"
"  --duration SEG          cuanto tomar; 0 = todo (0)\n"
"  --aspect MODO           letterbox | crop | stretch (letterbox)\n"
"  --rate MODO             native | pal (native)\n"
"                            native: repite frames para respetar la velocidad\n"
"                                    original; el audio no se toca\n"
"                            pal:    frames 1:1, todo 4%% mas rapido y el\n"
"                                    audio sube de tono, como la TV PAL\n"
"  --planes N              2, 3 o 4 bitplanes = 4, 8 o 16 colores (3)\n"
"  --dither MODO           none | bayer2 | bayer4 (none)\n"
"  --dither-strength F     fuerza del dither ordenado (0.05)\n"
"  --sharpen F             realce de bordes tras escalar; 0 = nada (1.2)\n"
"  --denoise F             denoise temporal antes de escalar; 0 = nada (0)\n"
"  --scene-threshold F     distancia Oklab media que dispara un corte (0.12)\n"
"  --min-scene N           frames minimos por escena (6)\n"
"  --preview-scale N       ampliacion del preview (2 = 640x512)\n"
"  --no-audio              preview sin audio\n"
"  --seed N                semilla del k-means (1)\n");
}

int main(int argc, char **argv)
{
    const char *in = NULL, *preview = NULL, *out = "work\\video.a5v";
    double start = 0, duration = 0;
    Aspect aspect = ASPECT_LETTERBOX;
    int planes = 3, min_scene = 6, pscale = 2, want_audio = 1;
    A5Dither dither = A5_DITHER_NONE;
    float dstrength = 0.05f;
    double sharpen = 1.2, denoise = 0;
    int    pal_speedup = 0;
    double scene_thr = 0.12;
    uint32_t seed = 1;
    double quality = 8, repeat_boost = 1.5, stability = 0.07;
    double frame_ms = 0;
    int    min_hold = 2;
    int    max_late = 2;
    long   cyc_limit;
    long   budget = -1;             /* -1 = disco menos el audio */
    int    audio_period = 443;
    uint8_t *idx = NULL;
    int i;

    A5SourceInfo src;
    char vfilter[512];
    int cols, rows, x0, y0, y1;
    int ncolors;

    uint8_t **frames = NULL, **srcframes = NULL;
    size_t nframes = 0, cap = 0, nsrc = 0;
    FILE *dec, *pre;
    size_t fsz = (size_t)A5_W * A5_H * 3;

    Oklab *prev = NULL, *cur = NULL;
    double *delta = NULL;
    Scene *scenes = NULL;
    int nscenes = 0;
    clock_t t0 = clock();

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        int has = i + 1 < argc;
        if (!strcmp(a, "--in") && has)                 in = argv[++i];
        else if (!strcmp(a, "--preview") && has)       preview = argv[++i];
        else if (!strcmp(a, "--out") && has)           out = argv[++i];
        else if (!strcmp(a, "--quality") && has)       quality = atof(argv[++i]);
        else if (!strcmp(a, "--budget") && has)        budget = atol(argv[++i]);
        else if (!strcmp(a, "--repeat-boost") && has) repeat_boost = atof(argv[++i]);
        else if (!strcmp(a, "--stability") && has)     stability = atof(argv[++i]);
        else if (!strcmp(a, "--frame-ms") && has)      frame_ms = atof(argv[++i]);
        else if (!strcmp(a, "--start") && has)         start = atof(argv[++i]);
        else if (!strcmp(a, "--duration") && has)      duration = atof(argv[++i]);
        else if (!strcmp(a, "--planes") && has)        planes = atoi(argv[++i]);
        else if (!strcmp(a, "--min-scene") && has)     min_scene = atoi(argv[++i]);
        else if (!strcmp(a, "--preview-scale") && has) pscale = atoi(argv[++i]);
        else if (!strcmp(a, "--seed") && has)          seed = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(a, "--scene-threshold") && has) scene_thr = atof(argv[++i]);
        else if (!strcmp(a, "--dither-strength") && has) dstrength = (float)atof(argv[++i]);
        else if (!strcmp(a, "--sharpen") && has)       sharpen = atof(argv[++i]);
        else if (!strcmp(a, "--denoise") && has)       denoise = atof(argv[++i]);
        else if (!strcmp(a, "--rate") && has) {
            const char *v = argv[++i];
            if (!strcmp(v, "native"))   pal_speedup = 0;
            else if (!strcmp(v, "pal")) pal_speedup = 1;
            else die("--rate: native o pal");
        }
        else if (!strcmp(a, "--no-audio"))             want_audio = 0;
        else if (!strcmp(a, "--min-hold") && has)      min_hold = atoi(argv[++i]);
        else if (!strcmp(a, "--max-late") && has)      max_late = atoi(argv[++i]);
        else if (!strcmp(a, "--audio-period") && has)  audio_period = atoi(argv[++i]);
        else if (!strcmp(a, "--aspect") && has) {
            const char *v = argv[++i];
            if (!strcmp(v, "letterbox"))    aspect = ASPECT_LETTERBOX;
            else if (!strcmp(v, "crop"))    aspect = ASPECT_CROP;
            else if (!strcmp(v, "stretch")) aspect = ASPECT_STRETCH;
            else die("--aspect: letterbox, crop o stretch");
        } else if (!strcmp(a, "--dither") && has) {
            const char *v = argv[++i];
            if (!strcmp(v, "none"))         dither = A5_DITHER_NONE;
            else if (!strcmp(v, "bayer2"))  dither = A5_DITHER_BAYER2;
            else if (!strcmp(v, "bayer4"))  dither = A5_DITHER_BAYER4;
            else die("--dither: none, bayer2 o bayer4");
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(); return 0;
        } else {
            fprintf(stderr, "opcion desconocida: %s\n", a);
            usage(); return 2;
        }
    }
    if (!in) { usage(); return 2; }
    if (planes < 1 || planes > 4) die("--planes tiene que ser 1..4");
    cyc_limit = (long)(A5_CPU_HZ * frame_ms / 1000.0);
    ncolors = 1 << planes;

    /* --- fuente --------------------------------------------------- */
    if (a5_probe(in, &src) != 0) die("ffprobe no pudo leer la fuente");

    printf("fuente     : %s\n", in);
    printf("             %dx%d, %.6f fps, %.2f s%s\n",
           src.width, src.height, src.fps, src.duration,
           src.progressive ? ", progresiva" : ", ENTRELAZADA");
    if (src.audio_rate)
        printf("             audio %d Hz, %d canales\n",
               src.audio_rate, src.audio_channels);
    if (!src.progressive)
        printf("  AVISO: la fuente dice ser entrelazada. Si viene de un "
               "telecine hay que\n         pasarle fieldmatch,decimate "
               "antes.\n");

    /* --- geometria -------------------------------------------------
     * Se calcula aca y no con force_original_aspect_ratio de ffmpeg porque
     * el pixel logico no es cuadrado: hay que corregir por A5_PAR. */
    {
        double dar = (double)src.width / src.height;

        if (aspect == ASPECT_STRETCH) {
            cols = A5_W; rows = A5_H;
        } else if (aspect == ASPECT_LETTERBOX) {
            cols = A5_W;
            rows = even((int)(A5_W * A5_PAR / dar + 0.5));
            if (rows > A5_H) {                 /* fuente mas alta que ancha */
                rows = A5_H;
                cols = even((int)(A5_H * dar / A5_PAR + 0.5));
            }
        } else {                               /* crop: llenar el cuadro */
            cols = A5_W;
            rows = even((int)(A5_W * A5_PAR / dar + 0.5));
            if (rows < A5_H) {
                rows = A5_H;
                cols = even((int)(A5_H * dar / A5_PAR + 0.5));
            }
        }
        x0 = (A5_W - cols) / 2;
        y0 = (A5_H - rows) / 2;
    }

    {
        /* El realce va despues de escalar, sobre la imagen chica: la linea
         * negra del anime a 160 px de ancho es sub-pixel y el escalado se la
         * come. Antes de escalar no serviria de nada. */
        char sh[128] = "", dn[128] = "";
        if (sharpen > 0)
            snprintf(sh, sizeof sh, ",unsharp=3:3:%.3f:3:3:0", sharpen);
        /* El denoise va ANTES de escalar y es sobre todo temporal: la fuente
         * es un h264 de 2 Mbit/s y su ruido de compresion no se ve a 160 px
         * de ancho, pero cambia en cada frame y se paga entero en bytes de
         * delta. Los dos ultimos parametros de hqdn3d son los temporales. */
        if (denoise > 0)
            snprintf(dn, sizeof dn, "hqdn3d=%.2f:%.2f:%.2f:%.2f,",
                     denoise * 0.5, denoise * 0.4, denoise, denoise);

        if (aspect == ASPECT_CROP)
            snprintf(vfilter, sizeof vfilter,
                     "%sscale=%d:%d:flags=lanczos%s,crop=%d:%d:(iw-%d)/2:"
                     "(ih-%d)/2,setsar=1", dn, cols, rows, sh, A5_W, A5_H,
                     A5_W, A5_H);
        else
            snprintf(vfilter, sizeof vfilter,
                     "%sscale=%d:%d:flags=lanczos%s,pad=%d:%d:%d:%d:"
                     "color=black,setsar=1", dn, cols, rows, sh, A5_W, A5_H,
                     x0 < 0 ? 0 : x0, y0 < 0 ? 0 : y0);
    }

    if (aspect == ASPECT_CROP) { y0 = 0; y1 = A5_H; }
    else                       { y1 = y0 + rows; if (y1 > A5_H) y1 = A5_H;
                                 if (y0 < 0) y0 = 0; }

    if (aspect == ASPECT_CROP)
        printf("geometria  : %d colores, se escala a %dx%d y se recorta a "
               "%dx%d (se pierden los bordes)\n",
               ncolors, cols, rows, A5_W, A5_H);
    else
        printf("geometria  : %d colores, area activa %dx%d centrada en %dx%d "
               "(%d filas de barra)\n",
               ncolors, cols, rows, A5_W, A5_H, A5_H - rows);
    printf("             filtro ffmpeg: %s\n", vfilter);

    /* --- decodificar ------------------------------------------------ */
    dec = a5_open_decoder(in, start, duration, vfilter, A5_W, A5_H);
    if (!dec) die("no pude arrancar ffmpeg");

    for (;;) {
        uint8_t *f;
        if (nframes == cap) {
            cap = cap ? cap * 2 : 256;
            frames = realloc(frames, cap * sizeof *frames);
            if (!frames) die("sin memoria");
        }
        f = malloc(fsz);
        if (!f) die("sin memoria");
        if (fread(f, 1, fsz, dec) != fsz) { free(f); break; }
        frames[nframes++] = f;
    }
    a5_pclose(dec);
    if (nframes == 0) die("ffmpeg no devolvio ningun frame");

    /* --- cadencia ---------------------------------------------------
     * La Amiga solo puede cambiar de frame cada 2 VBL, o sea 24,960205 fps
     * clavados. Una fuente de 23,976 se encaja de dos maneras:
     *
     *   pal    : frames 1:1. Todo corre 4,1% mas rapido, que es lo que hacia
     *            la television PAL, y obliga a subir el tono del audio.
     *   native : se repite un frame cada tanto para que la velocidad sea la
     *            original y el audio quede intacto. Como el formato ya tiene
     *            comando de repeticion, esos frames no cuestan casi nada.
     *
     * De aca en adelante se trabaja sobre los huecos de pantalla, no sobre
     * los frames de la fuente.
     */
    {
        double ratio = src.fps / A5_VIDEO_FPS;    /* frames fuente por hueco */
        size_t nslots = pal_speedup ? nframes
                                    : (size_t)(nframes / ratio + 0.5);
        uint8_t **slots = malloc(nslots * sizeof *slots);
        size_t k;

        if (!slots) die("sin memoria");
        for (k = 0; k < nslots; k++) {
            size_t sidx = pal_speedup ? k : (size_t)(k * ratio + 0.5);
            if (sidx >= nframes) sidx = nframes - 1;
            slots[k] = frames[sidx];
        }
        srcframes = frames; nsrc = nframes;
        frames = slots;     nframes = nslots;
    }

    printf("frames     : %lu de la fuente -> %lu en pantalla\n",
           (unsigned long)nsrc, (unsigned long)nframes);
    if (pal_speedup)
        printf("             cadencia pal: %.3f s de fuente en %.3f s, "
               "%.2f%% mas rapido\n", nsrc / src.fps, nframes / A5_VIDEO_FPS,
               (A5_VIDEO_FPS / src.fps - 1) * 100);
    else
        printf("             cadencia native: %.3f s de fuente en %.3f s, "
               "%lu frames repetidos, audio intacto\n",
               nsrc / src.fps, nframes / A5_VIDEO_FPS,
               (unsigned long)(nframes - nsrc));

    /* Presupuesto por defecto: el disco menos lo que se va a llevar el audio
     * (Hito 5: fib4, 4 bits por muestra, a 3546895/periodo Hz exactos). Se
     * reserva desde ahora para no volver a presupuestar el video contra un
     * disco que en realidad tiene ~4 KB/s menos. */
    if (budget < 0) {
        double paula_hz = A5_CCK_PAL / audio_period;
        long reserve = (long)ceil(nframes / A5_VIDEO_FPS * paula_hz / 2.0);
        budget = A5V_DEFAULT_BUDGET - reserve;
        printf("presupuesto: %ld bytes de video = %d de disco - %ld de audio "
               "(fib4 a %.1f Hz)\n", budget, A5V_DEFAULT_BUDGET, reserve,
               paula_hz);
    }

    /* --- deteccion de cortes ---------------------------------------- */
    {
        size_t npix = (size_t)(y1 - y0) * A5_W, k;
        size_t n;
        prev = malloc(npix * sizeof *prev);
        cur  = malloc(npix * sizeof *cur);
        delta = malloc(nframes * sizeof *delta);
        if (!prev || !cur || !delta) die("sin memoria");

        for (n = 0; n < nframes; n++) {
            const uint8_t *p = frames[n] + (size_t)y0 * A5_W * 3;
            double sum = 0;
            for (k = 0; k < npix; k++, p += 3)
                cur[k] = a5_srgb_to_oklab(p[0], p[1], p[2]);
            if (n == 0) {
                delta[0] = 1e9;                 /* el primer frame corta */
            } else {
                for (k = 0; k < npix; k++)
                    sum += sqrt((double)a5_oklab_dist2(cur[k], prev[k]));
                delta[n] = sum / npix;
            }
            memcpy(prev, cur, npix * sizeof *cur);
        }
    }

    scenes = malloc(nframes * sizeof *scenes);
    if (!scenes) die("sin memoria");
    {
        size_t n;
        for (n = 0; n < nframes; n++) {
            int cut = (n == 0) || (delta[n] >= scene_thr &&
                       (int)(n - scenes[nscenes - 1].start) >= min_scene);
            if (cut) {
                scenes[nscenes].start = (int)n;
                scenes[nscenes].count = 0;
                nscenes++;
            }
            scenes[nscenes - 1].count++;
        }
    }

    /* --- paleta por escena y cuantizacion a indices ------------------ */
    idx = malloc(nframes * (size_t)A5_W * A5_H);
    if (!idx) die("sin memoria para los frames cuantizados");
    {
        int s;
        int reserve_black = (aspect != ASPECT_CROP) && (y1 - y0 < A5_H);

        for (s = 0; s < nscenes; s++) {
            A5Hist *h = a5_hist_new();
            int n;
            for (n = 0; n < scenes[s].count; n++)
                a5_hist_add_frame(h, frames[scenes[s].start + n], A5_W, y0, y1);
            a5_quantize(h, ncolors, reserve_black, seed + s, &scenes[s].pal);
            a5_hist_assign(h, &scenes[s].pal);
            scenes[s].unique = a5_hist_unique(h);
            scenes[s].error = a5_hist_mean_error(h, &scenes[s].pal);

            /* Se cuantiza aca, con el histograma todavia vivo: sin dither
             * alcanza con la cache de indices que ya tiene cada color.
             *
             * Histeresis temporal: si el indice que tenia el pixel en el
             * frame anterior sigue siendo casi tan bueno como el optimo, se
             * lo deja. La fuente es un h264 de 2 Mbit/s y tiene ruido de
             * compresion; sin esto, en una zona quieta los pixeles saltan
             * entre dos colores vecinos de la paleta y cada salto se paga en
             * bytes de delta sin que se vea nada. */
            for (n = 0; n < scenes[s].count; n++) {
                size_t f = (size_t)scenes[s].start + n;
                uint8_t *dst = idx + f * A5_W * A5_H;
                const uint8_t *pv = n ? dst - (size_t)A5_W * A5_H : NULL;
                int y;
                memset(dst, 0, (size_t)A5_W * A5_H);
                for (y = y0; y < y1; y++) {
                    const uint8_t *p = frames[f] + (size_t)y * A5_W * 3;
                    int x;
                    for (x = 0; x < A5_W; x++, p += 3) {
                        int best = dither == A5_DITHER_NONE
                            ? a5_hist_lookup(h, p[0], p[1], p[2])
                            : a5_map_pixel_dither(&scenes[s].pal, p[0], p[1],
                                                  p[2], x, y, dither, dstrength);
                        if (pv && stability > 0) {
                            int old = pv[y * A5_W + x];
                            if (old != best) {
                                Oklab c = a5_srgb_to_oklab(p[0], p[1], p[2]);
                                double dold = sqrt((double)a5_oklab_dist2(
                                    c, scenes[s].pal.lab[old]));
                                double dnew = sqrt((double)a5_oklab_dist2(
                                    c, scenes[s].pal.lab[best]));
                                if (dold <= dnew + stability) best = old;
                            }
                        }
                        dst[y * A5_W + x] = (uint8_t)best;
                    }
                }
            }
            a5_hist_free(h);
        }
    }

    /* --- preview de la cuantizacion, sin comprimir --------------------
     * Es el techo de calidad: lo que se veria si el bitstream fuese sin
     * perdida. El preview de lo que realmente sale del disco lo genera el
     * decoder de referencia. */
    if (preview && *preview) {
        uint8_t *row = malloc((size_t)A5_DISP_W * 3);
        size_t n;
        int s = 0;

        pre = a5_open_preview(preview, A5_DISP_W, A5_DISP_H, A5_VIDEO_FPS,
                              pscale, want_audio && src.audio_rate ? in : NULL,
                              start, duration,
                              pal_speedup ? A5_VIDEO_FPS / src.fps : 1.0,
                              src.audio_rate);
        if (!pre || !row) die("no pude arrancar ffmpeg para el preview");

        for (n = 0; n < nframes; n++) {
            const A5Palette *pal;
            const uint8_t *f = idx + n * (size_t)A5_W * A5_H;
            int y;
            while (s + 1 < nscenes && (int)n >= scenes[s + 1].start) s++;
            pal = &scenes[s].pal;

            for (y = 0; y < A5_H; y++) {
                int x;
                for (x = 0; x < A5_W; x++) {
                    uint8_t r, g, b;
                    a5_rgb444_to_srgb(pal->rgb444[f[y * A5_W + x]], &r, &g, &b);
                    row[x * 6 + 0] = r; row[x * 6 + 1] = g; row[x * 6 + 2] = b;
                    row[x * 6 + 3] = r; row[x * 6 + 4] = g; row[x * 6 + 5] = b;
                }
                fwrite(row, 1, (size_t)A5_DISP_W * 3, pre);
                fwrite(row, 1, (size_t)A5_DISP_W * 3, pre);
            }
        }
        free(row);
        a5_pclose(pre);
    }

    /* --- estadisticas ------------------------------------------------ */
    {
        int s;
        double werr = 0;
        size_t still = 0, n;

        printf("\nescenas    : %d (umbral %.3f, minimo %d frames)\n",
               nscenes, scene_thr, min_scene);
        printf("  #   frame  largo  seg   colores  error  paleta\n");
        for (s = 0; s < nscenes; s++) {
            int c;
            printf("  %-3d %5d  %5d %5.2f  %7lu  %.4f  ", s,
                   scenes[s].start, scenes[s].count,
                   scenes[s].count / A5_VIDEO_FPS,
                   (unsigned long)scenes[s].unique, scenes[s].error);
            for (c = 0; c < scenes[s].pal.n; c++)
                printf("%03X ", scenes[s].pal.rgb444[c]);
            printf("\n");
            werr += scenes[s].error * scenes[s].count;
        }
        printf("\nerror medio de cuantizacion (Oklab): %.4f\n", werr / nframes);

        for (n = 1; n < nframes; n++)
            if (delta[n] < 0.008) still++;
        printf("frames casi identicos al anterior: %lu de %lu (%.1f%%)\n",
               (unsigned long)still, (unsigned long)nframes - 1,
               100.0 * still / (nframes - 1));
        printf("  -> es lo que va a poder comprimir el comando de repeticion\n");
    }

    /* --- bitstream y control de tasa ---------------------------------
     * Se codifica entero con una calidad dada y se mide. Si no entra en el
     * presupuesto, se sube la perdida y se vuelve a codificar. Busqueda
     * binaria sobre el umbral, que es monotono: mas umbral, menos bytes. */
    {
        A5Stream st;
        double lo = quality * 0.0015, hi = lo;
        size_t total;
        int pass = 0;

        build_stream(&st, idx, nframes, scenes, nscenes, planes, ncolors,
                     y0, y1, lo, repeat_boost, cyc_limit, min_hold, max_late);
        total = A5V_HEADER_SIZE + st.buf.len;
        printf("\nbitstream  : calidad %.4f -> %lu bytes\n", lo,
               (unsigned long)total);

        if (budget > 0 && total > (size_t)budget) {
            hi = lo > 0 ? lo : 0.01;
            do {
                hi *= 2;
                a5buf_free(&st.buf); free(st.crc);
                build_stream(&st, idx, nframes, scenes, nscenes, planes,
                             ncolors, y0, y1, hi, repeat_boost, cyc_limit, min_hold, max_late);
                total = A5V_HEADER_SIZE + st.buf.len;
                printf("             calidad %.4f -> %lu bytes\n", hi,
                       (unsigned long)total);
            } while (total > (size_t)budget && hi < 1.0 && ++pass < 12);

            for (pass = 0; pass < 10 && hi - lo > 0.0005; pass++) {
                double mid = (lo + hi) / 2;
                A5Stream t2;
                build_stream(&t2, idx, nframes, scenes, nscenes, planes,
                             ncolors, y0, y1, mid, repeat_boost, cyc_limit, min_hold, max_late);
                if (A5V_HEADER_SIZE + t2.buf.len <= (size_t)budget) {
                    a5buf_free(&st.buf); free(st.crc);
                    st = t2; hi = mid; total = A5V_HEADER_SIZE + st.buf.len;
                } else {
                    a5buf_free(&t2.buf); free(t2.crc);
                    lo = mid;
                }
            }
            printf("             ajustado a %.4f -> %lu bytes\n", hi,
                   (unsigned long)total);
        }

        /* --- escribir --------------------------------------------- */
        {
            FILE *f = fopen(out, "wb");
            A5Buf hdr;
            char crcpath[1024];

            if (!f) die("no pude abrir el archivo de salida");
            a5buf_init(&hdr);
            a5v_put_header(&hdr, planes, ncolors, 0 /* sin audio aun */,
                           y0, y1, (uint32_t)nframes, (uint32_t)st.buf.len);
            fwrite(hdr.p, 1, hdr.len, f);
            fwrite(st.buf.p, 1, st.buf.len, f);
            fclose(f);
            a5buf_free(&hdr);

            snprintf(crcpath, sizeof crcpath, "%s.crc", out);
            f = fopen(crcpath, "wb");
            if (f) {
                size_t n;
                for (n = 0; n < nframes; n++) {
                    uint8_t b[4];
                    b[0] = (uint8_t)(st.crc[n] >> 24);
                    b[1] = (uint8_t)(st.crc[n] >> 16);
                    b[2] = (uint8_t)(st.crc[n] >> 8);
                    b[3] = (uint8_t)st.crc[n];
                    fwrite(b, 1, 4, f);
                }
                fclose(f);
            }

            printf("\nsalida     : %s (%lu bytes)\n", out,
                   (unsigned long)total);
            printf("             %s (checksums para el decoder)\n", crcpath);
        }

        /* --- estadisticas del bitstream ---------------------------- */
        printf("\npaquetes   : %d (%d delta, %d repeticion = %.1f%%, "
               "%d cambios de paleta)\n",
               st.npackets, st.ndelta, st.nrepeat,
               100.0 * st.nrepeat / st.npackets, st.npalette);
        printf("bytes/frame: %.1f de media, %lu el peor (frame %d)\n",
               (double)st.buf.len / nframes, (unsigned long)st.maxpacket,
               st.maxpacket_frame);
        printf("tasa       : %.1f KB/s de video\n",
               st.buf.len / 1024.0 / (nframes / A5_VIDEO_FPS));
        printf("decodif.   : peor delta %ld ciclos = %.1f ms (frame %d), "
               "con el modelo medido\n",
               st.maxcycles, st.maxcycles * 1000.0 / A5_CPU_HZ,
               st.maxcycles_frame);
        printf("tiempo real: %d frames se van a ver tarde, el peor por %d VBL "
               "(frame %d); %d degradados para no pasar de %d VBL\n",
               st.late_frames, st.max_late, st.max_late_frame, st.degraded,
               max_late);
        if (st.over_budget)
            printf("  AVISO: %d frames pasan el tope fijo de %.0f ms\n",
                   st.over_budget, frame_ms);
        printf("error final: %.4f (contra el frame cuantizado ideal)\n",
               st.err_sum / nframes);
        printf("salpicado  : %.2f%% de los pixeles activos con error "
               "visible (> %.2f)\n",
               100.0 * st.bad_pixels / ((double)nframes * (y1 - y0) * A5_W),
               A5_VISIBLE_ERR);
        if (budget > 0)
            printf("presupuesto: %ld bytes, %s por %ld\n", budget,
                   total <= (size_t)budget ? "entra" : "NO ENTRA",
                   (long)((long)budget - (long)total));

        a5buf_free(&st.buf);
        free(st.crc);
    }

    printf("\ntiempo     : %.1f s\n", (double)(clock() - t0) / CLOCKS_PER_SEC);

    for (i = 0; (size_t)i < nsrc; i++) free(srcframes[i]);
    free(srcframes); free(frames); free(idx);
    free(prev); free(cur); free(delta); free(scenes);
    return 0;
}
