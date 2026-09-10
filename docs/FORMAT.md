# Formato del disco de A500VP

**Version de formato: 0 (Hito 0).** Todavia no hay bitstream de video; esto
define solo el disco y el arranque. El formato de paquetes se agrega en el
Hito 2 y sube la version.

Todo es **big-endian**. El 68000 lee el disco con punteros pelados, sin
conversiones.

Este archivo es la unica definicion del formato. El encoder
(`encoder\adf.c`), el decoder de referencia y el reproductor
(`player\*.s`) lo siguen al pie de la letra. Si cambia el formato, cambia la
version.

---

## Imagen de disco

ADF de disquete DD: 80 cilindros x 2 caras x 11 sectores x 512 bytes =
**901 120 bytes**, 1760 sectores. Sin sistema de archivos AmigaDOS: el disco
no es un volumen valido y esta bien.

| Sectores | Contenido |
|---|---|
| 0–1 | Bootblock (1024 bytes) |
| 2 … | Reproductor, en sectores consecutivos |
| … | Datos (desde el Hito 2) |
| 1759 | Sector de diagnostico del Hito 0 (`MEMR`) |

---

## Bootblock (sectores 0–1)

| Offset | Tamano | Contenido |
|---|---|---|
| 0 | 4 | `"DOS"`, 0 |
| 4 | 4 | Checksum |
| 8 | 4 | Longitud del reproductor en bytes, multiplo de 512 |
| 12 | … | Codigo. El Kickstart salta aca |

El offset 8 es, en un disco AmigaDOS, el numero de bloque raiz. Como este
disco no es un volumen DOS, nadie lo mira: lo usamos para pasarle al codigo de
arranque cuanto tiene que leer. Entra en el checksum como cualquier otra
longword.

### Checksum

Suma de las 256 longwords big-endian del bloque, con el campo de checksum en
cero y **acarreo circular** (si la suma se desborda, se le suma 1). El valor
que se guarda en el offset 4 es el complemento a uno (NOT) de esa suma.

La comprobacion que hace el Kickstart es sumar las 256 longwords *incluido* el
checksum y exigir `$FFFFFFFF`. `adf_boot_valid()` implementa exactamente eso y
`mkadf` lo verifica antes de escribir el archivo.

Se valido contra bootblocks reales generados por `xdftool` (los ADF del
proyecto `amiga260k`): los tres dan `$FFFFFFFF`.

### Que hace el codigo de arranque

Entra con `A1` = `IOStdReq` de trackdisk.device (unidad 0, ya abierto) y
`A6` = ExecBase; igual recarga ExecBase de `$4`.

1. Pone el borde en azul (senal de que arranco).
2. `AllocMem(longitud, MEMF_CHIP|MEMF_CLEAR)`. **Chip obligatorio:** en
   Kickstart 1.2/1.3 trackdisk solo lee a Chip RAM.
3. `CMD_READ` de esa longitud desde el offset 1024 (sector 2).
4. `TD_MOTOR` 0 para apagar el motor.
5. Pone el borde en verde y salta al reproductor con
   `A6` = ExecBase, `A1` = el mismo `IOStdReq`, `A0` = base del reproductor.

Si falta memoria o falla la lectura, pone el borde en rojo y se queda quieto.

El reproductor es **codigo independiente de posicion**: se ejecuta donde
AllocMem lo haya dejado.

---

## Sector de diagnostico del Hito 0 (sector 1759, offset 900 608)

Lo escribe el reproductor de prueba (`player\memcheck.s`) con `CMD_WRITE`
seguido de `CMD_UPDATE`, para poder leer la medicion desde el PC.

| Offset | Tamano | Contenido |
|---|---|---|
| 0 | 4 | `"MEMR"` |
| 4 | 4 | Chip libre |
| 8 | 4 | Chip, bloque contiguo mayor |
| 12 | 4 | No-chip (slow) libre |
| 16 | 4 | No-chip, bloque contiguo mayor |
| 20 | 4 | Total libre (chip + no-chip) |
| 24 | 4 | Direccion donde quedo cargado el reproductor |
| 28 | 4 | Direccion del bitplane |
| 32 | 480 | Cero |

Si el disquete esta protegido contra escritura la grabacion falla sin
consecuencias: el codigo de error de trackdisk se muestra en pantalla como
`SAVE ERR` (`$1C` = `TDERR_WriteProt`) y los valores se leen igual del monitor.
