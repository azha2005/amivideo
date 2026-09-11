/* mkadf.c - arma el .adf de A500VP.
 *
 *   mkadf --boot boot.bin --player player.bin [--data video.a5v]
 *         [--reserve-tail N] --out imagen.adf
 *
 * Bootblock en los sectores 0-1 (con su checksum y la longitud del
 * reproductor), el reproductor desde el sector 2 y, si hay datos, los datos
 * desde el sector siguiente al reproductor. Al reproductor se le escribe en
 * su cabecera "A5PL" donde quedaron los datos. Ver docs/FORMAT.md.
 *
 * --reserve-tail N deja libres los ultimos N sectores, donde los
 * reproductores de prueba vuelcan lo que hay que verificar desde el PC.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "adf.h"

#define PLAYER_SECTOR   ADF_BOOT_SECTORS   /* el reproductor arranca en el 2 */

/* Cabecera del reproductor (player/player.s) */
#define PL_OFF_MAGIC    4
#define PL_OFF_DATAOFF  8
#define PL_OFF_DATALEN  12
#define PL_HEADER_SIZE  16

static uint8_t disk[ADF_SIZE];

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (!f) { perror(path); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    rewind(f);
    if (n < 0) { fclose(f); return NULL; }
    buf = malloc((size_t)n ? (size_t)n : 1);
    if (!buf) { fclose(f); return NULL; }
    if (n && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(buf); return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static void usage(void)
{
    fprintf(stderr,
            "uso: mkadf --boot <bootblock.bin> --player <player.bin>\n"
            "           [--data <video.a5v>] [--reserve-tail N]\n"
            "           --out <imagen.adf>\n");
}

int main(int argc, char **argv)
{
    const char *bootpath = NULL, *playerpath = NULL, *datapath = NULL;
    const char *outpath = NULL;
    uint8_t *boot = NULL, *player = NULL, *data = NULL;
    size_t bootlen = 0, playerlen = 0, datalen = 0, playerpadded;
    uint32_t used, datasector = 0, limit;
    int reserve = 0;
    FILE *out;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--boot") && i + 1 < argc)        bootpath = argv[++i];
        else if (!strcmp(argv[i], "--player") && i + 1 < argc) playerpath = argv[++i];
        else if (!strcmp(argv[i], "--data") && i + 1 < argc)   datapath = argv[++i];
        else if (!strcmp(argv[i], "--reserve-tail") && i + 1 < argc)
            reserve = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)    outpath = argv[++i];
        else { usage(); return 2; }
    }
    if (!bootpath || !playerpath || !outpath) { usage(); return 2; }
    if (reserve < 0 || reserve >= ADF_SECTORS) { usage(); return 2; }
    limit = ADF_SECTORS - (uint32_t)reserve;

    boot = slurp(bootpath, &bootlen);
    player = slurp(playerpath, &playerlen);
    if (!boot || !player) return 1;
    if (datapath) {
        data = slurp(datapath, &datalen);
        if (!data) return 1;
    }

    if (bootlen > ADF_BOOT_SIZE) {
        fprintf(stderr, "el bootblock ocupa %lu bytes, no entra en %d\n",
                (unsigned long)bootlen, ADF_BOOT_SIZE);
        return 1;
    }
    if (bootlen < BB_OFF_CODE || memcmp(boot, "DOS", 3) != 0 || boot[3] != 0) {
        fprintf(stderr, "%s no empieza con \"DOS\\0\": no es un bootblock\n",
                bootpath);
        return 1;
    }
    if (playerlen == 0) {
        fprintf(stderr, "el reproductor esta vacio\n");
        return 1;
    }

    /* El bootblock lee sectores enteros, asi que la longitud va redondeada. */
    playerpadded = (size_t)adf_sectors_for(playerlen) * ADF_SECTOR_SIZE;
    used = PLAYER_SECTOR + adf_sectors_for(playerlen);

    if (data) {
        if (playerlen < PL_HEADER_SIZE ||
            memcmp(player + PL_OFF_MAGIC, "A5PL", 4) != 0) {
            fprintf(stderr, "%s no tiene la cabecera \"A5PL\": no sabe donde "
                    "buscar los datos\n", playerpath);
            return 1;
        }
        datasector = used;
        be_put_u32(player + PL_OFF_DATAOFF, datasector * ADF_SECTOR_SIZE);
        be_put_u32(player + PL_OFF_DATALEN, (uint32_t)datalen);
        used = datasector + adf_sectors_for(datalen);
    }
    if (used > limit) {
        fprintf(stderr, "no entra: hacen falta %u sectores y hay %u "
                "(%d reservados al final)\n", used, limit, reserve);
        return 1;
    }

    memset(disk, 0, sizeof disk);
    memcpy(disk, boot, bootlen);
    be_put_u32(disk + BB_OFF_STAGE2LEN, (uint32_t)playerpadded);
    adf_boot_finalize(disk);

    if (!adf_boot_valid(disk)) {
        fprintf(stderr, "checksum de bootblock invalido (error interno)\n");
        return 1;
    }
    if (adf_put(disk, PLAYER_SECTOR, player, playerlen) != 0 ||
        (data && adf_put(disk, datasector, data, datalen) != 0)) {
        fprintf(stderr, "no entra en el disco\n");
        return 1;
    }

    out = fopen(outpath, "wb");
    if (!out) { perror(outpath); return 1; }
    if (fwrite(disk, 1, sizeof disk, out) != sizeof disk) {
        perror(outpath); fclose(out); return 1;
    }
    fclose(out);

    printf("%s\n", outpath);
    printf("  bootblock : %lu bytes de %d, checksum %08lX\n",
           (unsigned long)bootlen, ADF_BOOT_SIZE,
           (unsigned long)be_get_u32(disk + BB_OFF_CHECKSUM));
    printf("  player    : %lu bytes -> %lu (sectores %d..%u)\n",
           (unsigned long)playerlen, (unsigned long)playerpadded,
           PLAYER_SECTOR, PLAYER_SECTOR + adf_sectors_for(playerlen) - 1);
    if (data)
        printf("  datos     : %lu bytes (sectores %u..%u, offset %u)\n",
               (unsigned long)datalen, datasector, used - 1,
               datasector * ADF_SECTOR_SIZE);
    printf("  libre     : %u sectores = %lu bytes\n", limit - used,
           (unsigned long)(limit - used) * ADF_SECTOR_SIZE);
    if (reserve)
        printf("  reservados: los ultimos %d sectores (%u..%d), para "
               "diagnostico\n", reserve, limit, ADF_SECTORS - 1);

    free(boot);
    free(player);
    free(data);
    return 0;
}
