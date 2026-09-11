/* mkadf.c - arma un .adf de A500VP a partir de piezas ya construidas.
 *
 *   mkadf --boot boot.bin --player player.bin [--data video.a5v]
 *         [--reserve-tail N] --out imagen.adf
 *
 * Lo usan los discos de prueba. El disco final lo arma el encoder con
 * --adf, con la misma funcion (adf_assemble). Ver docs/FORMAT.md.
 *
 * --reserve-tail N deja libres los ultimos N sectores, donde los
 * reproductores de prueba vuelcan lo que hay que verificar desde el PC.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "adf.h"

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
    const char *outpath = NULL, *err;
    uint8_t *boot = NULL, *player = NULL, *data = NULL;
    size_t bootlen = 0, playerlen = 0, datalen = 0;
    int reserve = 0, i;
    AdfLayout lay;
    FILE *out;

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

    boot = slurp(bootpath, &bootlen);
    player = slurp(playerpath, &playerlen);
    if (!boot || !player) return 1;
    if (datapath) {
        data = slurp(datapath, &datalen);
        if (!data) return 1;
    }

    err = adf_assemble(disk, boot, bootlen, player, playerlen, data, datalen,
                       reserve, &lay);
    if (err) {
        fprintf(stderr, "mkadf: %s\n", err);
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
    printf("  player    : %lu bytes (sectores %d..%u)\n",
           (unsigned long)playerlen, ADF_PLAYER_SECTOR,
           ADF_PLAYER_SECTOR + lay.player_sectors - 1);
    if (data)
        printf("  datos     : %lu bytes (sectores %u..%u, offset %u)\n",
               (unsigned long)datalen, lay.data_sector, lay.used - 1,
               lay.data_sector * ADF_SECTOR_SIZE);
    printf("  libre     : %u sectores = %lu bytes\n", lay.limit - lay.used,
           (unsigned long)(lay.limit - lay.used) * ADF_SECTOR_SIZE);
    if (reserve)
        printf("  reservados: los ultimos %d sectores (%u..%d), para "
               "diagnostico\n", reserve, lay.limit, ADF_SECTORS - 1);

    free(boot);
    free(player);
    free(data);
    return 0;
}
