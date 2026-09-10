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
