/* adf.c - ver adf.h */
#include <string.h>
#include "adf.h"

void be_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

uint32_t be_get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

uint32_t adf_boot_sum(const uint8_t *boot)
{
    uint32_t sum = 0;
    int i;

    for (i = 0; i < ADF_BOOT_SIZE / 4; i++) {
        uint32_t prev = sum;
        sum += be_get_u32(boot + i * 4);
        if (sum < prev)         /* acarreo circular */
            sum++;
    }
    return sum;
}

void adf_boot_finalize(uint8_t *boot)
{
    uint32_t sum;

    be_put_u32(boot + BB_OFF_CHECKSUM, 0);
    sum = adf_boot_sum(boot);
    be_put_u32(boot + BB_OFF_CHECKSUM, ~sum);
}

int adf_boot_valid(const uint8_t *boot)
{
    /* Esto es lo que hace el Kickstart: sumar las 256 longwords, incluido
     * el propio campo de checksum, y exigir 0xFFFFFFFF. */
    return adf_boot_sum(boot) == 0xFFFFFFFFu;
}

uint32_t adf_sectors_for(size_t len)
{
    return (uint32_t)((len + ADF_SECTOR_SIZE - 1) / ADF_SECTOR_SIZE);
}

int adf_put(uint8_t *disk, uint32_t sector, const uint8_t *data, size_t len)
{
    size_t off = (size_t)sector * ADF_SECTOR_SIZE;

    if (off + len > ADF_SIZE)
        return -1;
    memcpy(disk + off, data, len);
    return 0;
}

const char *adf_assemble(uint8_t *disk, const uint8_t *boot, size_t bootlen,
                         uint8_t *player, size_t playerlen,
                         const uint8_t *data, size_t datalen, int reserve,
                         AdfLayout *lay)
{
    size_t playerpadded;

    if (bootlen > ADF_BOOT_SIZE)
        return "el bootblock no entra en 1024 bytes";
    if (bootlen < BB_OFF_CODE || memcmp(boot, "DOS", 3) != 0 || boot[3] != 0)
        return "el bootblock no empieza con \"DOS\\0\"";
    if (playerlen == 0)
        return "el reproductor esta vacio";
    if (reserve < 0 || reserve >= ADF_SECTORS)
        return "cola reservada invalida";

    memset(lay, 0, sizeof *lay);
    lay->limit = ADF_SECTORS - (uint32_t)reserve;
    lay->player_sectors = adf_sectors_for(playerlen);
    lay->used = ADF_PLAYER_SECTOR + lay->player_sectors;

    /* El bootblock lee sectores enteros, asi que la longitud va redondeada. */
    playerpadded = (size_t)lay->player_sectors * ADF_SECTOR_SIZE;

    if (data) {
        if (playerlen < PL_HEADER_SIZE ||
            memcmp(player + PL_OFF_MAGIC, "A5PL", 4) != 0)
            return "el reproductor no tiene la cabecera \"A5PL\": no sabe "
                   "donde buscar los datos";
        lay->data_sector = lay->used;
        be_put_u32(player + PL_OFF_DATAOFF, lay->data_sector * ADF_SECTOR_SIZE);
        be_put_u32(player + PL_OFF_DATALEN, (uint32_t)datalen);
        lay->used = lay->data_sector + adf_sectors_for(datalen);
    }
    if (lay->used > lay->limit)
        return "no entra en el disquete";

    memset(disk, 0, ADF_SIZE);
    memcpy(disk, boot, bootlen);
    be_put_u32(disk + BB_OFF_STAGE2LEN, (uint32_t)playerpadded);
    adf_boot_finalize(disk);
    if (!adf_boot_valid(disk))
        return "checksum de bootblock invalido (error interno)";

    if (adf_put(disk, ADF_PLAYER_SECTOR, player, playerlen) != 0 ||
        (data && adf_put(disk, lay->data_sector, data, datalen) != 0))
        return "no entra en el disquete";
    return NULL;
}
