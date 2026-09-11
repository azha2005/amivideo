/* adf.h - escritura del contenedor ADF y del bootblock.
 *
 * Un ADF es una imagen cruda de disquete DD de Amiga:
 *   80 cilindros x 2 caras x 11 sectores x 512 bytes = 901120 bytes.
 * Sin sistema de archivos: A500VP escribe bootblock, reproductor y datos
 * en sectores fijos. Ver docs/FORMAT.md.
 */
#ifndef A500VP_ADF_H
#define A500VP_ADF_H

#include <stddef.h>
#include <stdint.h>

#define ADF_SECTOR_SIZE   512
#define ADF_SECTORS       1760                /* 80 * 2 * 11 */
#define ADF_SIZE          (ADF_SECTOR_SIZE * ADF_SECTORS)   /* 901120 */

#define ADF_BOOT_SECTORS  2
#define ADF_BOOT_SIZE     (ADF_BOOT_SECTORS * ADF_SECTOR_SIZE)  /* 1024 */

/* Desplazamientos dentro del bootblock (deben coincidir con player/boot.s). */
#define BB_OFF_MAGIC      0    /* "DOS\0"                                  */
#define BB_OFF_CHECKSUM   4    /* checksum, lo pone adf_boot_finalize()    */
#define BB_OFF_STAGE2LEN  8    /* longitud del reproductor, en bytes       */
#define BB_OFF_CODE       12   /* primera instruccion                      */

/* Enteros big-endian: el 68000 lee el disco con punteros pelados. */
void     be_put_u32(uint8_t *p, uint32_t v);
uint32_t be_get_u32(const uint8_t *p);

/* Suma de las 256 longwords big-endian con acarreo circular. */
uint32_t adf_boot_sum(const uint8_t *boot);

/* Escribe en boot[4] el checksum que espera el Kickstart. */
void adf_boot_finalize(uint8_t *boot);

/* 1 si el bootblock pasa la validacion del Kickstart (suma == 0xFFFFFFFF). */
int adf_boot_valid(const uint8_t *boot);

/* Copia len bytes en el sector indicado. Devuelve 0 si entra, -1 si no. */
int adf_put(uint8_t *disk, uint32_t sector, const uint8_t *data, size_t len);

/* Sectores necesarios para len bytes. */
uint32_t adf_sectors_for(size_t len);

/* Cabecera del reproductor (player/player.s): "A5PL" y donde estan los datos */
#define PL_OFF_MAGIC     4
#define PL_OFF_DATAOFF   8
#define PL_OFF_DATALEN   12
#define PL_HEADER_SIZE   16
#define ADF_PLAYER_SECTOR ADF_BOOT_SECTORS    /* el reproductor arranca en el 2 */

typedef struct {
    uint32_t player_sectors;
    uint32_t data_sector;      /* 0 si no hay datos */
    uint32_t used;             /* primer sector libre */
    uint32_t limit;            /* sectores disponibles (sin la cola reservada) */
} AdfLayout;

/* Arma la imagen completa en disk (ADF_SIZE bytes): bootblock con su
 * checksum y la longitud del reproductor, el reproductor desde el sector 2
 * y, si hay datos, los datos en el primer sector libre despues del
 * reproductor, anotando en la cabecera "A5PL" del reproductor (que se
 * modifica) donde quedaron. reserve deja libres los ultimos sectores.
 * Devuelve NULL si salio bien, o el motivo del error. */
const char *adf_assemble(uint8_t *disk, const uint8_t *boot, size_t bootlen,
                         uint8_t *player, size_t playerlen,
                         const uint8_t *data, size_t datalen, int reserve,
                         AdfLayout *lay);

#endif /* A500VP_ADF_H */
