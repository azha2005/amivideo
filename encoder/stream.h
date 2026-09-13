/* stream.h - el bitstream de A500VP. Lo usan el encoder y el decoder de
 * referencia, para que no puedan discrepar. La definicion en prosa esta en
 * docs/FORMAT.md y manda ese archivo.
 */
#ifndef A500VP_STREAM_H
#define A500VP_STREAM_H

#include "a500vp.h"

#define A5V_MAGIC        "A5VP"
#define A5V_VERSION      6
#define A5V_HEADER_SIZE  32

/* Formato del audio (byte 28 de la cabecera, desde la version 3). */
#define A5V_AUDIO_NONE   0
#define A5V_AUDIO_FIB4   1       /* Fibonacci-delta de 4 bits, 8SVX */
#define A5V_AUDIO_PCM8   2       /* PCM de 8 bits con signo */
#define A5V_AUDIO_ADPCM  3       /* IMA ADPCM de 4 bits (encoder/adpcm.c) */
#define A5V_HDR_AUDIOFMT 28

/* Paleta por franjas (byte 29, desde la version 4): filas logicas por
 * franja, desde y0; 0 = una sola paleta. Un paquete con paleta trae una
 * paleta completa por franja, y el color 0 es el mismo en todas. */
#define A5V_HDR_BANDROWS 29

/* Colores que cada franja puede cambiar respecto de la de arriba (byte 30,
 * desde la version 6). La franja 0 trae su paleta completa; las demas traen
 * exactamente band_colors pares (indice, color) y el Copper hace un MOVE por
 * par. Con band_colors = colores-1 se pueden cambiar todos, que es lo que
 * hacia la version 4 con 8 colores. */
#define A5V_HDR_BANDCOLORS 30
#define A5_MAX_BANDS     128

/* Cuantos colores alcanza a escribir el Copper al empezar una franja.
 * Cuenta de slots: el WAIT despierta en el color clock 8 y la imagen empieza
 * en el 64 (DDFSTRT $38, mas el retardo de fetch); un MOVE del Copper son 4
 * color clocks, y dos de ellos se van en los modulos del doblado vertical.
 * (64 - 8) / 4 - 2 = 12, menos lo que le roban el refresco de memoria (4
 * slots) y el audio (2) en esa misma ventana. Queda 8 con margen.
 *
 * La ventana esta ANTES de DDFSTRT, asi que no depende de cuantos bitplanes
 * haya: con 5 planos es la misma que con 3. El unico dato medido hasta ahora
 * es del Hito 6 (con 3 planos, 7 colores entran de sobra y 15 no), que es
 * consistente. Medido de verdad: ver DECISIONS.md. */
#define A5_BAND_COLORS_MAX 8

#define A5_MAX_PLANES    5
#define A5_ROWBYTES      (A5_W / 8)          /* 20 bytes logicos por fila */
#define A5_ROWMASK_SIZE  (A5_H / 8)          /* 16 bytes de mapa de filas */
#define A5_COLMASK_SIZE  3                   /* 20 bits de columnas, en 3 bytes */

/* op del paquete */
#define A5V_OP_DELTA     0
#define A5V_OP_REPEAT    1

/* flags del paquete */
#define A5V_F_PALETTE    0x01
/* Copia el area activa del buffer VISIBLE al oculto antes de aplicar el
 * delta (version 5, H12): asi el delta predice desde el ultimo frame
 * distinto en vez del penultimo. En la Amiga la hace el Blitter. */
#define A5V_F_COPY       0x02

/* Presupuesto por defecto: el disquete entero menos el bootblock menos un
 * hueco generoso para el reproductor. Se ajusta cuando el reproductor exista. */
#define A5V_DEFAULT_BUDGET  (901120 - 1024 - 16384)

