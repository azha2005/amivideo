/* stream.c - ver stream.h y docs/FORMAT.md */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "stream.h"

/* --- buffer que crece --------------------------------------------------- */

void a5buf_init(A5Buf *b) { b->p = NULL; b->len = b->cap = 0; }
void a5buf_free(A5Buf *b) { free(b->p); a5buf_init(b); }

static void need(A5Buf *b, size_t n)
{
    if (b->len + n > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        while (c < b->len + n) c *= 2;
        b->p = realloc(b->p, c);
        if (!b->p) { fprintf(stderr, "sin memoria\n"); exit(1); }
        b->cap = c;
    }
}

void a5buf_put8(A5Buf *b, unsigned v)
{
    need(b, 1);
    b->p[b->len++] = (uint8_t)v;
}

void a5buf_put16(A5Buf *b, unsigned v)
{
    need(b, 2);
    b->p[b->len++] = (uint8_t)(v >> 8);
    b->p[b->len++] = (uint8_t)v;
}

void a5buf_put32(A5Buf *b, uint32_t v)
{
    need(b, 4);
    b->p[b->len++] = (uint8_t)(v >> 24);
    b->p[b->len++] = (uint8_t)(v >> 16);
    b->p[b->len++] = (uint8_t)(v >> 8);
    b->p[b->len++] = (uint8_t)v;
}

void a5buf_write(A5Buf *b, const void *src, size_t n)
{
    need(b, n);
    memcpy(b->p + b->len, src, n);
    b->len += n;
}

/* --- indices <-> planos -------------------------------------------------
 * Bit 7 del primer byte = pixel logico de mas a la izquierda, que es como
 * lo lee el Denise y como lo va a escribir el reproductor.
 */

void a5_planarize_row(const uint8_t *idx_row, int planes, uint8_t *planar)
{
    int p, b, i;

    for (p = 0; p < planes; p++) {
        uint8_t *dst = planar + p * A5_ROWBYTES;
        for (b = 0; b < A5_ROWBYTES; b++) {
            unsigned v = 0;
            const uint8_t *s = idx_row + b * 8;
            for (i = 0; i < 8; i++)
                v = (v << 1) | ((s[i] >> p) & 1);
            dst[b] = (uint8_t)v;
        }
    }
}

void a5_depack_row(const uint8_t *planar, int planes, uint8_t *idx_row)
{
    int p, b, i;

    memset(idx_row, 0, A5_W);
    for (p = 0; p < planes; p++) {
        const uint8_t *s = planar + p * A5_ROWBYTES;
        for (b = 0; b < A5_ROWBYTES; b++) {
            unsigned v = s[b];
            for (i = 0; i < 8; i++)
                idx_row[b * 8 + i] |= (uint8_t)(((v >> (7 - i)) & 1) << p);
        }
    }
}

/* --- delta --------------------------------------------------------------- */

long a5_cyc_byte(int planes)
{
    return planes >= 5 ? A5_CYC_BYTE_5PL : A5_CYC_BYTE;
}

long a5_cyc_blit(int planes)
{
    return planes >= 5 ? A5_CYC_BLIT_5PL : A5_CYC_BLIT;
}

long a5_delta_cost(const A5DeltaStats *st, int planes)
{
    return A5_CYC_FRAME
         + (long)st->rows  * A5_CYC_ROW
         + (long)st->cols  * A5_CYC_COL
         + (long)st->bytes * a5_cyc_byte(planes);
}

