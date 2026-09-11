# Formato del disco de A500VP

**Version de formato: 2.** Define el disco, el arranque y el bitstream de
video. El audio tiene su lugar reservado en cada paquete pero todavia viaja
vacio (Hito 5).

Todo es **big-endian**. El 68000 lee el disco con punteros pelados, sin
conversiones.

Este archivo es la unica definicion del formato. El encoder
(`encoder\encode.c`, `encoder\stream.c`), el decoder de referencia
(`encoder\decode.c`) y el reproductor (`player\*.s`) lo siguen al pie de la
letra. Si cambia el formato, cambia la version.

La version 1 (delta por tramos "saltar N, escribir M" por plano) quedo
obsoleta; por que, con numeros, en `DECISIONS.md`.

---

## Imagen de disco

ADF de disquete DD: 80 cilindros x 2 caras x 11 sectores x 512 bytes =
**901 120 bytes**, 1760 sectores. Sin sistema de archivos AmigaDOS: el disco
no es un volumen valido y esta bien.

| Sectores | Contenido |
|---|---|
| 0–1 | Bootblock (1024 bytes) |
| 2 … | Reproductor, en sectores consecutivos |
| siguiente al reproductor … | Datos: bitstream (cabecera + paquetes) |
| 1719–1758 | Solo en discos de prueba: volcado del framebuffer (Hito 3) |
| 1754–1758 | Solo en discos de medicion: tiempos de decodificacion (Hito 4) |
| 1759 | Solo en discos de prueba: sector de informacion (`MEMR`, `FBDM` o `PLAY`) |

Los datos empiezan en el primer sector libre despues del reproductor. `mkadf`
escribe en la cabecera del reproductor donde quedaron. En los discos de
prueba, `mkadf --reserve-tail 41` garantiza que los datos no pisen la zona
de volcado; en el disco final esos sectores son para datos.

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
`mkadf` lo verifica antes de escribir el archivo. Se valido contra bootblocks
reales generados por `xdftool`.

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
El reproductor es **codigo independiente de posicion**.

---

## Cabecera del reproductor

El reproductor (`player\player.s`) empieza con 16 bytes fijos:

| Offset | Tamano | Contenido |
|---|---|---|
| 0 | 4 | `BRA.W` al codigo |
| 4 | 4 | `"A5PL"` |
| 8 | 4 | Offset en bytes, desde el principio del disco, donde empiezan los datos |
| 12 | 4 | Longitud de los datos en bytes |

`mkadf --data` exige el `"A5PL"` y escribe los dos campos. El reproductor
los lee PC-relativos y carga los datos el mismo, con el `IOStdReq` que le
pasa el bootblock.

---

## Bitstream de video

### Cabecera (32 bytes)

| Offset | Tamano | Contenido |
|---|---|---|
| 0 | 4 | `"A5VP"` |
| 4 | 2 | Version = 2 |
| 6 | 2 | Flags. Bit 0 = hay audio (en v2 siempre 0) |
| 8 | 2 | Ancho logico = 160 |
| 10 | 2 | Alto logico = 128 |
| 12 | 1 | Bitplanes (1..4) |
| 13 | 1 | Colores = 2^bitplanes |
| 14 | 2 | Periodo de Paula del audio (0 = sin audio) |
| 16 | 2 | Primera fila logica activa (`y0`) |
| 18 | 2 | Ultima fila activa + 1 (`y1`) |
| 20 | 4 | Cantidad de paquetes = cantidad de frames |
| 24 | 4 | Bytes de paquetes que siguen a la cabecera |
| 28 | 4 | Reservado, 0 |

Las filas fuera de `y0..y1-1` son las barras del letterbox: nunca cambian y
quedan en el color 0.

### Paquetes

Un paquete por frame, sin excepcion. **Cada paquete dura exactamente 2 VBL**
(1/24,960205 s). No hay paquetes de "repetir K frames": si hay que sostener
una imagen K frames, van K paquetes de repeticion. Asi el audio de cada frame
viaja en su propio paquete y la sincronia es trivial.

| Offset | Tamano | Contenido |
|---|---|---|
| 0 | 2 | Longitud total del paquete en bytes, incluido este campo. **Siempre par** |
| 2 | 1 | Operacion: 0 = DELTA, 1 = REPETICION |
| 3 | 1 | Flags. Bit 0 = trae paleta |
| 4 | 2 | Bytes de audio en este paquete |
| 6 | … | Paleta: `colores` x 2 bytes, RGB444 como `$0RGB` (si el bit 0 esta prendido) |
| … | … | Audio (los bytes indicados en el offset 4) |
| … | … | Delta (solo si la operacion es DELTA) |
| … | 0–1 | Relleno con cero hasta longitud par |

El audio va **antes** del video para que el reproductor lo encuentre en un
offset que se calcula sin recorrer el delta.

Como todos los paquetes tienen longitud par y prefijo de longitud, el cargador
puede recorrerlos y partir los datos en dos bloques de memoria en un limite de
paquete, sin mirar su contenido.

Un cambio de paleta siempre viaja con un DELTA: una REPETICION nunca trae
paleta.

### Delta

| Tamano | Contenido |
|---|---|
| 16 | Mapa de filas: 128 bits, uno por fila logica |
| … | Por cada fila marcada, en orden ascendente: el bloque de fila |

Mapa de filas: el byte `i` cubre las filas `8i..8i+7`; el bit 7 es la fila
`8i`.

Bloque de fila:

| Tamano | Contenido |
|---|---|
| 3 | Mascara de columnas: 20 bits, uno por byte logico de la fila |
| … | Por cada columna marcada, en orden ascendente: `bitplanes` bytes, plano 0 primero |

