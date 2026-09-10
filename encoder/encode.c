/* encode.c - encoder de A500VP, Hito 1: imagen.
 *
 * Lee el video con ffmpeg, lo mete en el frame logico de 160x128 con la
 * relacion de aspecto elegida, detecta cortes de escena y cuantiza a una
 * paleta RGB444 por escena. Saca un preview en mp4 de como se va a ver.
 *
 * Todavia no comprime nada: eso es el Hito 2.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "a500vp.h"

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

/* ------------------------------------------------------------------ */

static void usage(void)
{
    printf(
"uso: a500vp-enc --in <video> [opciones]\n"
"\n"
"  --in PATH               video fuente (cualquier cosa que lea ffmpeg)\n"
"  --preview PATH          mp4 de salida (por defecto work\\preview.mp4)\n"
"  --start SEG             desde donde cortar la fuente (0)\n"
"  --duration SEG          cuanto tomar; 0 = todo (0)\n"
"  --aspect MODO           letterbox | crop | stretch (letterbox)\n"
"  --planes N              2, 3 o 4 bitplanes = 4, 8 o 16 colores (3)\n"
"  --dither MODO           none | bayer2 | bayer4 (none)\n"
"  --dither-strength F     fuerza del dither ordenado (0.05)\n"
"  --scene-threshold F     distancia Oklab media que dispara un corte (0.12)\n"
"  --min-scene N           frames minimos por escena (6)\n"
"  --preview-scale N       ampliacion del preview (2 = 640x512)\n"
"  --no-audio              preview sin audio\n"
"  --seed N                semilla del k-means (1)\n");
}

int main(int argc, char **argv)
{
    const char *in = NULL, *preview = "work\\preview.mp4";
    double start = 0, duration = 0;
    Aspect aspect = ASPECT_LETTERBOX;
    int planes = 3, min_scene = 6, pscale = 2, want_audio = 1;
    A5Dither dither = A5_DITHER_NONE;
    float dstrength = 0.05f;
    double sharpen = 0;
    double scene_thr = 0.12;
    uint32_t seed = 1;
    int i;

    A5SourceInfo src;
    char vfilter[512];
    int cols, rows, x0, y0, y1;
    int ncolors;

    uint8_t **frames = NULL;
    size_t nframes = 0, cap = 0;
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
        else if (!strcmp(a, "--start") && has)         start = atof(argv[++i]);
        else if (!strcmp(a, "--duration") && has)      duration = atof(argv[++i]);
        else if (!strcmp(a, "--planes") && has)        planes = atoi(argv[++i]);
        else if (!strcmp(a, "--min-scene") && has)     min_scene = atoi(argv[++i]);
        else if (!strcmp(a, "--preview-scale") && has) pscale = atoi(argv[++i]);
        else if (!strcmp(a, "--seed") && has)          seed = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(a, "--scene-threshold") && has) scene_thr = atof(argv[++i]);
        else if (!strcmp(a, "--dither-strength") && has) dstrength = (float)atof(argv[++i]);
        else if (!strcmp(a, "--sharpen") && has)       sharpen = atof(argv[++i]);
        else if (!strcmp(a, "--no-audio"))             want_audio = 0;
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
        char sh[128] = "";
        if (sharpen > 0)
            snprintf(sh, sizeof sh, ",unsharp=3:3:%.3f:3:3:0", sharpen);

        if (aspect == ASPECT_CROP)
            snprintf(vfilter, sizeof vfilter,
                     "scale=%d:%d:flags=lanczos%s,crop=%d:%d:(iw-%d)/2:"
                     "(ih-%d)/2,setsar=1", cols, rows, sh, A5_W, A5_H,
                     A5_W, A5_H);
        else
            snprintf(vfilter, sizeof vfilter,
                     "scale=%d:%d:flags=lanczos%s,pad=%d:%d:%d:%d:color=black,"
                     "setsar=1", cols, rows, sh, A5_W, A5_H, x0 < 0 ? 0 : x0,
                     y0 < 0 ? 0 : y0);
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

    printf("frames     : %lu (%.3f s a %.6f fps de la Amiga)\n",
           (unsigned long)nframes, nframes / A5_VIDEO_FPS, A5_VIDEO_FPS);
    printf("             la fuente dura %.3f s: el PAL la acelera %.2f%%\n",
           nframes / src.fps, (A5_VIDEO_FPS / src.fps - 1) * 100);

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

    /* --- paleta por escena ------------------------------------------ */
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
            a5_hist_free(h);
        }
    }

    /* --- preview ---------------------------------------------------- */
    pre = a5_open_preview(preview, A5_DISP_W, A5_DISP_H, A5_VIDEO_FPS, pscale,
                          want_audio && src.audio_rate ? in : NULL,
                          start, duration, A5_VIDEO_FPS / src.fps,
                          src.audio_rate);
    if (!pre) die("no pude arrancar ffmpeg para el preview");

    {
        uint8_t *row = malloc((size_t)A5_DISP_W * 3);
        double err_sum = 0;
        size_t n;
        int s = 0;
        if (!row) die("sin memoria");

        for (n = 0; n < nframes; n++) {
            const A5Palette *pal;
            int y;
            while (s + 1 < nscenes && (int)n >= scenes[s + 1].start) s++;
            pal = &scenes[s].pal;

            for (y = 0; y < A5_H; y++) {
                const uint8_t *p = frames[n] + (size_t)y * A5_W * 3;
                int x;
                for (x = 0; x < A5_W; x++, p += 3) {
                    uint8_t r, g, b;
                    int idx;
                    if (y < y0 || y >= y1) idx = 0;
                    else idx = a5_map_pixel_dither(pal, p[0], p[1], p[2],
                                                   x, y, dither, dstrength);
                    a5_rgb444_to_srgb(pal->rgb444[idx], &r, &g, &b);
                    row[x * 6 + 0] = r; row[x * 6 + 1] = g; row[x * 6 + 2] = b;
                    row[x * 6 + 3] = r; row[x * 6 + 4] = g; row[x * 6 + 5] = b;
                }
                fwrite(row, 1, (size_t)A5_DISP_W * 3, pre);
                fwrite(row, 1, (size_t)A5_DISP_W * 3, pre);
            }
        }
        free(row);
        (void)err_sum;
    }
    a5_pclose(pre);

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

    printf("\npreview    : %s (%dx%d, %.3f s)\n", preview,
           A5_DISP_W * pscale, A5_DISP_H * pscale, nframes / A5_VIDEO_FPS);
    printf("tiempo     : %.1f s\n", (double)(clock() - t0) / CLOCKS_PER_SEC);

    for (i = 0; (size_t)i < nframes; i++) free(frames[i]);
    free(frames); free(prev); free(cur); free(delta); free(scenes);
    return 0;
}
