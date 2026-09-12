/* bandtest.c - simulacion hipotetica de "mas colores sin mas planos":
 * paleta por franjas con el Copper, cambiando SOLO unos pocos colores en
 * cada limite de franja.
 *
 * Con 5 planos no entran 31 MOVE de color en el borde horizontal, pero si
 * entran unos pocos (ver DECISIONS.md: la ventana esta antes de DDFSTRT y
 * no la achican los planos). Esto mide cuanto se ganaria cambiando K
 * colores por franja, contra la paleta unica de hoy y contra el techo
 * inalcanzable de una paleta entera por franja.
 *
 * Reglas del formato que se respetan (docs/FORMAT.md):
 *   - el color 0 es negro y es el mismo en todas las franjas (es el borde);
 *   - la franja 0 arranca con su paleta completa (la escribe el delta);
 *   - cada franja siguiente hereda la anterior y cambia K entradas.
 *
 * No es parte del build: se compila a mano.
 *   gcc -std=c11 -O2 color.c vio.c bandtest.c -o bandtest -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "a500vp.h"

#define W     160
#define H      96
#define NPX   (W * H)
#define NCOL   32
#define MAXBANDS 16

typedef struct {
    A5Color rgb444[NCOL];
    Oklab   lab[NCOL];
} Pal;

static uint8_t  px[NPX * 3];
static Oklab    lab[NPX];
static uint32_t rng = 12345;

static uint32_t xr(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

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

static void set_black(Pal *p, int i)
{
    uint8_t r, g, b;
    p->rgb444[i] = 0x000;
    a5_rgb444_to_srgb(0x000, &r, &g, &b);
    p->lab[i] = a5_srgb_to_oklab(r, g, b);
}

static double dist(Oklab a, Oklab b) { return sqrt((double)a5_oklab_dist2(a, b)); }

/* k-means sobre los pixeles [first, first+n), con el color 0 reservado a
 * negro y los centroides redondeados a RGB444 en cada vuelta, igual que el
 * encoder. */
static void kmeans(Pal *p, int first, int n)
{
    double sr[NCOL], sg[NCOL], sb[NCOL];
    long cnt[NCOL];
    int i, it, c;

    set_black(p, 0);
    for (c = 1; c < NCOL; c++) {
        int k = first + (int)(xr() % (unsigned)n);
        set_color(p, c, px[k * 3], px[k * 3 + 1], px[k * 3 + 2]);
    }
    for (it = 0; it < 40; it++) {
        for (c = 0; c < NCOL; c++) { sr[c] = sg[c] = sb[c] = 0; cnt[c] = 0; }
        for (i = first; i < first + n; i++) {
            int best = 0, k;
            double bd = 1e30;
            for (k = 0; k < NCOL; k++) {
                double d = dist(lab[i], p->lab[k]);
                if (d < bd) { bd = d; best = k; }
            }
            sr[best] += px[i * 3]; sg[best] += px[i * 3 + 1];
            sb[best] += px[i * 3 + 2];
            cnt[best]++;
        }
        for (c = 1; c < NCOL; c++) {           /* el 0 no se mueve */
            if (!cnt[c]) {
                int k = first + (int)(xr() % (unsigned)n);
                set_color(p, c, px[k * 3], px[k * 3 + 1], px[k * 3 + 2]);
                continue;
            }
            set_color(p, c, sr[c] / cnt[c], sg[c] / cnt[c], sb[c] / cnt[c]);
        }
    }
}

static double band_error(const Pal *p, int first, int n)
{
    double sum = 0;
    int i, k;

    for (i = first; i < first + n; i++) {
        double bd = 1e30;
        for (k = 0; k < NCOL; k++) {
            double d = dist(lab[i], p->lab[k]);
            if (d < bd) bd = d;
        }
        sum += bd;
    }
    return sum;
}

