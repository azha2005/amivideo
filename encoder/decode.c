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
    uint8_t pb[A5_MAX_COLORS * 2];
    int c;

    for (c = 0; c < ncolors; c++) {
        pb[c * 2]     = (uint8_t)(pal[c] >> 8);
        pb[c * 2 + 1] = (uint8_t)pal[c];
    }
    return a5_crc32(pb, (size_t)ncolors * 2, a5_crc32(vis, fsz, 0));
}

/* Exporta el frame visible como un bitstream de un solo paquete: un DELTA
 * desde negro con su paleta. Es lo que muestra el reproductor del Hito 3.
 * Escribe ademas lo que el reproductor tiene que dejar en su framebuffer
 * (<out>.fb: planos doblados a 40 bytes por fila, plano 0 primero) y una
 * imagen de referencia de 320x256 (<out>.ppm). */
static void write_still(const char *out, int planes, int ncolors, int y0,
                        int y1, const uint8_t *idx, const A5Color *pal)
{
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

    raw = 6 + (size_t)ncolors * 2 + delta.len;
    plen = (raw + 1) & ~(size_t)1;
    a5v_put_header(&file, planes, ncolors, 0, y0, y1, 1, (uint32_t)plen);
    a5buf_put16(&file, (unsigned)plen);
    a5buf_put8(&file, A5V_OP_DELTA);
    a5buf_put8(&file, A5V_F_PALETTE);
    a5buf_put16(&file, 0);
    for (c = 0; c < ncolors; c++) a5buf_put16(&file, pal[c]);
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
            a5_rgb444_to_srgb(pal[idx[(y / 2) * A5_W + x / 2]], &r, &g, &bl);
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

    uint8_t *data, *crcdata = NULL;
    size_t len, crclen = 0;
    const uint8_t *p, *end;
    int planes, ncolors, w, h, y0, y1;
    uint32_t nframes;

    uint8_t *fb[2], *idxbuf;
    A5Color pal[A5_MAX_COLORS];
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
        else {
            printf("uso: a500vp-dec --in <video.a5v> [--preview <out.mp4>]\n"
                   "                [--audio <fuente>] [--audio-start S]\n"
                   "                [--audio-duration S] [--preview-scale N]\n"
                   "                [--still K --still-out <frame.a5v>]\n");
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

    if (w != A5_W || h != A5_H) die("geometria inesperada");

    printf("bitstream  : %s (%lu bytes)\n", in, (unsigned long)len);
    printf("             %dx%d, %d planos, %d colores, filas activas %d..%d\n",
           w, h, planes, ncolors, y0, y1 - 1);
    printf("             %lu frames, %.3f s\n", (unsigned long)nframes,
           nframes / A5_VIDEO_FPS);

    {
        char crcpath[1024];
        snprintf(crcpath, sizeof crcpath, "%s.crc", in);
        crcdata = slurp(crcpath, &crclen);
        if (crcdata && crclen < nframes * 4u) {
            free(crcdata); crcdata = NULL;
            printf("             (el archivo .crc esta corto, se ignora)\n");
        }
    }

    fsz    = (size_t)A5_W * A5_H;
    fbsize = (size_t)planes * A5_H * A5_ROWBYTES;
    fb[0] = calloc(fbsize, 1);
    fb[1] = calloc(fbsize, 1);
    idxbuf = malloc(fsz);
    if (!fb[0] || !fb[1] || !idxbuf) die("sin memoria");
    memset(pal, 0, sizeof pal);

    if (preview) {
        row = malloc((size_t)A5_DISP_W * 3);
        pre = a5_open_preview(preview, A5_DISP_W, A5_DISP_H, A5_VIDEO_FPS,
                              pscale, audio_src, audio_start, audio_dur,
                              1.0, audio_src ? 44100 : 0);
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
            if (pkend - pk < ncolors * 2) die("paleta incompleta");
            for (c = 0; c < ncolors; c++) pal[c] = (A5Color)be16(pk + c * 2);
            pk += ncolors * 2;
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
            visible ^= 1;
            ndelta++;
            total_bytes += ds.bytes;
            total_rows += ds.rows;
            total_cols += ds.cols;
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
                    a5_rgb444_to_srgb(pal[idxbuf[y * A5_W + x]], &r, &g, &b);
                    row[x * 6 + 0] = r; row[x * 6 + 1] = g; row[x * 6 + 2] = b;
                    row[x * 6 + 3] = r; row[x * 6 + 4] = g; row[x * 6 + 5] = b;
                }
                fwrite(row, 1, (size_t)A5_DISP_W * 3, pre);
                fwrite(row, 1, (size_t)A5_DISP_W * 3, pre);
            }
        }

        if (crcdata) {
            uint32_t want = be32(crcdata + n * 4);
            uint32_t got  = frame_crc(idxbuf, fsz, pal, ncolors);
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
            write_still(still_out, planes, ncolors, y0, y1, idxbuf, pal);
        }

        p = pkend;
    }

    if (pre) { a5_pclose(pre); free(row); }

    printf("\npaquetes   : %lu (%d delta, %d repeticion, %d paletas)\n",
           (unsigned long)nframes, ndelta, nrepeat, npal);
    printf("filas      : %ld modificadas, %.1f columnas por fila\n",
           total_rows, total_rows ? (double)total_cols / total_rows : 0);
    printf("donde van los bytes:\n");
    printf("  cabecera de paquete  %8ld  (%4.1f%%)\n",
           (long)nframes * 6, 100.0 * nframes * 6 / len);
    printf("  paletas              %8ld  (%4.1f%%)\n",
           (long)npal * ncolors * 2, 100.0 * npal * ncolors * 2 / len);
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
    return 0;
}