int a5_delta_encode(A5Buf *out, const uint8_t *hidden, const uint8_t *target,
                    int planes, A5DeltaStats *st)
{
    uint8_t rowmask[A5_ROWMASK_SIZE];
    uint8_t ph[A5_MAX_PLANES * A5_ROWBYTES];
    uint8_t pt[A5_MAX_PLANES * A5_ROWBYTES];
    uint8_t colmask[A5_COLMASK_SIZE];
    size_t  mark;
    int y, p, b;

    memset(rowmask, 0, sizeof rowmask);
    memset(st, 0, sizeof *st);

    /* El mapa de filas se escribe primero y se corrige al final: hasta no
     * recorrer todas las filas no se sabe cuales cambiaron. */
    mark = out->len;
    a5buf_write(out, rowmask, sizeof rowmask);

    for (y = 0; y < A5_H; y++) {
        const uint8_t *hr = hidden + (size_t)y * A5_W;
        const uint8_t *tr = target + (size_t)y * A5_W;
        int any = 0;

        if (!memcmp(hr, tr, A5_W)) continue;

        a5_planarize_row(hr, planes, ph);
        a5_planarize_row(tr, planes, pt);

        memset(colmask, 0, sizeof colmask);
        for (b = 0; b < A5_ROWBYTES; b++) {
            for (p = 0; p < planes; p++)
                if (ph[p * A5_ROWBYTES + b] != pt[p * A5_ROWBYTES + b]) break;
            if (p < planes) {
                colmask[b >> 3] |= (uint8_t)(0x80 >> (b & 7));
                any++;
            }
        }
        if (!any) continue;            /* no puede pasar, pero por las dudas */

        rowmask[y >> 3] |= (uint8_t)(0x80 >> (y & 7));
        st->rows++;
        st->cols += any;

        a5buf_write(out, colmask, sizeof colmask);
        for (b = 0; b < A5_ROWBYTES; b++) {
            if (!(colmask[b >> 3] & (0x80 >> (b & 7)))) continue;
            for (p = 0; p < planes; p++)
                a5buf_put8(out, pt[p * A5_ROWBYTES + b]);
            st->bytes += planes;
        }
    }

    memcpy(out->p + mark, rowmask, sizeof rowmask);
    st->cycles = a5_delta_cost(st, planes);
    return st->rows > 0;
}

const uint8_t *a5_delta_apply(uint8_t *fb, const uint8_t *data,
                              const uint8_t *end, int planes,
                              A5DeltaStats *st)
{
    const uint8_t *rowmask = data;
    int y, p, b;

    memset(st, 0, sizeof *st);
    if (end - data < A5_ROWMASK_SIZE) return NULL;
    data += A5_ROWMASK_SIZE;

    for (y = 0; y < A5_H; y++) {
        const uint8_t *colmask;

        if (!(rowmask[y >> 3] & (0x80 >> (y & 7)))) continue;
        if (end - data < A5_COLMASK_SIZE) return NULL;
        colmask = data;
        data += A5_COLMASK_SIZE;
        st->rows++;

        /* Los 4 bits bajos del tercer byte no corresponden a ninguna
         * columna: tienen que venir en cero. */
        if (colmask[2] & 0x0F) return NULL;

        for (b = 0; b < A5_ROWBYTES; b++) {
            if (!(colmask[b >> 3] & (0x80 >> (b & 7)))) continue;
            if (end - data < planes) return NULL;
            for (p = 0; p < planes; p++)
                fb[((size_t)p * A5_H + y) * A5_ROWBYTES + b] = *data++;
            st->cols++;
            st->bytes += planes;
        }
    }
    st->cycles = a5_delta_cost(st, planes);
    return data;
}

/* --- cabecera y doblado --------------------------------------------------- */

int a5v_nbands(int band_rows, int y0, int y1)
{
    if (band_rows <= 0 || y1 <= y0) return 1;
    return (y1 - y0 + band_rows - 1) / band_rows;
}

int a5v_band_of(int y, int band_rows, int y0, int nbands)
{
    int b;
    if (nbands <= 1 || y < y0) return 0;
    b = (y - y0) / band_rows;
    return b < nbands ? b : nbands - 1;
}

int a5v_palette_words(int ncolors, int nbands, int band_colors)
{
    return ncolors + (nbands - 1) * band_colors * 2;
}

