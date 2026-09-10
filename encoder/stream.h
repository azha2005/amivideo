/* stream.h - el bitstream de A500VP. Lo usan el encoder y el decoder de
 * referencia, para que no puedan discrepar. La definicion en prosa esta en
 * docs/FORMAT.md y manda ese archivo.
 */
#ifndef A500VP_STREAM_H
#define A500VP_STREAM_H

#include "a500vp.h"

#define A5V_MAGIC        "A5VP"
#define A5V_VERSION      1
#define A5V_HEADER_SIZE  32

#define A5_ROWBYTES      (A5_W / 8)          /* 20 bytes logicos por fila */
#define A5_ROWMASK_SIZE  (A5_H / 8)          /* 16 bytes de mapa de filas */

/* op del paquete */
#define A5V_OP_DELTA     0
#define A5V_OP_REPEAT    1

/* flags del paquete */
#define A5V_F_PALETTE    0x01

/* Presupuesto por defecto: el disquete entero menos el bootblock menos un
 * hueco generoso para el reproductor. Se ajusta cuando el reproductor exista. */
#define A5V_DEFAULT_BUDGET  (901120 - 1024 - 16384)

/* --- modelo de costo de decodificacion ---------------------------------
 * SIN CALIBRAR. Los numeros salen de contar ciclos del 68000 a mano sobre el
 * lazo interno previsto (move.b (a0)+,d0 / add.w d0,d0 / lookup en la tabla
 * de doblado / move.w a chip RAM / dbf). No incluyen la contienda de DMA con
 * los bitplanes, que puede ser significativa. Se calibra en el Hito 4
 * midiendo el reproductor de verdad en WinUAE cycle-exact.
 */
#define A5_CPU_HZ            7093790.0
#define A5_CYC_FRAME         2000
#define A5_CYC_ROW           60
#define A5_CYC_PLANE         40
#define A5_CYC_RUN           50
#define A5_CYC_BYTE          50
#define A5_FRAME_BUDGET_CYC  ((long)(A5_CPU_HZ * 0.040))

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
    int  planerows;   /* pares (fila, plano) con datos */
    int  runs;
    int  bytes;       /* bytes literales escritos */
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

/* --- checksum de verificacion ------------------------------------------- */
uint32_t a5_crc32(const void *data, size_t n, uint32_t crc);

#endif /* A500VP_STREAM_H */
