/* stream.h - el bitstream de A500VP. Lo usan el encoder y el decoder de
 * referencia, para que no puedan discrepar. La definicion en prosa esta en
 * docs/FORMAT.md y manda ese archivo.
 */
#ifndef A500VP_STREAM_H
#define A500VP_STREAM_H

#include "a500vp.h"

#define A5V_MAGIC        "A5VP"
#define A5V_VERSION      2
#define A5V_HEADER_SIZE  32

#define A5_MAX_PLANES    4
#define A5_ROWBYTES      (A5_W / 8)          /* 20 bytes logicos por fila */
#define A5_ROWMASK_SIZE  (A5_H / 8)          /* 16 bytes de mapa de filas */
#define A5_COLMASK_SIZE  3                   /* 20 bits de columnas, en 3 bytes */

/* op del paquete */
#define A5V_OP_DELTA     0
#define A5V_OP_REPEAT    1

/* flags del paquete */
#define A5V_F_PALETTE    0x01

/* Presupuesto por defecto: el disquete entero menos el bootblock menos un
 * hueco generoso para el reproductor. Se ajusta cuando el reproductor exista. */
#define A5V_DEFAULT_BUDGET  (901120 - 1024 - 16384)

/* --- modelo de costo de decodificacion ---------------------------------
 * CALIBRADO en el Hito 4 (2026-09-11). El disco de medicion cronometro con
 * el haz de video los 253 deltas de final22.a5v, en WinUAE A500 68000
 * cycle-exact con 3 bitplanes en pantalla. Minimos cuadrados:
 *
 *   ciclos = 5580 + 851,6 x filas + 177,5 x columnas
 *            (R2 = 1,0000, residuo maximo 0,14 ms)
 *
 * La fila cara es el recorrido de los 20 bits de la mascara de columnas:
 * ~36 ciclos por columna, este marcada o no. Por columna marcada, lo que se
 * midio es la suma de la columna y sus 3 bytes; el reparto entre las dos
 * (22 + 3 x 52) sale de contar instrucciones, no de la medicion. Con otra
 * cantidad de planos hay que volver a medir.
 */
#define A5_CPU_HZ            7093790.0
#define A5_CYC_FRAME         5580
#define A5_CYC_ROW           852
#define A5_CYC_COL           22
#define A5_CYC_BYTE          52
#define A5_FRAME_BUDGET_CYC  ((long)(A5_CPU_HZ * 0.040))
#define A5_CYC_PER_VBL       (A5_CPU_HZ / A5_VBL_HZ)   /* ~142103 */

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
long a5_delta_cost(const A5DeltaStats *st);

/* --- cabecera y doblado ------------------------------------------------- */

/* Escribe la cabecera de 32 bytes del bitstream (docs/FORMAT.md). */
void a5v_put_header(A5Buf *b, int planes, int ncolors, int audio_period,
                    int y0, int y1, uint32_t npackets, uint32_t payload);

/* Doblado horizontal: cada bit del byte logico se repite dos veces, el bit 7
 * termina en los bits 15-14. Es la misma tabla que arma el reproductor. */
uint16_t a5_double_byte(uint8_t v);

/* --- checksum de verificacion ------------------------------------------- */
uint32_t a5_crc32(const void *data, size_t n, uint32_t crc);

#endif /* A500VP_STREAM_H */