/* Cambia K entradas de p (heredada de la franja de arriba) para que sirva
 * lo mejor posible a los pixeles [first, first+n).
 *
 * Cada vuelta prueba todas las combinaciones (entrada s, color candidato c)
 * y se queda con la que mas baja el error. Los candidatos son la paleta
 * ideal de esta franja. El costo se calcula en una sola pasada por pixel y
 * por candidato: para los pixeles que hoy usan s hay que caer al segundo
 * mejor si el nuevo color no les sirve; para el resto, el nuevo color solo
 * puede mejorar. */
static void swap_colors(Pal *p, int first, int n, const Pal *cand, int K)
{
    static double d1[NPX], d2[NPX], dc[NPX];
    static int own[NPX];
    double corr[NCOL];
    int k, i, c, s, bs, bc;
    double base, bestval;

    for (k = 0; k < K; k++) {
        base = 0;
        for (i = first; i < first + n; i++) {           /* mejor y segundo */
            double a = 1e30, b = 1e30;
            int w = 0, j;
            for (j = 0; j < NCOL; j++) {
                double d = dist(lab[i], p->lab[j]);
                if (d < a) { b = a; a = d; w = j; }
                else if (d < b) b = d;
            }
            d1[i] = a; d2[i] = b; own[i] = w;
            base += a;
        }
        bestval = base; bs = -1; bc = -1;

        for (c = 1; c < NCOL; c++) {                    /* candidato */
            double keep = 0;
            for (s = 0; s < NCOL; s++) corr[s] = 0;
            for (i = first; i < first + n; i++) {
                double m;
                dc[i] = dist(lab[i], cand->lab[c]);
                m = dc[i] < d1[i] ? dc[i] : d1[i];
                keep += m;
                /* si la entrada que se pisa era la suya, el piso es el
                 * segundo mejor, no el mejor */
                corr[own[i]] += (dc[i] < d2[i] ? dc[i] : d2[i]) - m;
            }
            for (s = 1; s < NCOL; s++) {                /* el 0 no se toca */
                double e = keep + corr[s];
                if (e < bestval - 1e-9) { bestval = e; bs = s; bc = c; }
            }
        }
        if (bs < 0) break;                              /* no mejora mas */
        p->rgb444[bs] = cand->rgb444[bc];
        p->lab[bs] = cand->lab[bc];
    }
}

/* band-snap, que el encoder ya hace con las franjas de 3 planos: un color
 * de la franja que quede a menos de thr (Oklab) de uno de la paleta de
 * referencia se vuelve ESE color. Cuantizar cada franja por su cuenta
 * corre los colores de las zonas lisas grandes (una pared blanca se va a
 * rosa) y el ojo lo ve aunque el error medio no lo castigue. */
static void snap(Pal *p, const Pal *ref, double thr)
{
    int c, k;

    if (thr <= 0) return;
    for (c = 1; c < NCOL; c++) {
        int best = -1;
        double bd = thr;
        for (k = 1; k < NCOL; k++) {
            double d = dist(p->lab[c], ref->lab[k]);
            if (d < bd) { bd = d; best = k; }
        }
        if (best >= 0) { p->rgb444[c] = ref->rgb444[best];
                         p->lab[c] = ref->lab[best]; }
    }
}

