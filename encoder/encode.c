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
#include "adpcm.h"
#include "adf.h"

/* Un pixel logico es 2x2 pixeles de pantalla, y la pantalla de 320x256 se ve
 * en un tubo 4:3. O sea que el pixel logico es 1,0667 veces mas ancho que
 * alto (la SAR 16:15 clasica de PAL lowres). */
#define A5_PAR   ((4.0 / 3.0) * (A5_DISP_H / (double)A5_DISP_W))

typedef enum { ASPECT_LETTERBOX, ASPECT_CROP, ASPECT_STRETCH } Aspect;

typedef struct {
    int    start, count;       /* primer frame y cantidad */
    A5Palette *pal;            /* una por franja (g_nbands) */
    size_t unique;
    double error;
} Scene;

/* Franjas de paleta, fijas para todo el video (se deciden en main). */
static int g_band_rows = 0, g_band_y0 = 0, g_nbands = 1;
/* Colores que cada franja puede cambiar respecto de la de arriba. El Copper
 * hace un MOVE por color en el borde horizontal y no le entran muchos; el
 * limite esta medido en DECISIONS.md. */
static int g_band_colors = 0;

/* La paleta con que se ve la fila logica y. */
static const A5Palette *rowpal(const A5Palette *pals, int y)
{
    return pals + a5v_band_of(y, g_band_rows, g_band_y0, g_nbands);
}

static void die(const char *msg)
{
    fprintf(stderr, "error: %s\n", msg);
    exit(1);
}

