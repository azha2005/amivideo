/* quant.c - histograma de colores de una escena, k-means en Oklab y mapeo a
 * paleta RGB444. Ver docs/DECISIONS.md.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "a500vp.h"

#define EMPTY 0xFFFFFFFFu

typedef struct {
    uint32_t key;        /* 0x00RRGGBB, o EMPTY */
    uint32_t count;
    Oklab    lab;
    int      idx;        /* indice de paleta asignado */
} Slot;

struct A5Hist {
    Slot   *slots;
    size_t  cap;         /* potencia de dos */
    size_t  used;
    uint64_t total;      /* suma de count */
};

static uint32_t mix(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static void hist_alloc(A5Hist *h, size_t cap)
{
    size_t i;
    h->slots = malloc(cap * sizeof *h->slots);
    if (!h->slots) { fprintf(stderr, "sin memoria\n"); exit(1); }
    for (i = 0; i < cap; i++) h->slots[i].key = EMPTY;
    h->cap = cap;
}

A5Hist *a5_hist_new(void)
{
    A5Hist *h = calloc(1, sizeof *h);
    if (!h) { fprintf(stderr, "sin memoria\n"); exit(1); }
    hist_alloc(h, 1u << 16);
    return h;
}

void a5_hist_free(A5Hist *h)
{
    if (!h) return;
    free(h->slots);
    free(h);
}

static void hist_grow(A5Hist *h)
{
    Slot  *old = h->slots;
    size_t oldcap = h->cap, i;

    hist_alloc(h, oldcap * 2);
    for (i = 0; i < oldcap; i++) {
        if (old[i].key != EMPTY) {
            size_t j = mix(old[i].key) & (h->cap - 1);
            while (h->slots[j].key != EMPTY) j = (j + 1) & (h->cap - 1);
            h->slots[j] = old[i];
        }
    }
    free(old);
}

static Slot *hist_slot(A5Hist *h, uint32_t key)
{
    size_t i = mix(key) & (h->cap - 1);

    while (h->slots[i].key != EMPTY && h->slots[i].key != key)
        i = (i + 1) & (h->cap - 1);

    if (h->slots[i].key == EMPTY) {
        h->slots[i].key = key;
        h->slots[i].count = 0;
        h->slots[i].idx = 0;
        h->slots[i].lab = a5_srgb_to_oklab((uint8_t)(key >> 16),
                                           (uint8_t)(key >> 8),
                                           (uint8_t)key);
        h->used++;
    }
    return &h->slots[i];
}

void a5_hist_add_frame(A5Hist *h, const uint8_t *rgb, int w, int y0, int y1)
{
    int x, y;

    for (y = y0; y < y1; y++) {
        const uint8_t *p = rgb + (size_t)y * w * 3;
        for (x = 0; x < w; x++, p += 3) {
            uint32_t key = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
            if ((h->used + 1) * 10 >= h->cap * 6) hist_grow(h);
            hist_slot(h, key)->count++;
            h->total++;
        }
    }
}

size_t a5_hist_unique(const A5Hist *h) { return h->used; }

/* ------------------------------------------------------------------ */

static uint32_t xs32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

static double frand(uint32_t *s) { return (xs32(s) >> 8) / 16777216.0; }

/* k-means++ ponderado por cuenta, en Oklab. */
static void kmeanspp(const Oklab *pts, const uint32_t *wt, size_t n,
                     int k, uint32_t *seed, Oklab *cent)
{
    double *d2 = malloc(n * sizeof *d2);
    size_t i;
    int c;

    if (!d2) { fprintf(stderr, "sin memoria\n"); exit(1); }

    /* primer centroide: uno al azar, ponderado */
    {
        double total = 0, r;
        for (i = 0; i < n; i++) total += wt[i];
        r = frand(seed) * total;
        for (i = 0; i < n && r >= wt[i]; i++) r -= wt[i];
        cent[0] = pts[i < n ? i : n - 1];
    }
    for (i = 0; i < n; i++) d2[i] = a5_oklab_dist2(pts[i], cent[0]);

    for (c = 1; c < k; c++) {
        double total = 0, r;
        for (i = 0; i < n; i++) total += d2[i] * wt[i];
        if (total <= 0) { cent[c] = cent[0]; continue; }
        r = frand(seed) * total;
        for (i = 0; i < n; i++) {
            double w = d2[i] * wt[i];
            if (r < w) break;
            r -= w;
        }
        cent[c] = pts[i < n ? i : n - 1];
        for (i = 0; i < n; i++) {
            double d = a5_oklab_dist2(pts[i], cent[c]);
            if (d < d2[i]) d2[i] = d;
        }
    }
    free(d2);
}

static int nearest(const Oklab *cent, int k, Oklab p)
{
    int best = 0, c;
    float bd = a5_oklab_dist2(p, cent[0]);

    for (c = 1; c < k; c++) {
        float d = a5_oklab_dist2(p, cent[c]);
        if (d < bd) { bd = d; best = c; }
    }
    return best;
}

void a5_quantize(A5Hist *h, int ncolors, int reserve_black, uint32_t seed,
                 A5Palette *out)
{
    Oklab   *pts, cent[A5_MAX_COLORS];
    uint32_t *wt;
    int     *own;
    size_t   n = 0, i;
    int      k, c, iter, base;
    uint32_t rng = seed ? seed : 1u;

    memset(out, 0, sizeof *out);
    out->n = ncolors;

    base = reserve_black ? 1 : 0;
    k = ncolors - base;
    if (k < 1) k = 1;

    pts = malloc(h->used * sizeof *pts);
    wt  = malloc(h->used * sizeof *wt);
    own = malloc(h->used * sizeof *own);
    if (!pts || !wt || !own) { fprintf(stderr, "sin memoria\n"); exit(1); }

    for (i = 0; i < h->cap; i++) {
        if (h->slots[i].key != EMPTY) {
            pts[n] = h->slots[i].lab;
            wt[n]  = h->slots[i].count;
            own[n] = -1;
            n++;
        }
    }

    if (n == 0) { out->n = ncolors; return; }
    if ((size_t)k > n) k = (int)n;

    kmeanspp(pts, wt, n, k, &rng, cent);

    for (iter = 0; iter < 64; iter++) {
        double sl[A5_MAX_COLORS], sa[A5_MAX_COLORS], sb[A5_MAX_COLORS];
        double sw[A5_MAX_COLORS];
        int changed = 0;

        for (c = 0; c < k; c++) sl[c] = sa[c] = sb[c] = sw[c] = 0;

        for (i = 0; i < n; i++) {
            int b = nearest(cent, k, pts[i]);
            if (b != own[i]) { own[i] = b; changed = 1; }
            sl[b] += (double)pts[i].L * wt[i];
            sa[b] += (double)pts[i].a * wt[i];
            sb[b] += (double)pts[i].b * wt[i];
            sw[b] += wt[i];
        }
        for (c = 0; c < k; c++) {
            if (sw[c] > 0) {
                cent[c].L = (float)(sl[c] / sw[c]);
                cent[c].a = (float)(sa[c] / sw[c]);
                cent[c].b = (float)(sb[c] / sw[c]);
            } else {
                /* Cluster vacio: se lo manda al punto peor representado. */
                double worst = -1;
                size_t wi = 0;
                for (i = 0; i < n; i++) {
                    double d = a5_oklab_dist2(pts[i], cent[own[i]]) * wt[i];
                    if (d > worst) { worst = d; wi = i; }
                }
                cent[c] = pts[wi];
                changed = 1;
            }
        }
        if (!changed) break;
    }

    /* Los centroides se redondean a RGB444 y la paleta pasa a ser la de
     * verdad: lo que se compara despues es contra lo que se va a ver. */
    if (reserve_black) {
        out->rgb444[0] = 0x000;
        out->lab[0] = a5_srgb_to_oklab(0, 0, 0);
    }
    for (c = 0; c < k; c++) {
        uint8_t r, g, b;
        a5_oklab_to_srgb(cent[c], &r, &g, &b);
        out->rgb444[base + c] = a5_srgb_to_rgb444(r, g, b);
        a5_rgb444_to_srgb(out->rgb444[base + c], &r, &g, &b);
        out->lab[base + c] = a5_srgb_to_oklab(r, g, b);
    }
    for (c = base + k; c < ncolors; c++) {          /* escena casi plana */
        out->rgb444[c] = out->rgb444[base];
        out->lab[c] = out->lab[base];
    }
    out->n = base + k;

    /* Dos centroides distintos pueden caer en el mismo RGB444 al redondear
     * (pasa sobre todo con el negro reservado). Ese hueco es un color
     * tirado a la basura: se lo da al color de la escena peor representado.
     * Solo entre los que el k-means encontro de verdad. */
    if (n > (size_t)k) {
        for (c = 1; c < out->n; c++) {
            int d, dup = 0;
            for (d = 0; d < c; d++)
                if (out->rgb444[d] == out->rgb444[c]) { dup = 1; break; }
            if (!dup) continue;

            {
                double worst = -1;
                size_t wi = (size_t)-1;
                for (i = 0; i < n; i++) {
                    int j, seen = 0;
                    A5Color cand;
                    uint8_t r, g, b;
                    a5_oklab_to_srgb(pts[i], &r, &g, &b);
                    cand = a5_srgb_to_rgb444(r, g, b);
                    for (j = 0; j < out->n; j++)
                        if (out->rgb444[j] == cand) { seen = 1; break; }
                    if (seen) continue;
                    {
                        double dd = a5_oklab_dist2(pts[i],
                                        out->lab[nearest(out->lab, out->n,
                                                         pts[i])]) * wt[i];
                        if (dd > worst) { worst = dd; wi = i; }
                    }
                }
                if (wi != (size_t)-1) {
                    uint8_t r, g, b;
                    a5_oklab_to_srgb(pts[wi], &r, &g, &b);
                    out->rgb444[c] = a5_srgb_to_rgb444(r, g, b);
                    a5_rgb444_to_srgb(out->rgb444[c], &r, &g, &b);
                    out->lab[c] = a5_srgb_to_oklab(r, g, b);
                }
            }
        }
    }
    out->n = ncolors;
    for (c = base + k; c < ncolors; c++) {
        out->rgb444[c] = out->rgb444[base];
        out->lab[c] = out->lab[base];
    }

    free(pts); free(wt); free(own);
}

void a5_hist_assign(A5Hist *h, const A5Palette *pal)
{
    size_t i;
    for (i = 0; i < h->cap; i++)
        if (h->slots[i].key != EMPTY)
            h->slots[i].idx = nearest(pal->lab, pal->n, h->slots[i].lab);
}

int a5_hist_lookup(const A5Hist *h, uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t key = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    size_t i = mix(key) & (h->cap - 1);

    while (h->slots[i].key != EMPTY) {
        if (h->slots[i].key == key) return h->slots[i].idx;
        i = (i + 1) & (h->cap - 1);
    }
    return 0;
}

double a5_hist_mean_error(const A5Hist *h, const A5Palette *pal)
{
    double sum = 0;
    size_t i;

    if (h->total == 0) return 0;
    for (i = 0; i < h->cap; i++) {
        if (h->slots[i].key != EMPTY) {
            float d2 = a5_oklab_dist2(h->slots[i].lab, pal->lab[h->slots[i].idx]);
            sum += sqrt((double)d2) * h->slots[i].count;
        }
    }
    return sum / (double)h->total;
}

/* ------------------------------------------------------------------ */

static const int bayer2[2][2] = { { 0, 2 }, { 3, 1 } };
static const int bayer4[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 }
};

int a5_map_pixel_dither(const A5Palette *pal, uint8_t r, uint8_t g, uint8_t b,
                        int x, int y, A5Dither d, float strength)
{
    Oklab p = a5_srgb_to_oklab(r, g, b);
    float t;

    switch (d) {
    case A5_DITHER_BAYER2: t = (bayer2[y & 1][x & 1] + 0.5f) / 4.0f;  break;
    case A5_DITHER_BAYER4: t = (bayer4[y & 3][x & 3] + 0.5f) / 16.0f; break;
    default:               return nearest(pal->lab, pal->n, p);
    }
    p.L += (t - 0.5f) * strength;
    return nearest(pal->lab, pal->n, p);
}