void a5v_put_header(A5Buf *b, int planes, int ncolors, int audio_format,
                    int audio_period, int y0, int y1, int band_rows,
                    int band_colors, uint32_t npackets, uint32_t payload)
{
    int has_audio = audio_format != A5V_AUDIO_NONE;

    a5buf_write(b, A5V_MAGIC, 4);
    a5buf_put16(b, A5V_VERSION);
    a5buf_put16(b, has_audio ? 1u : 0u);       /* flags: bit 0 = hay audio */
    a5buf_put16(b, A5_W);
    a5buf_put16(b, A5_H);
    a5buf_put8(b, (unsigned)planes);
    a5buf_put8(b, (unsigned)ncolors);
    a5buf_put16(b, has_audio ? (unsigned)audio_period : 0u);
    a5buf_put16(b, (unsigned)y0);
    a5buf_put16(b, (unsigned)y1);
    a5buf_put32(b, npackets);
    a5buf_put32(b, payload);
    a5buf_put8(b, (unsigned)audio_format);     /* 28: formato del audio */
    a5buf_put8(b, (unsigned)band_rows);        /* 29: filas por franja */
    a5buf_put8(b, (unsigned)band_colors);      /* 30: colores por franja */
    a5buf_put8(b, 0);
}

/* --- audio ---------------------------------------------------------------- */

const int8_t a5_fib_table[16] = {
    -34, -21, -13, -8, -5, -3, -2, -1, 0, 1, 2, 3, 5, 8, 13, 21
};

void a5_fib4_encode(const float *in, size_t n, uint8_t *out, int8_t *recon,
                    int *accp)
{
    int acc = *accp;
    size_t i;

    for (i = 0; i < n; i++) {
        int best = 8, k;
        float bd = 1e30f;

        /* Codicioso: el nibble que deja al acumulador mas cerca de la
         * muestra, sin salirse de 8 bits (el 68000 suma con add.b y daria
         * la vuelta). */
        for (k = 0; k < 16; k++) {
            int v = acc + a5_fib_table[k];
            float d;
            if (v < -128 || v > 127) continue;
            d = in[i] - (float)v;
            if (d < 0) d = -d;
            if (d < bd) { bd = d; best = k; }
        }
        acc += a5_fib_table[best];
        if (recon) recon[i] = (int8_t)acc;
        if (i & 1) out[i / 2] |= (uint8_t)best;
        else       out[i / 2] = (uint8_t)(best << 4);
    }
    *accp = acc;
}

void a5_fib4_decode(const uint8_t *in, size_t nbytes, int *accp, int8_t *out)
{
    /* Suma en 8 bits con vuelta, como add.b. */
    uint8_t acc = (uint8_t)*accp;
    size_t i;

    for (i = 0; i < nbytes; i++) {
        acc = (uint8_t)(acc + (uint8_t)a5_fib_table[in[i] >> 4]);
        *out++ = (int8_t)acc;
        acc = (uint8_t)(acc + (uint8_t)a5_fib_table[in[i] & 15]);
        *out++ = (int8_t)acc;
    }
    *accp = (int8_t)acc;
}

uint32_t a5_audio_samples_through(uint32_t n, double paula_hz)
{
    double sps = paula_hz / A5_VIDEO_FPS;      /* muestras por hueco */
    return 2u * (uint32_t)floor((n + 1) * sps / 2.0);
}

long a5_audio_fill_cost(int format)
{
    switch (format) {
    case A5V_AUDIO_FIB4: return A5_CYC_AUDIO_FILL;
    case A5V_AUDIO_PCM8: return A5_CYC_AUDIO_FILL_PCM8;
    default:             return 0;
    }
}

size_t a5_audio_bytes(int format, size_t samples)
{
    switch (format) {
    case A5V_AUDIO_FIB4: return samples / 2;
    case A5V_AUDIO_PCM8: return samples;
    default:             return 0;
    }
}

uint16_t a5_double_byte(uint8_t v)
{
    unsigned w = 0;
    int i;

    for (i = 7; i >= 0; i--)
        w = (w << 2) | (((v >> i) & 1) ? 3u : 0u);
    return (uint16_t)w;
}

/* --- crc32 (el de zlib, sin tabla precalculada) -------------------------- */

uint32_t a5_crc32(const void *data, size_t n, uint32_t crc)
{
    const uint8_t *p = data;
    size_t i;
    int k;

    crc = ~crc;
    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}