static int even(int v) { return v & ~1; }

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *p;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = malloc(n > 0 ? (size_t)n : 1);
    if (!p || n < 0 || fread(p, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(p); return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return p;
}

/* ====================================================================
 * Audio
 * ==================================================================== */

typedef struct {
    int      format;       /* A5V_AUDIO_*; NONE si no hay audio */
    int      period;
    double   hz;           /* 3546895 / periodo, exacto */
    size_t   nsamples;     /* S(nframes-1): lo que suena en todo el video */
    uint8_t *bytes;        /* el flujo codificado, todos los paquetes seguidos */
    size_t   nbytes;
    int8_t  *recon;        /* lo que va a sonar, muestra por muestra */
    double   snr;          /* dB, lo que suena contra la entrada */
    double   peak;         /* pico de la entrada en escala de 8 bits */
    long     clipped;      /* muestras recortadas por la ganancia */
} A5Audio;

/* Lee el audio de la fuente y lo codifica entero como un solo flujo.
 *
 * ffmpeg no puede entregar 8006,535 Hz: solo frecuencias enteras. Se le pide
 * la entera de arriba (con su filtro antialias, que es bueno) y el ultimo
 * paso lo hace una interpolacion lineal a la frecuencia EXACTA de Paula. La
 * razon es 1,00006: la interpolacion casi no filtra y no hay deriva.
 *
 * speed es cuanto mas rapido corre el video que la fuente (1 en native). En
 * pal el audio se toma mas rapido y sube de tono; se le pide a ffmpeg una
 * frecuencia mas baja en la misma proporcion para que su antialias corte
 * donde corresponde despues de acelerar. */
static void encode_audio(A5Audio *au, const char *in, double start,
                         double duration, double speed, size_t nframes,
                         double gain, const char *afilter)
{
    int rate = (int)ceil(au->hz / speed);
    double step = speed * rate / au->hz;   /* muestras de ffmpeg por muestra */
    FILE *f = a5_open_audio(in, start, duration, afilter, rate);
    int16_t *raw = NULL;
    size_t nraw = 0, cap = 0, i;
    float *x;
    double sig = 0, noise = 0;
    int acc = 0;
    uint8_t b2[2];

    if (!f) die("no pude arrancar ffmpeg para el audio");
    while (fread(b2, 1, 2, f) == 2) {
        if (nraw == cap) {
            cap = cap ? cap * 2 : 65536;
            raw = realloc(raw, cap * sizeof *raw);
            if (!raw) die("sin memoria");
        }
        raw[nraw++] = (int16_t)(b2[0] | (b2[1] << 8));   /* s16le */
    }
    a5_pclose(f);

    au->nsamples = a5_audio_samples_through((uint32_t)nframes - 1, au->hz);
    x = malloc(au->nsamples * sizeof *x);
    au->recon = malloc(au->nsamples ? au->nsamples : 1);
    au->nbytes = a5_audio_bytes(au->format, au->nsamples);
    au->bytes = calloc(au->nbytes ? au->nbytes : 1, 1);
    if (!x || !au->recon || !au->bytes) die("sin memoria");

    /* Si la fuente se acaba antes que el video, el resto es silencio. */
    au->peak = 0;
    au->clipped = 0;
    for (i = 0; i < au->nsamples; i++) {
        double pos = i * step, fr, a, b, v;
        size_t k = (size_t)pos;
        fr = pos - (double)k;
        a = k < nraw ? raw[k] : 0;
        b = k + 1 < nraw ? raw[k + 1] : 0;
        v = (a + (b - a) * fr) / 256.0 * gain;
        if (fabs(v) > au->peak) au->peak = fabs(v);
        if (v > 127)  { v = 127;  au->clipped++; }
        if (v < -128) { v = -128; au->clipped++; }
        x[i] = (float)v;
    }
    free(raw);

    if (au->format == A5V_AUDIO_FIB4) {
        a5_fib4_encode(x, au->nsamples, au->bytes, au->recon, &acc);
    } else if (au->format == A5V_AUDIO_ADPCM) {
        /* IMA ADPCM trabaja en 16 bits; lo que suena es el byte alto del
         * predictor, porque Paula es de 8. Por eso la entrada se sube a la
         * escala de 16 bits y la reconstruccion vuelve en 8. */
        A5AdpcmState st = { 0, 0 };
        int16_t *w = malloc((au->nsamples ? au->nsamples : 1) * sizeof *w);
        if (!w) die("sin memoria");
        for (i = 0; i < au->nsamples; i++) {
            long v = lround(x[i] * 256.0);
            if (v > 32767)  v = 32767;
            if (v < -32768) v = -32768;
            w[i] = (int16_t)v;
        }
        a5_adpcm_encode(w, au->nsamples, au->bytes, au->recon, &st, 1);
        free(w);
    } else {
        for (i = 0; i < au->nsamples; i++) {
            long v = lround(x[i]);
            if (v > 127) v = 127;
            if (v < -128) v = -128;
            au->recon[i] = (int8_t)v;
            au->bytes[i] = (uint8_t)(int8_t)v;
        }
    }

    for (i = 0; i < au->nsamples; i++) {
        double e = x[i] - au->recon[i];
        sig += (double)x[i] * x[i];
        noise += e * e;
    }
    au->snr = noise > 0 ? 10 * log10(sig / noise) : 99;
    free(x);
}

/* Los bytes de audio del paquete n: las muestras S(n-1)..S(n)-1. */
static const uint8_t *audio_slice(const A5Audio *au, size_t n, size_t *len)
{
    size_t a, b;

    if (!au || au->format == A5V_AUDIO_NONE) { *len = 0; return NULL; }
    a = n ? a5_audio_samples_through((uint32_t)n - 1, au->hz) : 0;
    b = a5_audio_samples_through((uint32_t)n, au->hz);
    a = a5_audio_bytes(au->format, a);
    b = a5_audio_bytes(au->format, b);
    *len = b - a;
    return au->bytes + a;
}

/* ====================================================================
 * Bitstream
 * ==================================================================== */

typedef struct {
    A5Buf  buf;             /* solo los paquetes, sin cabecera */
    int    npackets, nrepeat, ndelta, npalette;
    int    ncopy;           /* deltas que piden la copia del visible (H12) */
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
    double src_err;         /* error de lo que se ve contra la fuente, sumado */
    long   src_bad;         /* pixeles que se ven a mas de A5_VISIBLE_ERR */
} A5Stream;

/* Checksum de un frame tal como se ve: los indices mas la paleta con la que
 * se muestran. El decoder de referencia recalcula esto y tiene que dar lo
 * mismo, byte por byte. */
static uint32_t frame_crc(const uint8_t *vis, size_t fsz,
                          const A5Palette *pals, int ncolors)
{
    static uint8_t pb[A5_MAX_BANDS * A5_MAX_COLORS * 2];
    int b, c, k = 0;

    for (b = 0; b < g_nbands; b++)
        for (c = 0; c < ncolors; c++) {
            pb[k++] = (uint8_t)(pals[b].rgb444[c] >> 8);
            pb[k++] = (uint8_t)pals[b].rgb444[c];
        }
    return a5_crc32(pb, (size_t)k, a5_crc32(vis, fsz, 0));
}

/* Error perceptual medio entre dos mapas de indices, solo en el area activa.
 * pa y pb son juegos de paletas por franja. */
static double idx_error(const uint8_t *a, const uint8_t *b,
                        const A5Palette *pa, const A5Palette *pb,
                        int y0, int y1)
{
    size_t n = (size_t)(y1 - y0) * A5_W;
    double sum = 0;
    int y, x;

    if (!n) return 0;
    for (y = y0; y < y1; y++) {
        const A5Palette *ra = rowpal(pa, y), *rb = rowpal(pb, y);
        const uint8_t *qa = a + (size_t)y * A5_W, *qb = b + (size_t)y * A5_W;
        for (x = 0; x < A5_W; x++)
            if (qa[x] != qb[x])
                sum += sqrt((double)a5_oklab_dist2(ra->lab[qa[x]],
                                                   rb->lab[qb[x]]));
    }
    return sum / n;
}

/* Pixeles del area activa cuyo error se ve. El error medio esconde el
 * salpicado: 0,047 de media puede ser un 20% de pixeles con 0,2 de error.
 * 0,1 en Oklab es un cambio de color que se nota sin buscarlo. */
#define A5_VISIBLE_ERR  0.1

static long idx_bad(const uint8_t *a, const uint8_t *b, const A5Palette *pals,
                    int y0, int y1)
{
    long bad = 0;
    int y, x;

    for (y = y0; y < y1; y++) {
        const A5Palette *pal = rowpal(pals, y);
        const uint8_t *pa = a + (size_t)y * A5_W, *pb = b + (size_t)y * A5_W;
        for (x = 0; x < A5_W; x++)
            if (pa[x] != pb[x] &&
                a5_oklab_dist2(pal->lab[pa[x]], pal->lab[pb[x]]) >
                    A5_VISIBLE_ERR * A5_VISIBLE_ERR)
                bad++;
    }
    return bad;
}

/* Error contra la fuente de lo que se ve. Es la medida que sirve para
 * comparar paletas distintas: el error contra el frame cuantizado ideal no,
 * porque el ideal cambia con la paleta. */
static void src_account(A5Stream *st, const uint8_t *vis,
                        const A5Palette *pals, const uint8_t *rgb,
                        int y0, int y1)
{
    int y, x;

    for (y = y0; y < y1; y++) {
        const A5Palette *pal = rowpal(pals, y);
        const uint8_t *p = rgb + (size_t)y * A5_W * 3;
        const uint8_t *v = vis + (size_t)y * A5_W;
        for (x = 0; x < A5_W; x++, p += 3) {
            double d = sqrt((double)a5_oklab_dist2(
                a5_srgb_to_oklab(p[0], p[1], p[2]), pal->lab[v[x]]));
            st->src_err += d;
            if (d > A5_VISIBLE_ERR) st->src_bad++;
        }
    }
}

/* Costuras entre franjas: una zona lisa que cruza el borde cae en un tono
 * de un lado y en otro casi igual del otro, y se ve una raya horizontal. Un
 * color a menos de thr de uno de la franja de arriba pasa a ser exactamente
 * ese; cada color de arriba se usa una vez. */
static void snap_to(A5Palette *bp, const A5Palette *up, double thr)
{
    int c, u, taken[A5_MAX_COLORS] = { 0 };

    if (thr <= 0) return;
    for (c = 1; c < bp->n; c++) {
        int best = -1;
        float bd = (float)(thr * thr);
        for (u = 1; u < up->n; u++) {
            float d = a5_oklab_dist2(bp->lab[c], up->lab[u]);
            if (!taken[u] && d < bd) { bd = d; best = u; }
        }
        if (best >= 0) {
            taken[best] = 1;
            bp->rgb444[c] = up->rgb444[best];
            bp->lab[c] = up->lab[best];
        }
    }
}

/* --- H12: de donde se predice cada delta --------------------------------
 * El delta va normalmente contra el buffer OCULTO, que es el penultimo
 * frame distinto: con --min-hold 2 son 160 ms de movimiento. Si el paquete
 * pide la copia (A5V_F_COPY), el reproductor copia con el Blitter el area
 * activa del buffer VISIBLE al oculto antes de aplicar el delta, y
 * entonces se predice desde 80 ms.
 *
 * La copia no cuesta bytes (es un bit del flag) pero cuesta tiempo:
 * A5_CYC_BLIT por plano y por fila activa, medido en WinUAE. Por eso la
 * eleccion es por frame y mira las dos cosas: en un corte de escena el
 * visible no predice nada y la copia seria tiempo tirado.
 *
 * El vector de movimiento global que la hoja de ruta proponia (correr la
 * copia (dx,dy) con el barrel shifter del Blitter) se midio el 2026-09-12 y
 * ahorraba 72 bytes en todo el video: no se implemento. El detalle esta en
 * DECISIONS.md. */
#define A5_PRED_HIDDEN   0
#define A5_PRED_VISIBLE  1
#define A5_PRED_AUTO     2
static int g_predict = A5_PRED_AUTO;

/* El buffer contra el que se codifica cuando el paquete pide la copia: el
 * oculto con el area activa del visible encima, que es exactamente lo que
 * deja el blit del reproductor (un solo rectangulo, sin tocar el borde). */
static void predict_copy(uint8_t *pred, const uint8_t *hid,
                         const uint8_t *vis, int y0, int y1)
{
    memcpy(pred, hid, (size_t)A5_W * A5_H);
    if (y1 > y0)
        memcpy(pred + (size_t)y0 * A5_W, vis + (size_t)y0 * A5_W,
               (size_t)(y1 - y0) * A5_W);
}

/* Calidad con perdida: se deja sin actualizar todo pixel cuyo error, medido
 * contra el estado YA DECODIFICADO, quede por debajo del umbral. Despues, si
 * lo que queda de una fila es despreciable, se descarta la fila entera para
 * ahorrarse tambien su cabecera. */
static void apply_quality(uint8_t *target, const uint8_t *hidden,
                          const A5Palette *pals, int y0, int y1,
                          double pixel_thr, double row_thr)
{
    int y, x;

    if (pixel_thr <= 0) return;

    for (y = y0; y < y1; y++) {
        const A5Palette *pal = rowpal(pals, y);
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
 * cost ciclos, si le toca verse en due. due esta sobre la grilla de VBL.
 *
 * Los llenados de audio llegan cada fillp ciclos desde el VBL del frame 0 y
 * cada uno le roba fillc al delta; al alargarlo puede meter otro. */
static int late_vbls(double start, long cost, double due, double P,
                     double fillp, double fillc)
{
    double end;

    /* Si al empezar hay un llenado en curso, el delta espera a que termine:
     * el lazo principal no corre hasta entonces. */
    if (fillc > 0 && start >= 0) {
        double k = floor(start / fillp);
        if (k * fillp + fillc > start) start = k * fillp + fillc;
    }
    end = start + (double)cost;
    if (fillc > 0) {
        double k = ceil(start / fillp);
        if (k < 0) k = 0;
        while (k * fillp < end) { end += fillc; k++; }
    }
    if (end <= due) return 0;
    return (int)ceil((end - due) / P);
}

static void put_packet(A5Stream *st, int op, const A5Palette *pal, int ncolors,
                       const A5Buf *video, const uint8_t *audio,
                       size_t audio_len, int flags)
{
    size_t start = st->buf.len;
    size_t len = 6 + (pal ? (size_t)a5v_palette_words(ncolors, g_nbands,
                                                     g_band_colors) * 2 : 0)
               + audio_len + (video ? video->len : 0);
    int b, c;

    if (len & 1) len++;                 /* los paquetes quedan pares */

    a5buf_put16(&st->buf, (unsigned)len);
    a5buf_put8(&st->buf, (unsigned)op);
    a5buf_put8(&st->buf, (unsigned)(flags | (pal ? A5V_F_PALETTE : 0)));
    a5buf_put16(&st->buf, (unsigned)audio_len);

    if (pal) {
        /* La franja 0 entera; las demas, solo lo que cambia respecto de la
         * de arriba, como pares (indice, color). Los pares que sobran van
         * con indice 0, que el reproductor traduce a un MOVE al revive
         * ($01FE): el color 0 nunca cambia entre franjas. */
        for (c = 0; c < ncolors; c++)
            a5buf_put16(&st->buf, pal[0].rgb444[c]);
        for (b = 1; b < g_nbands; b++) {
            int k = 0, ndiff = 0;
            for (c = 1; c < ncolors; c++)
                if (pal[b].rgb444[c] != pal[b - 1].rgb444[c]) ndiff++;
            /* Si la franja cambia mas de lo que el Copper escribe, el
             * paquete no puede llevarlo y el reproductor mostraria otra
             * cosa que la que simulo el encoder. Es un bug del cuantizador,
             * no algo para truncar en silencio. */
            if (ndiff > g_band_colors)
                die("la franja cambia mas colores de los que entran "
                    "(bug del cuantizador)");
            for (c = 1; c < ncolors && k < g_band_colors; c++)
                if (pal[b].rgb444[c] != pal[b - 1].rgb444[c]) {
                    a5buf_put16(&st->buf, (unsigned)c);
                    a5buf_put16(&st->buf, pal[b].rgb444[c]);
                    k++;
                }
            for (; k < g_band_colors; k++) {
                a5buf_put16(&st->buf, 0);
                a5buf_put16(&st->buf, 0);
            }
        }
    }
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

/* --- H12: un candidato de delta -----------------------------------------
 * Codificar un frame es elegir entre dos: el delta contra el oculto, y la
 * copia del visible mas el delta contra eso. Cada uno se prueba con su
 * propia degradacion, porque el que predice mejor puede no necesitarla. */
typedef struct {
    A5Buf        video;
    uint8_t     *tgt;        /* el frame que queda en el buffer, ya con perdida */
    A5DeltaStats ds;
    long         cycles;     /* decodificacion: la copia mas el delta */
    int          late;       /* VBL de atraso previstos */
    int          tries;      /* veces que hubo que degradar */
    int          copy;       /* pide A5V_F_COPY */
} Cand;

/* Codifica el delta de base a ideal, degradando mientras no llegue a
 * tiempo. extra son los ciclos que el reproductor gasta antes del delta
 * (la copia con el Blitter, o 0). */
static void try_delta(Cand *c, const uint8_t *base, const uint8_t *ideal,
                      const A5Palette *pal, int planes, int y0, int y1,
                      double thr, long extra, long cyc_limit, int max_late,
                      double t_free, double due, double P,
                      double fillp, double fillc)
{
    size_t fsz = (size_t)A5_W * A5_H;
    int attempt;

    for (attempt = 0; attempt < 6; attempt++) {
        int capped;
        memcpy(c->tgt, ideal, fsz);
        apply_quality(c->tgt, base, pal, y0, y1, thr, thr);
        c->video.len = 0;
        a5_delta_encode(&c->video, base, c->tgt, planes, &c->ds);
        c->cycles = c->ds.cycles + extra;
        c->late = late_vbls(t_free, c->cycles, due, P, fillp, fillc);
        capped = !cyc_limit || c->cycles <= cyc_limit;
        if (capped && c->late <= max_late) break;
        /* Si ni un delta vacio llega a tiempo, degradar no arregla nada. */
        if (capped && late_vbls(t_free, extra, due, P, fillp, fillc) > max_late)
            break;
        thr = thr > 0 ? thr * 2 : 0.01;
    }
    c->tries = attempt;
}

/* Cual de los dos conviene: primero llegar a tiempo, despues no degradar,
 * y a igualdad de las dos, el que gasta menos bytes. */
static int cand_better(const Cand *a, const Cand *b)
{
    if (a->late != b->late)   return a->late < b->late;
    if (a->tries != b->tries) return a->tries < b->tries;
    return a->video.len < b->video.len;
}

/* Arma el bitstream entero. idx tiene nframes mapas de A5_W*A5_H indices. */
static void build_stream(A5Stream *st, const uint8_t *idx, size_t nframes,
                         const Scene *scenes, int nscenes, int planes,
                         int ncolors, int y0, int y1,
                         double pixel_thr, double repeat_boost, long cyc_limit,
                         int min_hold, int max_late, const A5Audio *au,
                         const uint8_t *const *src)
{
    size_t fsz = (size_t)A5_W * A5_H;
    uint8_t *vis = calloc(fsz, 1);
    uint8_t *hid = calloc(fsz, 1);
    uint8_t *tgt = malloc(fsz);
    uint8_t *pred = malloc(fsz);
    Cand ca = {{0, 0, 0}, NULL, {0, 0, 0, 0, 0}, 0, 0, 0, 0};
    Cand cb = {{0, 0, 0}, NULL, {0, 0, 0, 0, 0}, 0, 0, 0, 1};
    /* Lo que cuesta la copia con el Blitter: un blit por plano sobre las
     * filas activas, con la CPU esperandolo (medido, ver stream.h). */
    long copy_cyc = a5_cyc_blit(planes) * planes * (y1 - y0);
    size_t n;
    int s = 0;
    const A5Palette *vispal = NULL;
    size_t last_delta = 0;
    /* Linea de tiempo del reproductor, en ciclos de CPU, con el origen en el
     * VBL del frame 0. El reproductor empieza a decodificar 2 VBL antes. */
    double P = A5_CYC_PER_VBL;
    double t_free = -2.0 * P;
    int afmt = au ? au->format : A5V_AUDIO_NONE;
    double fillp = afmt ? 2.0 * A5_AUD_BUF_SAMPLES * au->period : 1.0;
    double fillc = (double)a5_audio_fill_cost(afmt);

    ca.tgt = malloc(fsz);
    cb.tgt = malloc(fsz);
    if (!vis || !hid || !tgt || !pred || !ca.tgt || !cb.tgt)
        die("sin memoria");
    memset(st, 0, sizeof *st);
    a5buf_init(&st->buf);
    a5buf_init(&ca.video);
    a5buf_init(&cb.video);
    st->crc = malloc(nframes * sizeof *st->crc);
    if (!st->crc) die("sin memoria");

    for (n = 0; n < nframes; n++) {
        const A5Palette *pal;
        const uint8_t *ideal = idx + n * fsz;
        int newscene;
        Cand *best;
        double thr = pixel_thr;
        double due = 2.0 * (double)n * P;    /* VBL en que se ve este frame */
        int late = 0;
        size_t alen;
        const uint8_t *ab = audio_slice(au, n, &alen);

        while (s + 1 < nscenes && (int)n >= scenes[s + 1].start) s++;
        pal = scenes[s].pal;
        newscene = ((int)n == scenes[s].start);

        /* Tope de cadencia: la imagen se actualiza como mucho cada min_hold
         * huecos. Un corte de escena siempre pasa. */
        if (!newscene && vispal == pal && min_hold > 1 &&
            n - last_delta < (size_t)min_hold) {
            put_packet(st, A5V_OP_REPEAT, NULL, ncolors, NULL, ab, alen, 0);
            st->nrepeat++;
            st->err_sum += idx_error(vis, ideal, pal, pal, y0, y1);
            st->bad_pixels += idx_bad(vis, ideal, pal, y0, y1);
            st->crc[n] = frame_crc(vis, fsz, pal, ncolors);
            if (src) src_account(st, vis, pal, src[n], y0, y1);
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
                put_packet(st, A5V_OP_REPEAT, NULL, ncolors, NULL, ab, alen, 0);
                st->nrepeat++;
                st->err_sum += idx_error(vis, ideal, pal, pal, y0, y1);
                st->bad_pixels += idx_bad(vis, ideal, pal, y0, y1);
                st->crc[n] = frame_crc(vis, fsz, pal, ncolors);
                if (src) src_account(st, vis, pal, src[n], y0, y1);
                continue;
            }
        }

        /* Delta contra el buffer oculto (el penultimo frame distinto) o,
         * si conviene, copia del visible mas delta contra eso (H12).
         *
         * Linea de tiempo del reproductor, medida en el Hito 4: el delta se
         * empieza a decodificar cuando el intercambio anterior libera el
         * buffer oculto (t_free) y tiene que estar listo para el VBL en que
         * le toca verse. Si no llega, se ve tarde y las repeticiones que
         * siguen absorben el atraso. Se degrada (subiendo el umbral) solo si
         * se pasaria de max_late VBL: un frame degradado deja salpicado, que
         * se ve bastante peor que un frame que llega 20 o 40 ms tarde.
         *
         * Los dos candidatos dejan el mismo contenido en los buffers (el
         * delta escribe tgt entero), asi que la eleccion es frame por frame
         * y no arrastra estado. */
        best = &ca;
        if (g_predict != A5_PRED_VISIBLE)
            try_delta(&ca, hid, ideal, pal, planes, y0, y1, thr, 0,
                      cyc_limit, max_late, t_free, due, P, fillp, fillc);
        if (g_predict != A5_PRED_HIDDEN) {
            predict_copy(pred, hid, vis, y0, y1);
            try_delta(&cb, pred, ideal, pal, planes, y0, y1, thr, copy_cyc,
                      cyc_limit, max_late, t_free, due, P, fillp, fillc);
            if (g_predict == A5_PRED_VISIBLE || cand_better(&cb, &ca))
                best = &cb;
        }
        late = best->late;

        if (best->tries > 0) st->degraded++;
        if (best->copy) st->ncopy++;
        if (cyc_limit && best->cycles > cyc_limit) st->over_budget++;
        if (best->cycles > st->maxcycles) {
            st->maxcycles = best->cycles;
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
                   &best->video, ab, alen, best->copy ? A5V_F_COPY : 0);
        st->ndelta++;
        if (newscene) st->npalette++;
        last_delta = n;

        memcpy(hid, best->tgt, fsz);
        { uint8_t *t = vis; vis = hid; hid = t; }
        vispal = pal;

        st->err_sum += idx_error(vis, ideal, pal, pal, y0, y1);
        st->bad_pixels += idx_bad(vis, ideal, pal, y0, y1);
        st->crc[n] = frame_crc(vis, fsz, pal, ncolors);
        if (src) src_account(st, vis, pal, src[n], y0, y1);
    }

    a5buf_free(&ca.video);
    a5buf_free(&cb.video);
    free(vis); free(hid); free(tgt); free(pred);
    free(ca.tgt); free(cb.tgt);
}

/* ------------------------------------------------------------------ */

/* --- control de tasa -----------------------------------------------------
 * Codifica con un umbral de perdida y se queda con el resultado si entra en
 * el presupuesto y ademas es mejor que el que ya tenia. Guardar el mejor
 * medido, en vez del umbral mas chico que entro, es lo que hace que la
 * busqueda binaria no se pierda cuando el umbral no es monotono en bytes.
 * Si no entra ninguno, conserva el mas chico. */
typedef struct {
    const uint8_t *idx;
    size_t         nframes;
    const Scene   *scenes;
    int            nscenes, planes, ncolors, y0, y1;
    double         repeat_boost;
    long           cyc_limit;
    int            min_hold, max_late;
    const A5Audio *au;
    long           budget;

    A5Stream best;          /* el elegido hasta ahora */
    int       have, fits;
    double    bestthr;
    size_t    besttotal;
} RateCtl;

static size_t rc_try(RateCtl *rc, double thr)
{
    A5Stream s;
    size_t total;
    int fits, better;

    build_stream(&s, rc->idx, rc->nframes, rc->scenes, rc->nscenes,
                 rc->planes, rc->ncolors, rc->y0, rc->y1, thr,
                 rc->repeat_boost, rc->cyc_limit, rc->min_hold, rc->max_late,
                 rc->au, NULL);
    total = A5V_HEADER_SIZE + s.buf.len;
    fits = rc->budget <= 0 || total <= (size_t)rc->budget;

    if (!rc->have)            better = 1;
    else if (fits && !rc->fits) better = 1;          /* entrar manda */
    else if (fits)            better = s.err_sum < rc->best.err_sum;
    else                      better = !rc->fits && total < rc->besttotal;

    if (better) {
        if (rc->have) { a5buf_free(&rc->best.buf); free(rc->best.crc); }
        rc->best = s; rc->have = 1; rc->fits = fits;
        rc->bestthr = thr; rc->besttotal = total;
    } else {
        a5buf_free(&s.buf); free(s.crc);
    }
    return total;
}

static void usage(void)
{
    printf(
"uso: a500vp-enc --in <video> [opciones]\n"
"\n"
"  --in PATH               video fuente (cualquier cosa que lea ffmpeg)\n"
"  --out PATH              bitstream de salida (work\\video.a5v)\n"
"  --preview PATH          mp4 opcional con la cuantizacion sin comprimir\n"
"                          (el preview de verdad lo hace a500vp-dec)\n"
"  --adf PATH              ademas, el disco entero listo para bootear\n"
"  --boot PATH             bootblock para --adf (work\\boot.bin)\n"
"  --player PATH           reproductor para --adf (work\\player.bin)\n"
"  --reserve-tail N        sectores del final del disco que quedan libres (0)\n"
"  --quality N             perdida permitida, 0 = sin perdida (8)\n"
"  --budget BYTES          tope de bytes del bitstream, audio incluido;\n"
"                          0 = sin tope (por defecto: lo que queda en el disco\n"
"                          despues del reproductor, o 883712 sin --adf)\n"
"  --audio-format F        fib4 | adpcm | pcm8 | none (fib4). adpcm usa los\n"
"                          mismos 4 bits que fib4 pero con paso adaptativo:\n"
"                          sigue los agudos, y cuesta mas CPU al reproducir\n"
"  --audio-rate HZ         frecuencia aproximada; se usa el periodo entero mas\n"
"                          cercano y su frecuencia exacta (8006,5)\n"
"  --audio-period N        periodo de Paula, en lugar de --audio-rate (443)\n"
"  --audio-gain F          ganancia antes de pasar a 8 bits (1.0)\n"
"  --audio-channel C       mix | left | right (mix). mix suma los canales y\n"
"                          borra lo que esta en contrafase; left o right\n"
"                          lo conserva\n"
"  --repeat-boost F        cuanto mas permisivo es repetir que actualizar (1.5)\n");
    /* Partido en dos: un solo literal pasaba los 4095 caracteres que C99
     * obliga a soportar, y -pedantic avisa. */
    printf(
"  --predict MODO          contra que se codifica el delta: auto (el\n"
"                          encoder elige por frame, mirando bytes y\n"
"                          milisegundos), hidden (siempre el penultimo\n"
"                          frame distinto) o visible (siempre el ultimo,\n"
"                          copiandolo antes con el Blitter). auto\n"
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
"  --planes N              2 a 5 bitplanes = 4, 8, 16 o 32 colores (3).\n"
"                          Con mas de 3 no hay franjas de paleta: el Copper\n"
"                          no llega a cambiarlas antes de la linea\n"
"  --band-rows N           paleta por franjas de N filas logicas, cambiada\n"
"                          por el Copper; 0 = una sola paleta. 16 con 3\n"
"                          planos o menos; con mas hay que pedirlo\n"
"  --band-colors N         colores que cada franja puede cambiar respecto\n"
"                          de la de arriba; el Copper no llega a mas de 8\n"
"                          antes de que empiece la linea (colores-1, o 8)\n"
"  --band-overlap N        filas de las franjas vecinas que entran al\n"
"                          histograma de cada franja (0)\n"
"  --band-snap F           un color a menos de F (Oklab) de uno de la franja\n"
"                          de arriba se vuelve ese mismo: quita costuras en\n"
"                          zonas lisas (0.04)\n"
"  --dither MODO           none | bayer2 | bayer4 (none)\n"
"  --dither-strength F     fuerza del dither ordenado (0.05)\n"
"  --sharpen F             realce de bordes tras escalar; 0 = nada (1.2)\n"
"  --denoise F             denoise temporal antes de escalar; 0 = nada (0)\n"
"  --scene-threshold F     distancia Oklab media que dispara un corte (0.12)\n"
"  --min-scene N           frames minimos por escena (6)\n"
"  --preview-scale N       ampliacion del preview (2 = 640x512)\n"
"  --no-audio              sin audio, ni en el disco ni en el preview\n"
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
    long   budget = -1;             /* -1 = lo que queda en el disco */
    int    audio_period = 443;
    int    audio_format = -1;       /* -1 = fib4 si la fuente tiene audio */
    double audio_gain = 1.0;
    const char *audio_filter = NULL;   /* NULL = mezcla L+R (-ac 1) */
    const char *adf_path = NULL;
    const char *boot_path = "work\\boot.bin", *player_path = "work\\player.bin";
    int    reserve_tail = 0;
    int    band_rows = 16, band_overlap = 0, band_rows_set = 0;
    int    band_colors = 0, band_colors_set = 0;
    double band_snap = 0.04;           /* Hito 6: medidos y mirados */
    uint8_t *boot = NULL, *player = NULL;
    size_t bootlen = 0, playerlen = 0;
    A5Audio au;
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
        else if (!strcmp(a, "--audio-rate") && has) {
            double r = atof(argv[++i]);
            if (r <= 0) die("--audio-rate tiene que ser positivo");
            audio_period = (int)(A5_CCK_PAL / r + 0.5);
        }
        else if (!strcmp(a, "--audio-gain") && has)    audio_gain = atof(argv[++i]);
        else if (!strcmp(a, "--audio-channel") && has) {
            /* Por indice y no por nombre (FL/FR): asi left anda tambien con
             * una fuente mono, donde el unico canal es FC. */
            const char *v = argv[++i];
            if (!strcmp(v, "mix"))        audio_filter = NULL;
            else if (!strcmp(v, "left"))  audio_filter = "pan=mono|c0=c0";
            else if (!strcmp(v, "right")) audio_filter = "pan=mono|c0=c1";
            else die("--audio-channel: mix, left o right");
        }
        else if (!strcmp(a, "--audio-format") && has) {
            const char *v = argv[++i];
            if (!strcmp(v, "fib4"))      audio_format = A5V_AUDIO_FIB4;
            else if (!strcmp(v, "pcm8")) audio_format = A5V_AUDIO_PCM8;
            else if (!strcmp(v, "adpcm")) audio_format = A5V_AUDIO_ADPCM;
            else if (!strcmp(v, "none")) audio_format = A5V_AUDIO_NONE;
            else die("--audio-format: fib4, adpcm, pcm8 o none");
        }
        else if (!strcmp(a, "--adf") && has)           adf_path = argv[++i];
        else if (!strcmp(a, "--boot") && has)          boot_path = argv[++i];
        else if (!strcmp(a, "--player") && has)        player_path = argv[++i];
        else if (!strcmp(a, "--reserve-tail") && has)  reserve_tail = atoi(argv[++i]);
        else if (!strcmp(a, "--band-rows") && has) {
            band_rows = atoi(argv[++i]);
            band_rows_set = 1;
        }
        else if (!strcmp(a, "--band-overlap") && has)  band_overlap = atoi(argv[++i]);
        else if (!strcmp(a, "--band-colors") && has) {
            band_colors = atoi(argv[++i]); band_colors_set = 1;
        }
        else if (!strcmp(a, "--band-snap") && has)     band_snap = atof(argv[++i]);
        else if (!strcmp(a, "--predict") && has) {
            const char *v = argv[++i];
            if (!strcmp(v, "hidden"))       g_predict = A5_PRED_HIDDEN;
            else if (!strcmp(v, "visible")) g_predict = A5_PRED_VISIBLE;
            else if (!strcmp(v, "auto"))    g_predict = A5_PRED_AUTO;
            else die("--predict: auto, hidden o visible");
        }
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
    if (planes < 1 || planes > 5) die("--planes tiene que ser 1..5");
    if (audio_period < 124 || audio_period > 65535)
        die("periodo de audio fuera de rango (124..65535; Paula no baja de 124)");
    if (reserve_tail < 0) die("--reserve-tail no puede ser negativo");
    cyc_limit = (long)(A5_CPU_HZ * frame_ms / 1000.0);
    ncolors = 1 << planes;

    /* El reproductor se lee antes de codificar: su tamano decide cuanto disco
     * queda para los datos. */
    if (adf_path) {
        boot = read_file(boot_path, &bootlen);
        player = read_file(player_path, &playerlen);
        if (!boot) die("no pude leer el bootblock (--boot)");
        if (!player) die("no pude leer el reproductor (--player)");
    }

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

    /* --- franjas de paleta ------------------------------------------ */
    if (band_rows < 0 || band_rows > A5_H) die("--band-rows fuera de rango");
    if (band_overlap < 0) die("--band-overlap no puede ser negativo");
    if (planes > 3 && !band_rows_set) band_rows = 0;   /* hay que pedirlas */
    g_nbands = a5v_nbands(band_rows, y0, y1);
    g_band_rows = g_nbands > 1 ? band_rows : 0;
    g_band_y0 = y0;
    /* Cuantos colores puede cambiar cada franja. El Copper hace un MOVE por
     * color entre el WAIT (hpos $06) y el principio de la imagen, y no le
     * entran todos con mas de 8 colores: el tope medido esta en
     * DECISIONS.md. Con pocos colores, todos. */
    if (!band_colors_set)
        band_colors = ncolors - 1 <= A5_BAND_COLORS_MAX
                    ? ncolors - 1 : A5_BAND_COLORS_MAX;
    if (g_nbands > 1) {
        if (band_colors < 1 || band_colors > ncolors - 1)
            die("--band-colors tiene que estar entre 1 y colores-1");
        if (band_colors > A5_BAND_COLORS_MAX)
            printf("AVISO: %d colores por franja pasan los %d que el Copper "
                   "alcanza a escribir; los ultimos van a llegar tarde\n",
                   band_colors, A5_BAND_COLORS_MAX);
    } else {
        band_colors = 0;
    }
    g_band_colors = band_colors;
    if (g_nbands > 1)
        printf("paleta     : %d franjas de %d filas logicas, %d colores por "
               "franja (solapamiento %d)\n", g_nbands, band_rows,
               band_colors, band_overlap);

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
    else {
        /* Una fuente mas rapida que 24,96 fps no repite frames: descarta.
         * La resta sin signo informaba "4294967295 frames repetidos". */
        long extra = (long)nframes - (long)nsrc;
        printf("             cadencia native: %.3f s de fuente en %.3f s, "
               "%ld frames %s, audio intacto\n",
               nsrc / src.fps, nframes / A5_VIDEO_FPS,
               extra < 0 ? -extra : extra,
               extra < 0 ? "descartados" : "repetidos");
    }

    /* --- audio -------------------------------------------------------
     * Va dentro de los paquetes, asi que el presupuesto es uno solo: cabecera
     * mas paquetes, audio incluido. El audio no se negocia con el control de
     * tasa; lo que se ajusta es el video. */
    memset(&au, 0, sizeof au);
    if (audio_format < 0)
        audio_format = want_audio && src.audio_rate ? A5V_AUDIO_FIB4
                                                    : A5V_AUDIO_NONE;
    if (!want_audio) audio_format = A5V_AUDIO_NONE;
    if (audio_format != A5V_AUDIO_NONE && !src.audio_rate) {
        printf("  AVISO: la fuente no tiene audio; el disco sale mudo\n");
        audio_format = A5V_AUDIO_NONE;
    }
    au.format = audio_format;
    au.period = audio_period;
    au.hz = A5_CCK_PAL / audio_period;
    if (au.format != A5V_AUDIO_NONE) {
        encode_audio(&au, in, start, duration,
                     pal_speedup ? A5_VIDEO_FPS / src.fps : 1.0, nframes,
                     audio_gain, audio_filter);
        printf("audio      : %s, periodo %d = %.3f Hz, %lu muestras (%.3f s), "
               "%lu bytes = %.2f KB/s\n",
               au.format == A5V_AUDIO_FIB4 ? "fib4"
               : au.format == A5V_AUDIO_ADPCM ? "adpcm" : "pcm8", au.period, au.hz,
               (unsigned long)au.nsamples, au.nsamples / au.hz,
               (unsigned long)au.nbytes,
               au.nbytes / 1024.0 / (nframes / A5_VIDEO_FPS));
        printf("             ganancia %.2f, pico %.1f de 127, %ld muestras "
               "recortadas, SNR %.1f dB\n", audio_gain, au.peak, au.clipped,
               au.snr);
    }

    /* Presupuesto por defecto: con --adf, exactamente lo que queda en el
     * disco despues del bootblock, el reproductor y la cola reservada. La
     * memoria no limita: medida en el Hito 0, entre Chip y slow sobran ~40 KB
     * (docs/DECISIONS.md). */
    if (budget < 0) {
        if (adf_path) {
            long sectors = ADF_SECTORS - ADF_BOOT_SECTORS
                         - (long)adf_sectors_for(playerlen) - reserve_tail;
            budget = sectors * ADF_SECTOR_SIZE;
            printf("presupuesto: %ld bytes = %ld sectores libres (reproductor "
                   "%lu bytes, %d reservados)\n", budget, sectors,
                   (unsigned long)playerlen, reserve_tail);
        } else {
            budget = A5V_DEFAULT_BUDGET;
            printf("presupuesto: %ld bytes (sin --adf, estimado)\n", budget);
        }
        if (au.nbytes)
            printf("             %lu para el audio, %ld para el video\n",
                   (unsigned long)au.nbytes, budget - (long)au.nbytes);
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
        long nswap = 0, nswapb = 0;     /* colores cambiados por franja */
        /* Con franjas el negro se reserva siempre: el color 0 es tambien el
         * del borde, y el Copper no lo cambia por franja (FORMAT.md). */
        int reserve_black = ((aspect != ASPECT_CROP) && (y1 - y0 < A5_H))
                          || g_nbands > 1;

        for (s = 0; s < nscenes; s++) {
            int n, b;
            double esum = 0, wsum = 0;

            scenes[s].pal = calloc((size_t)g_nbands, sizeof *scenes[s].pal);
            if (!scenes[s].pal) die("sin memoria");
            scenes[s].unique = 0;
            for (n = 0; n < scenes[s].count; n++)
                memset(idx + ((size_t)scenes[s].start + n) * A5_W * A5_H, 0,
                       (size_t)A5_W * A5_H);

            /* Una paleta por franja, con el histograma de sus filas en toda
             * la escena. band_overlap suma al histograma (no al mapeo) filas
             * de las franjas vecinas, para que dos paletas vecinas se
             * parezcan cerca del borde entre ellas. Con una sola franja es
             * exactamente la paleta por escena de antes, semilla incluida. */
            for (b = 0; b < g_nbands; b++) {
                A5Palette *bp = &scenes[s].pal[b];
                int by0 = g_nbands > 1 ? y0 + b * band_rows : y0;
                int by1 = g_nbands > 1 && by0 + band_rows < y1
                        ? by0 + band_rows : y1;
                int hy0 = by0 - band_overlap < y0 ? y0 : by0 - band_overlap;
                int hy1 = by1 + band_overlap > y1 ? y1 : by1 + band_overlap;
                A5Hist *h = a5_hist_new();

                for (n = 0; n < scenes[s].count; n++)
                    a5_hist_add_frame(h, frames[scenes[s].start + n], A5_W,
                                      hy0, hy1);
                if (b == 0 || g_band_colors >= ncolors - 1) {
                    a5_quantize(h, ncolors, reserve_black,
                                seed + (uint32_t)s + (uint32_t)b * 100003u,
                                bp);
                    if (b > 0) snap_to(bp, &scenes[s].pal[b - 1], band_snap);
                } else {
                    /* El Copper no alcanza a cambiar toda la paleta al
                     * empezar la franja: se hereda la de arriba y se cambian
                     * las g_band_colors entradas que mas bajan el error,
                     * eligiendolas de la paleta ideal de esta franja. El
                     * snap va sobre las candidatas: una candidata casi igual
                     * a un color heredado se vuelve ese mismo, y entonces
                     * cambiarla no gana nada y la ranura queda para otra. */
                    A5Palette cand;
                    a5_quantize(h, ncolors, reserve_black,
                                seed + (uint32_t)s + (uint32_t)b * 100003u,
                                &cand);
                    snap_to(&cand, &scenes[s].pal[b - 1], band_snap);
                    *bp = scenes[s].pal[b - 1];
                    nswap += a5_palette_swap(h, bp, &cand, g_band_colors);
                    nswapb++;
                }

                a5_hist_assign(h, bp);
                scenes[s].unique += a5_hist_unique(h);
                esum += a5_hist_mean_error(h, bp) * (by1 - by0);
                wsum += by1 - by0;

                /* Se cuantiza aca, con el histograma todavia vivo: sin dither
                 * alcanza con la cache de indices que ya tiene cada color.
                 *
                 * Histeresis temporal: si el indice que tenia el pixel en el
                 * frame anterior sigue siendo casi tan bueno como el optimo,
                 * se lo deja. La fuente es un h264 de 2 Mbit/s y tiene ruido
                 * de compresion; sin esto, en una zona quieta los pixeles
                 * saltan entre dos colores vecinos de la paleta y cada salto
                 * se paga en bytes de delta sin que se vea nada. */
                for (n = 0; n < scenes[s].count; n++) {
                    size_t f = (size_t)scenes[s].start + n;
                    uint8_t *dst = idx + f * A5_W * A5_H;
                    const uint8_t *pv = n ? dst - (size_t)A5_W * A5_H : NULL;
                    int y;
                    for (y = by0; y < by1; y++) {
                        const uint8_t *p = frames[f] + (size_t)y * A5_W * 3;
                        int x;
                        for (x = 0; x < A5_W; x++, p += 3) {
                            int best = dither == A5_DITHER_NONE
                                ? a5_hist_lookup(h, p[0], p[1], p[2])
                                : a5_map_pixel_dither(bp, p[0], p[1], p[2],
                                                      x, y, dither, dstrength);
                            if (pv && stability > 0) {
                                int old = pv[y * A5_W + x];
                                if (old != best) {
                                    Oklab c = a5_srgb_to_oklab(p[0], p[1],
                                                               p[2]);
                                    double dold = sqrt((double)a5_oklab_dist2(
                                        c, bp->lab[old]));
                                    double dnew = sqrt((double)a5_oklab_dist2(
                                        c, bp->lab[best]));
                                    if (dold <= dnew + stability) best = old;
                                }
                            }
                            dst[y * A5_W + x] = (uint8_t)best;
                        }
                    }
                }
                a5_hist_free(h);
            }
            scenes[s].error = wsum > 0 ? esum / wsum : 0;
        }
        if (nswapb)
            printf("             %.1f colores cambiados por franja de los "
                   "%d que entran\n", (double)nswap / nswapb,
                   g_band_colors);
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
            pal = scenes[s].pal;

            for (y = 0; y < A5_H; y++) {
                int x;
                for (x = 0; x < A5_W; x++) {
                    uint8_t r, g, b;
                    a5_rgb444_to_srgb(rowpal(pal, y)->rgb444[f[y * A5_W + x]],
                                      &r, &g, &b);
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
            for (c = 0; c < scenes[s].pal[0].n; c++)
                printf("%03X ", scenes[s].pal[0].rgb444[c]);
            if (g_nbands > 1) printf("(franja 0 de %d)", g_nbands);
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
     * presupuesto, se sube la perdida y se vuelve a codificar; si entra y
     * sobra disco, se baja.
     *
     * La busqueda es binaria sobre el umbral, pero el umbral NO es monotono
     * en bytes: una repeticion de mas o de menos cambia el buffer oculto de
     * todo lo que sigue. Por eso no se devuelve "el umbral mas bajo que
     * entro" sino **el mejor stream que entro**, comparando el error que
     * cada uno midio. Asumir la monotonia dejaba hasta un 12 %% del disco
     * sin usar con mas perdida de la necesaria (medido con melissa.mp4 a 32
     * colores, 2026-09-12). */
    {
        A5Stream st;
        double lo = quality * 0.0015, hi = lo, qthr = lo;
        size_t total;
        int pass = 0;
        RateCtl rc;

        memset(&rc, 0, sizeof rc);
        rc.idx = idx; rc.nframes = nframes; rc.scenes = scenes;
        rc.nscenes = nscenes; rc.planes = planes; rc.ncolors = ncolors;
        rc.y0 = y0; rc.y1 = y1; rc.repeat_boost = repeat_boost;
        rc.cyc_limit = cyc_limit; rc.min_hold = min_hold;
        rc.max_late = max_late; rc.au = &au; rc.budget = budget;

        total = rc_try(&rc, lo);
        printf("\nbitstream  : calidad %.4f -> %lu bytes\n", lo,
               (unsigned long)total);

        if (budget > 0 && total > (size_t)budget) {
            hi = lo > 0 ? lo : 0.01;
            do {
                hi *= 2;
                total = rc_try(&rc, hi);
                printf("             calidad %.4f -> %lu bytes\n", hi,
                       (unsigned long)total);
            } while (total > (size_t)budget && hi < 1.0 && ++pass < 12);

            for (pass = 0; pass < 10 && hi - lo > 0.0005; pass++) {
                double mid = (lo + hi) / 2;
                if (rc_try(&rc, mid) <= (size_t)budget) hi = mid;
                else                                    lo = mid;
            }
            printf("             ajustado a %.4f -> %lu bytes\n",
                   rc.bestthr, (unsigned long)rc.besttotal);
        } else if (budget > 0 && lo > 0) {
            /* Entra y sobra disco: se baja la perdida para usarlo. */
            double a = 0, b = lo;

            if (rc_try(&rc, 0.0) > (size_t)budget) {
                for (pass = 0; pass < 8 && b - a > 0.0005; pass++) {
                    double mid = (a + b) / 2;
                    if (rc_try(&rc, mid) <= (size_t)budget) b = mid;
                    else                                    a = mid;
                }
            }
            if (rc.bestthr < lo)
                printf("             sobraba disco: bajado a %.4f -> %lu "
                       "bytes\n", rc.bestthr,
                       (unsigned long)rc.besttotal);
        }
        st = rc.best;
        qthr = rc.bestthr;
        total = rc.besttotal;
        if (!rc.fits && budget > 0)
            printf("             AVISO: ningun umbral entro en el "
                   "presupuesto\n");

        /* Calidad contra la fuente del stream elegido: se lo arma una vez
         * mas (sale identico) midiendo lo que se ve contra el original. */
        {
            A5Stream t3;
            build_stream(&t3, idx, nframes, scenes, nscenes, planes, ncolors,
                         y0, y1, qthr, repeat_boost, cyc_limit, min_hold,
                         max_late, &au, (const uint8_t *const *)frames);
            a5buf_free(&st.buf); free(st.crc);
            st = t3;
        }

        /* --- escribir --------------------------------------------- */
        {
            FILE *f = fopen(out, "wb");
            A5Buf hdr;
            char crcpath[1024];

            if (!f) die("no pude abrir el archivo de salida");
            a5buf_init(&hdr);
            a5v_put_header(&hdr, planes, ncolors, au.format, au.period,
                           y0, y1, g_band_rows, g_band_colors,
                           (uint32_t)nframes,
                           (uint32_t)st.buf.len);
            fwrite(hdr.p, 1, hdr.len, f);
            fwrite(st.buf.p, 1, st.buf.len, f);
            fclose(f);

            /* Un CRC por frame y, si hay audio, uno mas de todo lo que suena
             * (docs/FORMAT.md, Verificacion). */
            snprintf(crcpath, sizeof crcpath, "%s.crc", out);
            f = fopen(crcpath, "wb");
            if (f) {
                size_t n;
                for (n = 0; n <= nframes; n++) {
                    uint32_t c;
                    uint8_t b[4];
                    if (n == nframes) {
                        if (au.format == A5V_AUDIO_NONE) break;
                        c = a5_crc32(au.recon, au.nsamples, 0);
                    } else {
                        c = st.crc[n];
                    }
                    b[0] = (uint8_t)(c >> 24);
                    b[1] = (uint8_t)(c >> 16);
                    b[2] = (uint8_t)(c >> 8);
                    b[3] = (uint8_t)c;
                    fwrite(b, 1, 4, f);
                }
                fclose(f);
            }

            printf("\nsalida     : %s (%lu bytes)\n", out,
                   (unsigned long)total);
            printf("             %s (checksums para el decoder)\n", crcpath);

            /* El disco entero: los datos son la cabecera y los paquetes, tal
             * cual el .a5v. */
            if (adf_path) {
                uint8_t *disk = calloc(ADF_SIZE, 1);
                A5Buf data;
                AdfLayout lay;
                const char *err;

                if (!disk) die("sin memoria");
                a5buf_init(&data);
                a5buf_write(&data, hdr.p, hdr.len);
                a5buf_write(&data, st.buf.p, st.buf.len);
                err = adf_assemble(disk, boot, bootlen, player, playerlen,
                                   data.p, data.len, reserve_tail, &lay);
                if (err) die(err);
                f = fopen(adf_path, "wb");
                if (!f || fwrite(disk, 1, ADF_SIZE, f) != ADF_SIZE)
                    die("no pude escribir el ADF");
                fclose(f);
                printf("disco      : %s\n", adf_path);
                printf("             reproductor en %u sectores, datos desde "
                       "el %u, %u de %u sectores usados (%u libres)\n",
                       lay.player_sectors, lay.data_sector, lay.used,
                       lay.limit, lay.limit - lay.used);
                a5buf_free(&data);
                free(disk);
            }
            a5buf_free(&hdr);
        }

        /* --- estadisticas del bitstream ---------------------------- */
        printf("\npaquetes   : %d (%d delta, %d repeticion = %.1f%%, "
               "%d cambios de paleta)\n",
               st.npackets, st.ndelta, st.nrepeat,
               100.0 * st.nrepeat / st.npackets, st.npalette);
        printf("bytes/frame: %.1f de media, %lu el peor (frame %d)\n",
               (double)st.buf.len / nframes, (unsigned long)st.maxpacket,
               st.maxpacket_frame);
        printf("tasa       : %.1f KB/s de video + %.1f KB/s de audio\n",
               (st.buf.len - au.nbytes) / 1024.0 / (nframes / A5_VIDEO_FPS),
               au.nbytes / 1024.0 / (nframes / A5_VIDEO_FPS));
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
        printf("prediccion : %d de %d deltas copian el buffer visible antes "
               "(%.1f%%), %.1f ms cada copia\n",
               st.ncopy, st.ndelta,
               st.ndelta ? 100.0 * st.ncopy / st.ndelta : 0.0,
               a5_cyc_blit(planes) * (double)planes * (y1 - y0)
                   * 1000.0 / A5_CPU_HZ);
        printf("error final: %.4f (contra el frame cuantizado ideal)\n",
               st.err_sum / nframes);
        printf("salpicado  : %.2f%% de los pixeles activos con error "
               "visible (> %.2f)\n",
               100.0 * st.bad_pixels / ((double)nframes * (y1 - y0) * A5_W),
               A5_VISIBLE_ERR);
        printf("vs. fuente : error %.4f; %.2f%% de los pixeles activos a mas "
               "de %.2f del original\n",
               st.src_err / ((double)nframes * (y1 - y0) * A5_W),
               100.0 * st.src_bad / ((double)nframes * (y1 - y0) * A5_W),
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
    for (i = 0; i < nscenes; i++) free(scenes[i].pal);
    free(prev); free(cur); free(delta); free(scenes);
    free(au.bytes); free(au.recon); free(boot); free(player);
    return 0;
}
