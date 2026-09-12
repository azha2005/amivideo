/* a500vp.h - tipos y constantes compartidos por el encoder y el decoder de
 * referencia. Ver docs/FORMAT.md y docs/DECISIONS.md.
 */
#ifndef A500VP_H
#define A500VP_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* --- geometria logica ---------------------------------------------------
 * El frame logico es de 160x128 y se ve a 320x256 en lowres PAL: cada pixel
 * logico ocupa 2x2 pixeles de pantalla.
 */
#define A5_W            160
#define A5_H            128
#define A5_DISP_W       (A5_W * 2)
#define A5_DISP_H       (A5_H * 2)

/* --- temporizacion ------------------------------------------------------
 * Reloj de color PAL exacto y frame no entrelazado de 313 lineas x 227 CCK.
 * Da 49,920409 Hz de VBL; el video va a 1 frame cada 2 VBL.
 */
#define A5_CCK_PAL       3546895.0
#define A5_PAL_LINES     313
#define A5_CCK_PER_LINE  227
#define A5_VBL_HZ        (A5_CCK_PAL / (A5_PAL_LINES * (double)A5_CCK_PER_LINE))
#define A5_VIDEO_FPS     (A5_VBL_HZ / 2.0)

#define A5_MAX_COLORS    32      /* 5 bitplanes */

/* --- color --------------------------------------------------------------
 * Oklab como espacio perceptual. Es mas simple que CIE Lab, no necesita
 * elegir blanco de referencia y se porta mejor con azules saturados, que en
 * un opening de anime hay de sobra.
 */
typedef struct { float L, a, b; } Oklab;

Oklab    a5_srgb_to_oklab(uint8_t r, uint8_t g, uint8_t b);
void     a5_oklab_to_srgb(Oklab c, uint8_t *r, uint8_t *g, uint8_t *bb);
float    a5_oklab_dist2(Oklab p, Oklab q);

/* Un color RGB444 (lo que entiende el OCS) empaquetado como 0x0RGB. */
typedef uint16_t A5Color;

A5Color  a5_srgb_to_rgb444(uint8_t r, uint8_t g, uint8_t b);
void     a5_rgb444_to_srgb(A5Color c, uint8_t *r, uint8_t *g, uint8_t *b);

/* --- paletas y cuantizacion --------------------------------------------- */
typedef struct {
    int      n;                    /* cantidad de colores en uso */
    A5Color  rgb444[A5_MAX_COLORS];
    Oklab    lab[A5_MAX_COLORS];   /* los mismos colores, ya redondeados */
} A5Palette;

/* Tabla de colores unicos de una escena, con su cuenta y su indice asignado. */
typedef struct A5Hist A5Hist;

A5Hist  *a5_hist_new(void);
void     a5_hist_free(A5Hist *h);
void     a5_hist_add_frame(A5Hist *h, const uint8_t *rgb, int w,
                           int y0, int y1);
size_t   a5_hist_unique(const A5Hist *h);

/* k-means en Oklab sobre los colores unicos, ponderado por cuenta.
 * Si reserve_black, el indice 0 queda fijo en negro puro y el k-means corre
 * con n-1 centroides. seed fija el k-means++ para que el encoder sea
 * reproducible. */
void     a5_quantize(A5Hist *h, int ncolors, int reserve_black,
                     uint32_t seed, A5Palette *out);

/* Asigna a cada color unico su indice de paleta (se cachea en la tabla). */
void     a5_hist_assign(A5Hist *h, const A5Palette *pal);

/* Indice de paleta de un color concreto, usando la cache de la tabla. */
int      a5_hist_lookup(const A5Hist *h, uint8_t r, uint8_t g, uint8_t b);

/* Error medio de cuantizacion (distancia Oklab) ponderado por cuenta. */
double   a5_hist_mean_error(const A5Hist *h, const A5Palette *pal);

/* --- dithering ordenado -------------------------------------------------
 * Prohibida la difusion de error: rompe la compresion delta. Solo Bayer.
 */
typedef enum { A5_DITHER_NONE = 0, A5_DITHER_BAYER2, A5_DITHER_BAYER4 } A5Dither;

int      a5_map_pixel_dither(const A5Palette *pal, uint8_t r, uint8_t g,
                             uint8_t b, int x, int y, A5Dither d,
                             float strength);

/* --- proceso con ffmpeg ------------------------------------------------- */
typedef struct {
    int      width, height;
    double   fps;              /* frames por segundo reales de la fuente */
    double   duration;         /* segundos */
    int      progressive;
    int      audio_rate;       /* 0 si no hay audio */
    int      audio_channels;
} A5SourceInfo;

int      a5_probe(const char *path, A5SourceInfo *out);

/* Abre ffmpeg y devuelve un FILE* del que salen frames rgb24 de w*h. */
FILE    *a5_open_decoder(const char *path, double start, double duration,
                         const char *vfilter, int w, int h);

/* Abre ffmpeg y devuelve un FILE* del que sale el audio de la fuente en mono,
 * s16le a rate Hz. afilter (puede ser NULL) va antes del remuestreo. */
FILE    *a5_open_audio(const char *path, double start, double duration,
                       const char *afilter, int rate);

/* Abre ffmpeg escribiendo un mp4; se le mandan frames rgb24 de w*h. */
FILE    *a5_open_preview(const char *out, int w, int h, double fps,
                         int scale, const char *audio_src,
                         double audio_start, double audio_dur,
                         double audio_ratio, int audio_rate);

int      a5_pclose(FILE *f);

#endif /* A500VP_H */
