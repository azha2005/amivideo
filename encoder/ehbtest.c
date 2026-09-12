/* ehbtest.c - simulacion honesta de "mas colores" (H16/H17).
 *
 * Responde una sola pregunta: en la geometria real del video (160x96
 * logicos, que se ven doblados a 320x192) y con el hardware real (colores
 * RGB444), cuanto mejora la imagen al pasar de 16 colores a 32, a 64, y a
 * los 64 de EHB, que no son libres: son 32 colores mas sus mitades exactas.
 *
 * Usa el mismo espacio perceptual (Oklab) y el mismo redondeo a RGB444 que
 * el encoder. El k-means es propio porque A5Palette solo llega a 16.
 *
 * No es parte del build: se compila a mano cuando hace falta.
 *   gcc -std=c11 -O2 color.c vio.c ehbtest.c -o ehbtest -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "a500vp.h"

#define W   160
#define H   96
#define NPX (W * H)
#define MAXC 64

typedef struct {
    A5Color rgb444[MAXC];
    Oklab   lab[MAXC];
    int     n;
} Pal;

static uint8_t  px[NPX * 3];
static Oklab    lab[NPX];
static uint32_t rng = 12345;

static uint32_t xr(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

/* Redondea un centroide sRGB a RGB444 y deja su Oklab al lado. */
static void set_color(Pal *p, int i, double r, double g, double b)
{
    uint8_t rr, gg, bb;
    double cl[3] = { r, g, b };
    int k;

    for (k = 0; k < 3; k++) {
        if (cl[k] < 0)   cl[k] = 0;
        if (cl[k] > 255) cl[k] = 255;
    }
    p->rgb444[i] = a5_srgb_to_rgb444((uint8_t)(cl[0] + 0.5),
                                     (uint8_t)(cl[1] + 0.5),
                                     (uint8_t)(cl[2] + 0.5));
    a5_rgb444_to_srgb(p->rgb444[i], &rr, &gg, &bb);
    p->lab[i] = a5_srgb_to_oklab(rr, gg, bb);
}

static int nearest(const Pal *p, Oklab t)
{
    int i, best = 0;
    float bd = 1e30f;

    for (i = 0; i < p->n; i++) {
        float d = a5_oklab_dist2(t, p->lab[i]);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* k-means en Oklab sobre los pixeles, con los centroides redondeados a
 * RGB444 en cada vuelta: asi el redondeo del hardware entra en el lazo y no
 * al final, que es lo que hace el encoder. */
static void kmeans(Pal *p, int n)
{
    static int owner[NPX];
    double sr[MAXC], sg[MAXC], sb[MAXC];
    long cnt[MAXC];
    int i, it, c;

    p->n = n;
    for (i = 0; i < n; i++) {            /* siembra: pixeles al azar */
        int k = (int)(xr() % NPX);
        set_color(p, i, px[k * 3], px[k * 3 + 1], px[k * 3 + 2]);
    }
    for (it = 0; it < 40; it++) {
        for (c = 0; c < n; c++) { sr[c] = sg[c] = sb[c] = 0; cnt[c] = 0; }
        for (i = 0; i < NPX; i++) {
            int k = nearest(p, lab[i]);
            owner[i] = k;
            sr[k] += px[i * 3]; sg[k] += px[i * 3 + 1]; sb[k] += px[i * 3 + 2];
            cnt[k]++;
        }
        for (c = 0; c < n; c++) {
            if (!cnt[c]) {                /* centroide vacio: al azar */
                int k = (int)(xr() % NPX);
                set_color(p, c, px[k * 3], px[k * 3 + 1], px[k * 3 + 2]);
                continue;
            }
            set_color(p, c, sr[c] / cnt[c], sg[c] / cnt[c], sb[c] / cnt[c]);
        }
    }
    (void)owner;
}

/* EHB: los colores 32..63 son los 0..31 con cada componente a la mitad. Lo
 * hace el hardware, no se eligen. */
static void make_ehb(Pal *p)
{
    int i;

    for (i = 0; i < 32; i++) {
        A5Color c = p->rgb444[i];
        A5Color h = (A5Color)((((c >> 8) & 15) >> 1) << 8 |
                              (((c >> 4) & 15) >> 1) << 4 |
                              (((c) & 15) >> 1));
        uint8_t r, g, b;
        p->rgb444[32 + i] = h;
        a5_rgb444_to_srgb(h, &r, &g, &b);
        p->lab[32 + i] = a5_srgb_to_oklab(r, g, b);
    }
    p->n = 64;
}

/* Error medio y fraccion de pixeles a mas de 0,1, y el PPM doblado a
 * 320x192, que es como se ve en pantalla. */
static void report(const char *name, Pal *p, const char *ppm)
{
    double sum = 0;
    long bad = 0;
    int i;
    FILE *f;
    static uint8_t out[320 * 192 * 3];

    for (i = 0; i < NPX; i++) {
        int k = nearest(p, lab[i]);
        double d = sqrt((double)a5_oklab_dist2(lab[i], p->lab[k]));
        int x = i % W, y = i / W, dx, dy;
        uint8_t r, g, b;

        sum += d;
        if (d > 0.1) bad++;
        a5_rgb444_to_srgb(p->rgb444[k], &r, &g, &b);
        for (dy = 0; dy < 2; dy++)
            for (dx = 0; dx < 2; dx++) {
                size_t o = ((size_t)(y * 2 + dy) * 320 + x * 2 + dx) * 3;
                out[o] = r; out[o + 1] = g; out[o + 2] = b;
            }
    }
    printf("%-22s error %.4f  %5.2f%% de pixeles a mas de 0,1\n",
           name, sum / NPX, 100.0 * bad / NPX);
    if (!ppm) return;
    f = fopen(ppm, "wb");
    if (!f) return;
    fprintf(f, "P6\n320 192\n255\n");
    fwrite(out, 1, sizeof out, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *in = argc > 1 ? argv[1] : NULL;
    double t = argc > 2 ? atof(argv[2]) : 0;
    const char *prefix = argc > 3 ? argv[3] : "work\\ehb";
    char path[512];
    FILE *p;
    Pal pal;
    int i;

    if (!in) { printf("uso: ehbtest <video> [segundo] [prefijo]\n"); return 2; }

    p = a5_open_decoder(in, t, 0, "scale=160:96:flags=lanczos", W, H);
    if (!p || fread(px, 1, sizeof px, p) != sizeof px) {
        printf("no pude leer el frame\n");
        return 1;
    }
    pclose(p);
    for (i = 0; i < NPX; i++)
        lab[i] = a5_srgb_to_oklab(px[i * 3], px[i * 3 + 1], px[i * 3 + 2]);

    printf("frame de %s a los %.1f s, %dx%d logicos (RGB444, Oklab)\n\n",
           in, t, W, H);

    rng = 12345; kmeans(&pal, 16);
    snprintf(path, sizeof path, "%s16.ppm", prefix);
    report("16 colores (hoy)", &pal, path);

    rng = 12345; kmeans(&pal, 32);
    snprintf(path, sizeof path, "%s32.ppm", prefix);
    report("32 colores (5 planos)", &pal, path);

    rng = 12345; kmeans(&pal, 32); make_ehb(&pal);
    snprintf(path, sizeof path, "%sehb.ppm", prefix);
    report("64 EHB (32 + mitades)", &pal, path);

    rng = 12345; kmeans(&pal, 64);
    snprintf(path, sizeof path, "%s64.ppm", prefix);
    report("64 libres (no existe)", &pal, path);
    return 0;
}