/* Dibuja y reporta. pals[b] es la paleta de la franja b. */
static void report(const char *name, const Pal *pals, int brows, int nb,
                   const char *ppm)
{
    static uint8_t out[320 * 192 * 3];
    double sum = 0;
    long bad = 0;
    int i;
    FILE *f;

    for (i = 0; i < NPX; i++) {
        int y = i / W, x = i % W, b = y / brows, k, best = 0, dx, dy;
        double bd = 1e30;
        uint8_t r, g, bl;

        if (b >= nb) b = nb - 1;
        for (k = 0; k < NCOL; k++) {
            double d = dist(lab[i], pals[b].lab[k]);
            if (d < bd) { bd = d; best = k; }
        }
        sum += bd;
        if (bd > 0.1) bad++;
        a5_rgb444_to_srgb(pals[b].rgb444[best], &r, &g, &bl);
        for (dy = 0; dy < 2; dy++)
            for (dx = 0; dx < 2; dx++) {
                size_t o = ((size_t)(y * 2 + dy) * 320 + x * 2 + dx) * 3;
                out[o] = r; out[o + 1] = g; out[o + 2] = bl;
            }
    }
    printf("%-30s error %.4f  %5.2f%% de pixeles a mas de 0,1\n",
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
    const char *prefix = argc > 3 ? argv[3] : "work\\band";
    int brows = argc > 4 ? atoi(argv[4]) : 16;
    double snapthr = argc > 5 ? atof(argv[5]) : 0.04;
    int nb = (H + brows - 1) / brows;
    static Pal pals[MAXBANDS], ideal[MAXBANDS], one;
    char path[512];
    FILE *p;
    int i, b, kk;
    static const int Ks[] = { 4, 6, 8 };

    if (!in) {
        printf("uso: bandtest <video> [segundo] [prefijo] [filas por franja]\n");
        return 2;
    }
    if (nb > MAXBANDS) { printf("demasiadas franjas\n"); return 2; }

    p = a5_open_decoder(in, t, 0, "scale=160:96:flags=lanczos", W, H);
    if (!p || fread(px, 1, sizeof px, p) != sizeof px) {
        printf("no pude leer el frame\n");
        return 1;
    }
    pclose(p);
    for (i = 0; i < NPX; i++)
        lab[i] = a5_srgb_to_oklab(px[i * 3], px[i * 3 + 1], px[i * 3 + 2]);

    printf("frame de %s a los %.1f s, %dx%d logicos, %d franjas de %d filas\n"
           "32 colores (5 planos), color 0 negro y comun a todas\n\n",
           in, t, W, H, nb, brows);

    /* Hoy: una sola paleta para todo el frame. */
    rng = 12345; kmeans(&one, 0, NPX);
    for (b = 0; b < nb; b++) pals[b] = one;
    snprintf(path, sizeof path, "%s_una.ppm", prefix);
    report("una paleta (hoy)", pals, brows, nb, path);

    /* La paleta ideal de cada franja: candidata, y techo. */
    for (b = 0; b < nb; b++) {
        int first = b * brows * W;
        int n = (b == nb - 1 ? H - b * brows : brows) * W;
        rng = 12345 + (uint32_t)b;
        kmeans(&ideal[b], first, n);
        snap(&ideal[b], &one, snapthr);
    }

    /* Lo que si se puede: la franja 0 con su paleta completa y cada una de
     * las siguientes cambiando K colores. */
    for (kk = 0; kk < (int)(sizeof Ks / sizeof *Ks); kk++) {
        int K = Ks[kk];
        char name[64];

        pals[0] = ideal[0];
        for (b = 1; b < nb; b++) {
            int first = b * brows * W;
            int n = (b == nb - 1 ? H - b * brows : brows) * W;
            pals[b] = pals[b - 1];
            swap_colors(&pals[b], first, n, &ideal[b], K);
        }
        snprintf(name, sizeof name, "franjas, %d colores por franja", K);
        snprintf(path, sizeof path, "%s_k%d.ppm", prefix, K);
        report(name, pals, brows, nb, path);
    }

    /* Techo: paleta entera por franja (el Copper no llega). */
    for (b = 0; b < nb; b++) pals[b] = ideal[b];
    snprintf(path, sizeof path, "%s_full.ppm", prefix);
    report("franjas, paleta entera (no entra)", pals, brows, nb, path);

    /* Cuanto de la diferencia es la franja y cuanto el frame entero. */
    {
        double e = 0;
        for (b = 0; b < nb; b++) {
            int first = b * brows * W;
            int n = (b == nb - 1 ? H - b * brows : brows) * W;
            e += band_error(&ideal[b], first, n);
        }
        printf("\n(control: error de las paletas ideales = %.4f)\n", e / NPX);
    }
    return 0;
}