/* --- modelo de costo de decodificacion ---------------------------------
 * CALIBRADO en el Hito 4 (2026-09-11) y RECALIBRADO en el H10 (2026-09-12),
 * cuando el decodificador del reproductor se desenrollo. El disco de
 * medicion cronometra con el haz de video cada delta, en WinUAE A500 68000
 * cycle-exact con 3 bitplanes en pantalla. Minimos cuadrados:
 *
 *   Hito 4:  ciclos = 5580 + 851,6 x filas + 177,5 x columnas
 *                     (253 deltas de final22.a5v, R2 = 1,0000)
 *   H10:     ciclos =  -1243 + 900,5 x filas + 126,6 x columnas
 *                     (233 deltas de fmab_op.a5v, R2 = 0,9910)
 *
 * La fila cara es el recorrido de los 20 bits de la mascara de columnas:
 * ~36 ciclos por columna, este marcada o no. El H10 saltea 8 columnas de
 * una cuando el byte de mascara es cero, pero en material real las filas
 * marcadas son densas y casi nunca dispara: el costo por fila hasta subio
 * un poco (los dos cmp.l del atajo). Lo que si bajo mucho es la columna,
 * de 177,5 a 126,6, por desenrollar la escritura de planos (se fueron el
 * dbf y el lea de cada plano).
 *
 * Por columna marcada lo medido es la suma de la columna y sus bytes; el
 * reparto entre las dos (19 + planos x A5_CYC_BYTE) sale de contar
 * instrucciones, no de la medicion. El intercepto del ajuste dio negativo,
 * que es un artefacto: se usa 0.
 *
 * El costo por byte NO es el mismo con cualquier cantidad de planos,
 * porque el DMA de bitplanes le roba slots de bus a la CPU. Medido con el
 * mismo metodo, manteniendo 900 ciclos por fila:
 *
 *   3 planos: 36 ciclos por byte (127 por columna), medido 126,6
 *   4 planos: 36 ciclos por byte (163 por columna), medido 168,5
 *   5 planos: 43 ciclos por byte (234 por columna), medido 235,6
 *
 * Con 3 y 4 planos el byte cuesta lo mismo; con 5 cuesta un 19 % mas. En
 * lowres el DMA de bitplanes usa slots que con 4 planos o menos le sobran
 * a la CPU, y con 5 se los empieza a sacar. 1 y 2 planos no se midieron:
 * usan el mismo 36.
 */
#define A5_CPU_HZ            7093790.0
#define A5_CYC_FRAME         0
#define A5_CYC_ROW           900
#define A5_CYC_COL           19
#define A5_CYC_BYTE          36
#define A5_CYC_BYTE_5PL      43

/* Ciclos por byte literal escrito, con `planes` bitplanes en pantalla. */
long a5_cyc_byte(int planes);
#define A5_FRAME_BUDGET_CYC  ((long)(A5_CPU_HZ * 0.040))
#define A5_CYC_PER_VBL       (A5_CPU_HZ / A5_VBL_HZ)   /* ~142103 */

/* --- costo del audio en la CPU -------------------------------------------
 * El reproductor llena un buffer de Paula de A5_AUD_BUF_SAMPLES muestras
 * en cada interrupcion de nivel 4, que llega cada 512 x periodo color
 * clocks desde el VBL del frame 0. Ese tiempo se lo roba al delta que se
 * este decodificando. Ciclos de CPU por llenado, segun el formato, medidos
 * con el disco de medicion (Hito 5, 2026-09-11):
 *
 *   fib4: 32015 de media dentro de la rutina (345 llenados, el peor
 *         32830), mas ~300 de entrar y salir de la interrupcion, contados
 *         a mano. Es el 7,1 % de la CPU a 8006,5 Hz.
 *   pcm8: 27918 de media (el peor 28690), mas los mismos ~300: 6,2 %. */
#define A5_AUD_BUF_SAMPLES   512
#define A5_CYC_AUDIO_FILL    32300
#define A5_CYC_AUDIO_FILL_PCM8 28200
/* ADPCM: ESTIMADO contando instrucciones del decodificador de A5MU
 * (player/music.s, adpcm_step), no medido todavia en el disco de medicion.
 * Son ~115 ciclos por muestra contra ~63 de fib4: el paso adaptativo se
 * paga en CPU. Medirlo es lo primero que hay que hacer si esto se usa. */
#define A5_CYC_AUDIO_FILL_ADPCM 59000

/* --- costo de la copia con el Blitter (H12) ------------------------------
 * Medido el 2026-09-12 con el disco de medicion, copiando 16 veces el area
 * activa de un framebuffer al otro con la imagen en pantalla (o sea con la
 * contencion de DMA real) y la CPU esperando al Blitter:
 *
 *   Blitter normal: 8,814 ms de media (62 527 ciclos), el peor 10,191 ms
 *   Blitter nasty : 6,685 ms de media (47 422 ciclos), el peor 7,358 ms
 *
 * Con 4 planos y 94 filas activas, nasty da 47422 / (4 x 94) = 126 ciclos
 * por plano y por fila. El reproductor usa nasty siempre: la CPU no tiene
 * nada que hacer mientras espera.
 *
 * Con 5 planos, medido en el propio lazo de reproduccion (151 copias, con
 * audio): 9,259 ms de media. De ahi hay que descontar los llenados de
 * audio que caen dentro de la espera, que la linea de tiempo ya cuenta
 * aparte: si un llenado (5,04 ms) cae con probabilidad copia/64 ms, la
 * copia pura queda en 8,58 ms = 60 870 ciclos, o 130 por plano y por fila.
 * La peor copia medida, 14,885 ms, es justo una con un llenado adentro. */
#define A5_CYC_BLIT          126
#define A5_CYC_BLIT_5PL      130

/* Ciclos de la copia del visible al oculto, por plano y por fila activa. */
long a5_cyc_blit(int planes);

long a5_audio_fill_cost(int format);