Mascara de columnas: el bit 7 del primer byte es la columna 0 (pixeles
logicos 0..7), el bit 0 del segundo byte es la columna 15, y en el tercer
byte los bits 7..4 son las columnas 16..19. **Los bits 3..0 del tercer byte
tienen que valer 0**; el decoder de referencia rechaza el delta si no.

Cada byte de plano son 8 pixeles logicos, el bit 7 es el de mas a la
izquierda (como lo lee Denise).

Una columna marcada lleva **todos** los planos, aunque alguno no haya
cambiado: medido, en una fila modificada cambian en promedio 2,84 de 3
planos, y escribir el plano que sobra sale mas barato que describir cuales
cambiaron.

### Semantica (lo que hace el reproductor)

- Hay dos framebuffers planares, A y B, cada uno con su copper list. Al
  empezar los dos estan en cero (negro) y se ve A.
- **DELTA:** se escriben los bytes en el buffer **oculto**. Cada byte logico
  se expande a una palabra con la tabla de doblado de 256 entradas (cada
  pixel logico ocupa 2 pixeles de pantalla). Si el paquete trae paleta, se
  escribe en el copper list del buffer oculto. En el vertical blank se
  intercambian los buffers escribiendo `COP1LC`. Despues del intercambio la
  paleta se copia tambien al copper list del otro buffer, para que los dos la
  tengan.
- **REPETICION:** no se toca nada y no se intercambia: se sigue viendo el
  mismo buffer.
- El delta de un frame es contra el contenido del buffer oculto, que es el
  penultimo frame **distinto** mostrado (las repeticiones no intercambian).
- Cada fila logica se ve dos veces (doblado vertical por Copper; Hito 3).

### Verificacion

El encoder escribe junto al bitstream un archivo `<salida>.crc`: un CRC-32
big-endian por frame, calculado sobre el mapa de indices visible
(160 x 128 bytes, un indice de paleta por pixel logico) seguido de la paleta
visible serializada en big-endian. El decoder de referencia recalcula lo
mismo a partir del bitstream y tiene que coincidir en **todos** los frames.

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

---

## Volcado del Hito 3 (sectores 1719–1759)

Lo escribe el reproductor despues de decodificar el primer paquete y antes
de tomar el hardware, con `CMD_WRITE` + `CMD_UPDATE`.

**Sectores 1719 en adelante:** el framebuffer tal cual esta en Chip RAM:
`bitplanes` x 5120 bytes (40 bytes por fila de pantalla x 128 filas), plano
0 primero. Son los bytes logicos ya doblados por la tabla. Con 3 planos
ocupa los sectores 1719–1748.

**Sector 1759:**

| Offset | Tamano | Contenido |
|---|---|---|
| 0 | 4 | `"FBDM"` |
| 4 | 4 | Bitplanes |
| 8 | 4 | Direccion del framebuffer |
| 12 | 4 | Direccion del copper list |
| 16 | 4 | Bytes de delta que consumio el decodificador |
| 20 | 4 | Error de trackdisk al volcar el framebuffer (0 = bien) |
| 24 | 32 | Paleta leida del paquete: 16 palabras `$0RGB` |
| 56 | 456 | Cero |

`a500vp-dec --still K --still-out <f>` genera lo que tiene que dar:
`<f>` (el frame K como un solo DELTA desde negro con su paleta), `<f>.fb`
(el framebuffer esperado, mismo formato que el volcado) y `<f>.ppm` (la
imagen de referencia). `build.ps1 still` compara el volcado contra `<f>.fb`
byte a byte.

---

## Mediciones del Hito 4 (sectores 1754–1759)

Las graba el reproductor de medicion (`player.s` ensamblado con
`-DBENCH=1`) al terminar la reproduccion, despues de devolverle la maquina
al sistema.

**Sectores 1754–1758:** 640 longwords big-endian, una por paquete: los
color clocks (3 546 895 por segundo; la CPU hace 2 ciclos por cada uno) que
tardo en decodificarse ese DELTA, medidos con el haz de video (VBL, linea y
posicion horizontal). Cero para las repeticiones y para los paquetes
despues del 640.

**Sector 1759:**

| Offset | Tamano | Contenido |
|---|---|---|
| 0 | 4 | `"PLAY"` |
| 4 | 4 | Paquetes del bitstream |
| 8 | 4 | Duracion de la carga, en VSYNC (TOD del CIA-A) |
| 12 | 4 | Bytes cargados (cabecera + paquetes) |
| 16 | 4 | Direccion del bloque 1 (slow RAM) |
| 20 | 4 | Bytes de paquetes que quedaron en el bloque 1 |
| 24 | 4 | Direccion del bloque 2 (Chip) |
| 28 | 4 | Bytes de paquetes que quedaron en el bloque 2 |
| 32 | 4 | Frames que se mostraron tarde |
| 36 | 4 | Mayor atraso, en VBL |
| 40 | 4 | VBL que duro la reproduccion |
| 44 | 4 | VBL que tendria que haber durado (2 por paquete) |
| 48 | 8 | Direcciones de los dos framebuffers |
| 56 | 4 | Peor decodificacion, en color clocks |
| 60 | 4 | Paquete de la peor decodificacion |
| 64 | 4 | Deltas medidos |
| 68 | 4 | Entradas de la tabla de tiempos (640) |
| 72 | 4 | Error de trackdisk al grabar la tabla (0 = bien) |
| 76 | 436 | Cero |

`a500vp-dec --in <bitstream> --measure <disco.adf>` los cruza con las
estadisticas de cada delta y ajusta el modelo de costo.
