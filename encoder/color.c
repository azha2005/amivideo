/* color.c - sRGB <-> Oklab y redondeo a RGB444 (los 12 bits del OCS). */
#include <math.h>
#include "a500vp.h"

static float s_lin[256];       /* sRGB de 8 bits -> lineal 0..1 */
static int   s_ready = 0;

static void init_tables(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        double c = i / 255.0;
        s_lin[i] = (float)(c <= 0.04045 ? c / 12.92
                                        : pow((c + 0.055) / 1.055, 2.4));
    }
    s_ready = 1;
}

static double lin_to_srgb(double c)
{
    if (c <= 0.0) return 0.0;
    if (c >= 1.0) return 1.0;
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

Oklab a5_srgb_to_oklab(uint8_t r, uint8_t g, uint8_t b)
{
    float lr, lg, lb, l, m, s, l_, m_, s_;
    Oklab out;

    if (!s_ready) init_tables();
    lr = s_lin[r]; lg = s_lin[g]; lb = s_lin[b];

    l = 0.4122214708f * lr + 0.5363325363f * lg + 0.0514459929f * lb;
    m = 0.2119034982f * lr + 0.6806995451f * lg + 0.1073969566f * lb;
    s = 0.0883024619f * lr + 0.2817188376f * lg + 0.6299787005f * lb;

    l_ = cbrtf(l); m_ = cbrtf(m); s_ = cbrtf(s);

    out.L = 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_;
    out.a = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_;
    out.b = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_;
    return out;
}

void a5_oklab_to_srgb(Oklab c, uint8_t *r, uint8_t *g, uint8_t *bb)
{
    float l_ = c.L + 0.3963377774f * c.a + 0.2158037573f * c.b;
    float m_ = c.L - 0.1055613458f * c.a - 0.0638541728f * c.b;
    float s_ = c.L - 0.0894841775f * c.a - 1.2914855480f * c.b;
    float l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;

    double lr =  4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    double lg = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    double lb = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;

    *r  = (uint8_t)(lin_to_srgb(lr) * 255.0 + 0.5);
    *g  = (uint8_t)(lin_to_srgb(lg) * 255.0 + 0.5);
    *bb = (uint8_t)(lin_to_srgb(lb) * 255.0 + 0.5);
}

float a5_oklab_dist2(Oklab p, Oklab q)
{
    float dl = p.L - q.L, da = p.a - q.a, db = p.b - q.b;
    return dl * dl + da * da + db * db;
}

/* El OCS tiene 4 bits por componente. Un nibble n se ve como n*17 en 8 bits
 * (0, 17, 34 ... 255), que es lo que hace el DAC al repetir el nibble. */
static int to_nibble(int v8)
{
    int n = (v8 * 15 + 127) / 255;
    return n < 0 ? 0 : (n > 15 ? 15 : n);
}

A5Color a5_srgb_to_rgb444(uint8_t r, uint8_t g, uint8_t b)
{
    return (A5Color)((to_nibble(r) << 8) | (to_nibble(g) << 4) | to_nibble(b));
}

void a5_rgb444_to_srgb(A5Color c, uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = (uint8_t)(((c >> 8) & 15) * 17);
    *g = (uint8_t)(((c >> 4) & 15) * 17);
    *b = (uint8_t)((c & 15) * 17);
}