/* --- buffer de bytes que crece ----------------------------------------- */
typedef struct { uint8_t *p; size_t len, cap; } A5Buf;

void a5buf_init(A5Buf *b);
void a5buf_free(A5Buf *b);
void a5buf_put8(A5Buf *b, unsigned v);
void a5buf_put16(A5Buf *b, unsigned v);
void a5buf_put32(A5Buf *b, uint32_t v);
void a5buf_write(A5Buf *b, const void *src, size_t n);

/* --- conversion entre indices y planos --------------------------------- */
/* idx_row: A5_W bytes con el indice de paleta de cada pixel logico.
 * planar : planes * A5_ROWBYTES bytes, plano 0 primero, bit 7 = pixel de mas
 *          a la izquierda. */
void a5_planarize_row(const uint8_t *idx_row, int planes, uint8_t *planar);
void a5_depack_row(const uint8_t *planar, int planes, uint8_t *idx_row);

/* --- delta -------------------------------------------------------------- */
typedef struct {
    int  rows;        /* filas logicas modificadas */
    int  cols;        /* columnas (byte logico x todos los planos) escritas */
    int  bytes;       /* bytes literales escritos = cols * planos */
    int  same;        /* de esos, los que escriben lo que ya estaba */
    long cycles;      /* costo estimado de decodificacion */
} A5DeltaStats;

/* Codifica la diferencia entre el buffer oculto y el frame objetivo, los dos
 * como arreglos de indices de A5_W*A5_H. Devuelve 1 si hubo algo que escribir. */
int  a5_delta_encode(A5Buf *out, const uint8_t *hidden, const uint8_t *target,
                     int planes, A5DeltaStats *st);

/* Aplica un delta sobre un framebuffer planar de planes*A5_H*A5_ROWBYTES.
 * Devuelve el puntero justo despues del delta, o NULL si el dato esta roto. */
const uint8_t *a5_delta_apply(uint8_t *fb, const uint8_t *data,
                              const uint8_t *end, int planes,
                              A5DeltaStats *st);

/* Cuenta el costo de un delta ya codificado sin aplicarlo. */
long a5_delta_cost(const A5DeltaStats *st, int planes);

/* --- cabecera y doblado ------------------------------------------------- */

/* Escribe la cabecera de 32 bytes del bitstream (docs/FORMAT.md). */
void a5v_put_header(A5Buf *b, int planes, int ncolors, int audio_format,
                    int audio_period, int y0, int y1, int band_rows,
                    int band_colors, uint32_t npackets, uint32_t payload);

/* Palabras de paleta que lleva un paquete con paleta: la franja 0 entera
 * mas band_colors pares (indice, color) por cada franja siguiente. */
int  a5v_palette_words(int ncolors, int nbands, int band_colors);

/* Cuantas franjas hay, y a cual pertenece la fila logica y. Las filas fuera
 * de y0..y1-1 usan la franja 0 (son color 0, igual en todas). */
int  a5v_nbands(int band_rows, int y0, int y1);
int  a5v_band_of(int y, int band_rows, int y0, int nbands);

/* --- audio ---------------------------------------------------------------
 * El audio de todos los paquetes, puesto uno detras del otro, es un solo
 * flujo continuo. Cada paquete lleva una cantidad par de muestras.
 *
 * fib4: dos muestras por byte, nibble alto primero. Cada nibble indexa
 * a5_fib_table y se suma a un acumulador de 8 bits que arranca en 0 al
 * principio del flujo y sigue de un paquete al otro. El encoder elige los
 * nibbles para que el acumulador nunca de la vuelta.
 * pcm8: una muestra por byte, con signo. */
extern const int8_t a5_fib_table[16];

/* Codifica n muestras (n par, en escala de 8 bits con signo) a n/2 bytes.
 * recon recibe lo que va a sonar (puede ser NULL). acc es el acumulador,
 * que se conserva entre llamadas. */
void a5_fib4_encode(const float *in, size_t n, uint8_t *out, int8_t *recon,
                    int *acc);
void a5_fib4_decode(const uint8_t *in, size_t nbytes, int *acc, int8_t *out);

/* Muestras que suenan mientras se ven los frames 0..n inclusive: la
 * acumulacion fraccional con las frecuencias exactas, redondeada a par. */
uint32_t a5_audio_samples_through(uint32_t n, double paula_hz);

/* Bytes de audio de una cantidad de muestras, segun el formato. */
size_t a5_audio_bytes(int format, size_t samples);

/* Doblado horizontal: cada bit del byte logico se repite dos veces, el bit 7
 * termina en los bits 15-14. Es la misma tabla que arma el reproductor. */
uint16_t a5_double_byte(uint8_t v);

/* --- checksum de verificacion ------------------------------------------- */
uint32_t a5_crc32(const void *data, size_t n, uint32_t crc);

#endif /* A500VP_STREAM_H */
