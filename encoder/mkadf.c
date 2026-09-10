/* mkadf.c - arma el .adf de A500VP.
 *
 *   mkadf --boot boot.bin --player memcheck.bin --out a500vp.adf
 *
 * Escribe el bootblock (con su checksum y la longitud del reproductor),
 * el reproductor a partir del sector 2, y rellena el resto con ceros.
 * Los datos de video se agregaran en el Hito 2.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "adf.h"

#define PLAYER_SECTOR  ADF_BOOT_SECTORS   /* el reproductor arranca en el 2 */
#define RESULT_SECTOR  (ADF_SECTORS - 1)  /* sector de diagnostico del Hito 0 */

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
            "uso: mkadf --boot <bootblock.bin> --player <player.bin> "
            "--out <imagen.adf>\n");
}

int main(int argc, char **argv)
{
    const char *bootpath = NULL, *playerpath = NULL, *outpath = NULL;
    uint8_t *boot = NULL, *player = NULL;
    size_t bootlen = 0, playerlen = 0, playerpadded;
    uint32_t used;
    FILE *out;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--boot") && i + 1 < argc)        bootpath = argv[++i];
        else if (!strcmp(argv[i], "--player") && i + 1 < argc) playerpath = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)    outpath = argv[++i];
        else { usage(); return 2; }
    }
    if (!bootpath || !playerpath || !outpath) { usage(); return 2; }

    boot = slurp(bootpath, &bootlen);
    player = slurp(playerpath, &playerlen);
    if (!boot || !player) return 1;

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

    memset(disk, 0, sizeof disk);
    memcpy(disk, boot, bootlen);
    be_put_u32(disk + BB_OFF_STAGE2LEN, (uint32_t)playerpadded);
    adf_boot_finalize(disk);

    if (!adf_boot_valid(disk)) {
        fprintf(stderr, "checksum de bootblock invalido (error interno)\n");
        return 1;
    }
    if (adf_put(disk, PLAYER_SECTOR, player, playerlen) != 0) {
        fprintf(stderr, "el reproductor no entra en el disco\n");
        return 1;
    }

    out = fopen(outpath, "wb");
    if (!out) { perror(outpath); return 1; }
    if (fwrite(disk, 1, sizeof disk, out) != sizeof disk) {
        perror(outpath); fclose(out); return 1;
    }
    fclose(out);

    used = PLAYER_SECTOR + adf_sectors_for(playerlen);
    printf("%s\n", outpath);
    printf("  bootblock : %lu bytes de %d, checksum %08lX\n",
           (unsigned long)bootlen, ADF_BOOT_SIZE,
           (unsigned long)be_get_u32(disk + BB_OFF_CHECKSUM));
    printf("  player    : %lu bytes -> %lu (sectores %d..%u)\n",
           (unsigned long)playerlen, (unsigned long)playerpadded,
           PLAYER_SECTOR, used - 1);
    printf("  libre     : %u sectores = %lu bytes para datos\n",
           ADF_SECTORS - used,
           (unsigned long)(ADF_SECTORS - used) * ADF_SECTOR_SIZE);
    printf("  sector %d reservado para la medicion del Hito 0\n",
           RESULT_SECTOR);

    free(boot);
    free(player);
    return 0;
}
