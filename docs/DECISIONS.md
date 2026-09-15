# Decisiones de diseno

Una entrada por decision no obvia, con fecha, metodo y resultado. Las
mediciones reemplazan a las estimaciones de `CLAUDE.md`; cuando algo
contradice ese documento, se dice aca.

---

## 2026-09-10 — Toolchain: se reusa el vbcc que ya estaba

**Decision:** usar `C:\Users\JC\vbcc\bin\vasmm68k_mot.exe` (vasm 1.9) y el gcc
de MSYS2/UCRT64, ambos ya instalados en la maquina para el proyecto
`amiga260k`.

**Motivo:** no tiene sentido tener dos ensambladores. Se llego a compilar vasm
2.0f desde el fuente antes de encontrar el que ya estaba; se comprobo que
ambos producen bytes identicos para `player\boot.s` y se descarto el propio.
`tools\get-vasm.ps1` queda como plan B documentado.

---

## 2026-09-10 — Kickstart 1.2, no 1.3

**Decision:** el objetivo de desarrollo pasa a ser Kickstart **1.2** (33.180).

**Motivo:** es la ROM de la maquina real de Az, que es la que manda. `CLAUDE.md`
pedia 1.3; 1.2 es mas viejo, asi que lo que funcione en 1.2 funciona en 1.3.

**Consecuencia concreta:** `AvailMem(MEMF_TOTAL)` es **V36 (Kickstart 2.0)** y
no existe en 1.2/1.3. La "memoria total" que pedia el Hito 0 se mide entonces
como *libre* (suma de bloques) y *bloque contiguo mas grande*
(`MEMF_LARGEST`), que ademas es lo que de verdad importa para presupuestar.

---

## 2026-09-10 — Memoria libre en la maquina objetivo (Hito 0)

**Metodo:** el bootblock carga un stage2 de 1548 bytes desde el sector 2 y
salta a el. Lo primero que hace stage2, antes de reservar nada mas, son cuatro
llamadas a `AvailMem`. WinUAE configurado como A500 PAL OCS, 68000
cycle-exact, KS 1.2, 512 KB chip + 512 KB slow, cero fast.

En ese momento ya estan reservados el buffer del bootblock y los 2048 bytes de
stage2, que es lo correcto: el reproductor tambien va a estar residente cuando
cargue los datos.

| | bytes | KB | usado por el sistema |
|---|---|---|---|
| Chip libre | 499 224 | 487,5 | 25 064 |
| Chip, bloque contiguo mayor | 499 224 | 487,5 | (sin fragmentar) |
| Slow libre (`MEMF_FAST`) | 499 200 | 487,5 | 25 088 |
| Slow, bloque contiguo mayor | 499 176 | 487,5 | |
| **Total libre** | **998 424** | **975,0** | |

stage2 quedo en `$000059E8` y el bitplane en `$000061E8`.

**Conclusion:** hay bastante mas memoria de la que estimaba `CLAUDE.md`
(~850–900 KB). El presupuesto real **no lo limita la RAM sino el disquete**:

- disco: 901 120 − 1024 (bootblock) − reproductor ≈ **898 KB**
- RAM: 975 KB

Con lo cual `--budget` se calcula desde el disco. Ademas, como ningun bloque
contiguo pasa de 487,5 KB, los datos se van a repartir igual en dos bloques
(uno en slow, uno en chip), tal como preveia el diseno.

---

## 2026-09-10 — Frecuencia exacta del VBL PAL

**Decision:** VBL = **49,920409 Hz**, video a **24,960205 fps** (1 frame cada
2 VBL).

**Metodo:** el reloj de color PAL de la Amiga es 3 546 895 Hz exactos
(portadora PAL 4 433 618,75 Hz × 4/5). Un frame PAL **no entrelazado** tiene
**313 lineas** de 227 color clocks:

```
3 546 895 / (313 × 227) = 3 546 895 / 71 051 = 49,920409 Hz
```

WinUAE confirma las dos cosas en su barra de estado durante la prueba del
Hito 0: muestra `313p` y `FPS: 49.9`.

Es decir: no son 50 Hz, y tampoco 49,92 redondos. Un opening de 30 s son
748,8 frames de video. Este valor va parametrizado en el encoder, no escrito a
mano en varios lados.

Para el audio, con `periodo = 443` la frecuencia real de Paula es
3 546 895 / 443 = **8006,535 Hz**, que es la que tiene que usar el encoder para
la acumulacion fraccional de muestras por frame.

---

## 2026-09-10 — El reproductor graba la medicion en el disquete

**Decision:** ademas de mostrarla en pantalla, stage2 escribe la medicion en el
**ultimo sector del disco (1759)** con `CMD_WRITE` + `CMD_UPDATE`.

**Motivo:** que el resultado del Hito 0 se pueda leer desde el PC en vez de
depender de una foto de la pantalla. `build.ps1 check` arranca WinUAE, lo
cierra y lee ese sector del `.adf`. Es una medicion automatizada y repetible,
no un "se ve bien".

**Trampa encontrada:** WinUAE mantiene la pista escrita en memoria. Si se lo
mata con `Kill()` el `.adf` queda sin cambios y parece que la Amiga no grabo
nada. Hay que cerrarlo con `CloseMainWindow()`. En la Amiga hace falta
`CMD_UPDATE` despues del `CMD_WRITE` por el mismo motivo: trackdisk tambien
bufferea la pista.

Este sector es solo de diagnostico del Hito 0. Cuando entren los datos de
video (Hito 2) hay que decidir si se conserva o se libera; son 512 bytes.

---

## 2026-09-10 — Hito 1: la fuente no necesita telecine inverso

**Metodo:** `ffprobe` sobre *Fullmetal Alchemist Brotherhood OP 1_2.mp4*.

1280x720, SAR 1:1, DAR 16:9, `field_order=progressive`, **24000/1001 =
23,976024 fps** reales, 92,69 s, audio AAC 44100 Hz estereo.

**Decision:** se toma 1:1, sin `fieldmatch,decimate`. Es una fuente
progresiva de 23,976, no un telecine de 29,97.

La aceleracion PAL queda en **+4,10 %**: 24,960205 / 23,976024 = 1,041044.
Los primeros 30 s de fuente (720 frames) duran **28,846 s** en la Amiga.

---

## 2026-09-10 — El letterbox son 96 filas, no 90

**Decision:** para una fuente 16:9 el area activa es **160x96**, con 32 filas
de barra negra (16 arriba, 16 abajo).

**Motivo:** `CLAUDE.md` estimaba "unas 90 filas". El numero exacto sale de la
relacion de aspecto del pixel. La pantalla de 320x256 se ve en un tubo 4:3, o
sea que un pixel de pantalla (y por lo tanto un pixel logico, que son 2x2) es
1,0667 veces mas ancho que alto: la SAR 16:15 de PAL lowres.

```
filas = 160 x 1,0667 / (16/9) = 96
```

Comprobacion: 96 filas logicas son 192 lineas de pantalla; 192/256 x 3 = 2,25;
4 / 2,25 = 1,7778 = 16:9 exacto.

El calculo se hace en el encoder y no con `force_original_aspect_ratio` de
ffmpeg, justamente porque ffmpeg supondria pixeles cuadrados.

---

## 2026-09-10 — Oklab en vez de CIE Lab

**Decision:** el espacio perceptual del encoder es **Oklab**.

**Motivo:** `CLAUDE.md` pedia "Lab o similar". Oklab no necesita elegir blanco
de referencia, es mas barato de calcular y se porta bastante mejor con azules
saturados, que en un opening de anime sobran. Se usa para el k-means, para
elegir el color de paleta mas cercano y para medir el error.

---

## 2026-09-10 — Negro reservado y deduplicacion de paleta

**Decision:** con letterbox, el indice 0 de la paleta queda fijo en `$000` y
el k-means corre con n-1 centroides.

**Motivo:** las barras ocupan el 25 % de la pantalla y tienen que ser negro
exacto; el color 0 es ademas el fondo del Copper. Si se dejara al k-means,
saldria un gris oscuro y las barras se verian sucias.

**Correccion que hizo falta:** al redondear los centroides a RGB444, dos
centroides distintos pueden caer en el mismo color. Con el negro reservado
pasaba seguido: la escena del fuego terminaba con `000 000 ...`, o sea 7
colores utiles en vez de 8. Ahora, si un color de la paleta queda repetido, se
lo reemplaza por el color de la escena peor representado. El error medio bajo
de 0,0414 a 0,0405.

---

## 2026-09-10 — Deteccion de cortes por distancia Oklab media

**Decision:** hay corte de escena cuando la distancia Oklab media entre un
frame y el anterior (solo sobre el area activa) supera 0,12, con un minimo de
6 frames por escena.

**Motivo:** se mide sobre el frame ya escalado a 160x96, que es exactamente lo
que se va a cuantizar. Medirlo sobre la fuente en HD detectaria cosas que a
160 px de ancho no existen.

**Resultado en los primeros 30 s:** 15 escenas, error medio de cuantizacion
0,0405 en Oklab. Y un dato que importa para el Hito 2: **el 45,6 % de los
frames son casi identicos al anterior** (distancia < 0,008). Es lo que va a
comprimir el comando de repeticion; el anime animado "en dos" se nota.

---

## 2026-09-10 — Realce despues de escalar, y lo que cuesta

**Decision:** opcion `--sharpen F` (apagada por defecto) que mete un
`unsharp=3:3:F:3:3:0` **despues** de escalar.

**Motivo:** el anime vive de la linea negra de contorno. A 160 px de ancho,
partiendo de 1280, esa linea es sub-pixel y el escalado la promedia hasta
hacerla desaparecer: las caras quedan planas. Realzar despues de escalar la
devuelve. Antes de escalar no serviria de nada.

**El costo, medido:** con `--sharpen 1.2` los frames casi identicos bajan de
45,6 % a 40,5 %. El realce agrega detalle de alta frecuencia, y ese detalle
cambia entre frames. **Imagen mas nitida = mas bytes de delta.** Hay que
volver a medirlo en el Hito 2 contra el presupuesto real.

---

## 2026-09-10 — Cadencia nativa en vez de aceleracion PAL

**Esto contradice a `CLAUDE.md`, a pedido de Az.** El documento decia: "los
frames de la fuente se toman 1:1 [...] el audio se remuestrea para durar
exactamente lo mismo (con el cambio de tono, como en la TV PAL)". Az escucho
el preview y el audio acelerado le molesto.

**Decision:** por defecto `--rate native`. Se repite un frame cada tanto para
que la velocidad sea la de la fuente y **el audio no se toca**. `--rate pal`
mantiene el comportamiento viejo.

**Como funciona:** la Amiga solo puede cambiar de frame cada 2 VBL, o sea
24,960205 fps clavados. En vez de estirar el tiempo, se mapea cada hueco de
pantalla al frame de fuente que le toca:

```
frame_fuente(hueco) = redondeo(hueco * 23,976024 / 24,960205)
```

Cuando el indice no avanza, sale un comando de repeticion.

**Lo que cuesta, medido:** los primeros 30 s pasan de 720 a **750 huecos, de
los cuales 30 son repeticiones** (4,1 %). En bytes de video eso es casi nada,
porque el comando de repeticion ya existia para el anime animado en dos: los
frames casi identicos suben de 40,5 % a 42,9 %. Lo unico que crece de verdad
es el audio, que ahora dura 30,03 s en vez de 28,85 s: a ~4 KB/s son unos
**4,7 KB mas**. Es una ganga.

**Residuo:** 750 huecos son 30,048 s contra 30,030 s de fuente, 18 ms de
diferencia en 30 s (0,06 %). El audio se estira eso, que es una milesima de
semitono: inaudible.

**Preview:** va a 24,960205 fps con el audio original en mono, que es lo que
va a salir por Paula. En `--rate pal` el audio se acelera con
`asetrate` + `aresample` y no con `atempo`, porque la TV PAL sube la velocidad
**y** el tono, y el preview tiene que sonar como va a sonar.

---

## 2026-09-11 — Hito 2: bitstream v2, mascara de columnas en vez de tramos

**Decision:** el delta describe cada fila modificada con una **mascara de 20
bits** (una por byte logico) y, por cada columna marcada, manda **todos** los
planos. Reemplaza al formato v1 que proponia `CLAUDE.md` como punto de
partida: por fila y por plano, tramos "saltar N, escribir M".

**Metodo:** se codifico el opening (30 s, 8 colores, ajustado al presupuesto)
con v1 y el decoder de referencia desgloso a donde iban los bytes:

| v1 | bytes | % |
|---|---|---|
| cabeceras de tramo (2 bytes cada una) | 333 636 | 37,9 |
| cuenta de tramos por fila-plano | 78 914 | 9,0 |
| mascara de planos por fila | 27 762 | 3,2 |
| datos literales | 428 972 | 48,7 |

Los tramos promediaban **2,57 bytes**: se pagaban 2 bytes de cabecera por
cada 2,57 de dato. Y en una fila modificada cambiaban **2,84 de 3 planos**,
casi siempre en las mismas columnas. Describir por separado que cambio en
cada plano era tirar bytes.

**Resultado, v2 con la misma fuente y el mismo presupuesto (883 712 bytes):**

| | v1 | v2 |
|---|---|---|
| 8 colores, error final (Oklab) | 0,0708 | **0,0471** |
| 16 colores, error final | 0,1067 | **0,0763** |
| Mascara de columnas / datos | — | 11,7 % / 86,8 % |

Con 8 colores v2 ademas deja 84 KB sin usar (ver el punto sobre el control
de tasa, mas abajo). El decoder de referencia reconstruye los 750 frames de
las dos variantes bit a bit igual que el encoder.

**Costo de decodificacion:** el lazo del 68000 pasa a recorrer 20 bits por
fila en vez de interpretar tramos. Con el modelo (sin calibrar) el peor frame
queda en 39,8 ms con 8 colores y 49,5 ms con 16.

---

## 2026-09-11 — La repeticion usa el mismo criterio que la calidad

**Decision:** un frame se repite si, aplicando el filtro de calidad contra el
buffer **visible** (con un umbral 1,5 veces mas permisivo, `--repeat-boost`),
no queda ningun pixel que valga la pena actualizar.

**Motivo:** al principio la repeticion tenia un umbral propio y fijo (error
medio < 0,010). Al subir la perdida para entrar en el presupuesto, el buffer
visible se alejaba cada vez mas del frame ideal y las repeticiones caian de
~50 % a **3,3 %**, justo cuando mas falta hacian. Con el criterio unificado,
ajustado al presupuesto, 8 colores repite el 45 % de los frames y 16 colores
el 56 %.

---

## 2026-09-11 — Bug en la regla que descarta filas enteras

La calidad con perdida descartaba una fila si la suma de errores que quedaban
**dividida por 160** era chica. Eso hacia parecer despreciable un cambio
grande concentrado en tres pixeles (3 x 1,0 / 160 = 0,019) y se tiraban filas
con detalle real. Ahora se compara la suma sin dividir. Los streams salen mas
grandes a igual umbral, pero porque ya no se pierde lo que no se debia perder.

---

## 2026-09-11 — Histeresis temporal en el cuantizador

**Decision:** al cuantizar un frame, si el indice que tenia el pixel en el
frame anterior esta a menos de `--stability` (Oklab) del optimo, se lo deja.
Default **0,10**.

**Motivo:** la fuente es un h264 de 2 Mbit/s. Su ruido de compresion no se
ve a 160 px de ancho, pero hace que en zonas quietas los pixeles salten entre
dos colores vecinos de la paleta, y cada salto se paga en bytes de delta. Con
16 colores es peor, porque los colores de la paleta estan mas cerca entre si.

**Medido** (16 colores, v1, casi sin perdida): 2,05 MB sin histeresis,
1,91 MB con 0,03, 1,84 MB con 0,06, **1,41 MB con 0,10**. El error final
paso de 0,0844 a 0,0921: un 31 % menos de bytes por casi nada.

---

## 2026-09-11 — El denoise temporal empeora las cosas

Se probo `hqdn3d` antes de escalar (`--denoise`), para limpiar el ruido de
la fuente. **Los streams salieron mas grandes**: de 1,49 a 1,56 MB con 8
colores y de 2,33 a 2,39 MB con 16. El filtrado temporal convierte el ruido
en derivas lentas que igual cruzan los limites entre colores de la paleta, y
la histeresis ya hacia ese trabajo mejor. La opcion queda, apagada por
defecto.

---

## 2026-09-11 — 16 colores contra 8 con el presupuesto del disquete

Az eligio 16 colores en el Hito 1 mirando el preview **sin comprimir**, donde
16 se ve claramente mejor (error de cuantizacion 0,0318 contra 0,0446). Con
el bitstream real y el disquete como limite, la cuenta cambia:

| 30 s, 883 712 bytes | 8 colores | 16 colores |
|---|---|---|
| Tamano casi sin perdida | 1,63 MB | 2,31 MB |
| Perdida necesaria para entrar | umbral 0,207 | umbral 0,271 |
| Frames repetidos | 45 % | 56 % |
| Error final contra su ideal | **0,0471** | 0,0763 |
| Peor frame, modelo sin calibrar | 39,8 ms | 49,5 ms |

Un 4.o bitplane son 33 % mas bytes por columna, y con un solo disquete eso se
paga en perdida y en frames sostenidos (movimiento mas entrecortado).

**Pero ninguna de las dos variantes es aceptable asi.** Ver la entrada
siguiente: los errores de esta tabla son medias, y la media escondia un
salpicado muy visible.

---

## 2026-09-11 — La perdida por pixel produce salpicado: no sirve a este nivel

**Hallazgo:** mirando los previews decodificados (no las estadisticas), los
streams ajustados al disquete tienen **pixeles viejos desparramados** por
toda la imagen: restos de la escena anterior sobre la cara de un personaje,
estela en el fuego. El control, el mismo opening casi sin perdida (1,63 MB),
sale limpio: el formato y el decoder estan bien, y el decoder verifica bit a
bit contra el encoder. El problema es la estrategia de perdida.

**Por que:** para meter 1,63 MB en ~880 KB, el control de tasa subio el
umbral por pixel hasta 0,207 en Oklab. Eso quiere decir "no actualices
ningun pixel cuyo color este a menos de 0,2 del correcto", y 0,2 es
muchisimo. Cada pixel decide solo, sin mirar a sus vecinos, y el resultado
es ruido sal-y-pimienta que ademas persiste frame tras frame.

**Leccion de metodo:** el error *medio* (0,047) parecia razonable y no lo
era. El encoder ahora informa tambien el **porcentaje de pixeles activos con
error visible (> 0,1)**, que es lo que el ojo ve. Ningun parametro se da por
bueno sin mirar el preview decodificado.

**Consecuencia:** hay que reducir ~1,9x de otra manera. Opciones a medir y
decidir con Az: tope de cadencia (`--min-hold`, el anime esta animado en dos),
perdida coherente por regiones en vez de por pixel, clip mas corto.

---

## 2026-09-11 — Tope de cadencia (`--min-hold`): medido

**Que es:** la imagen se actualiza como mucho cada N huecos de pantalla
(N = 2 son hasta 12,5 fps de imagenes distintas; N = 3, hasta 8,3). Los
cortes de escena pasan siempre. El anime esta animado mayormente "en dos",
asi que buena parte de esos frames ya eran repetidos.

**Medido** (8 colores, primeros 30 s, realce 1,2):

| `--min-hold` | casi sin perdida | ajustado a 883 712: pixeles con error visible |
|---|---|---|
| 1 | 1 633 634 bytes | 27,2 % |
| 2 | 1 087 810 bytes | 18,1 % |
| 3 | **833 820 bytes** | 6,0 % |

Con N = 3 el opening entra en 883 KB **sin perdida**. Su 6 % de "error
visible" no es salpicado: es el retraso de sostener una imagen hasta 3
huecos en las zonas que se mueven, o sea movimiento mas entrecortado. La
metrica no distingue esos dos casos; el ojo si.

**Correccion de presupuesto:** esos 883 KB eran solo video. El audio del
Hito 5 (fib4, 4 bits, periodo 443 = 8006,5 Hz) son **4003 bytes/s**:
~120 KB en 30 s. El presupuesto real de video para 30 s es ~763 KB, y ahi
ya ni N = 3 entra sin perdida.

---

## 2026-09-11 — Decision de Az: 22 s a 12,5 fps, 8 colores, sin perdida

**Lo que no entra:** 30 s a 25 fps, casi sin perdida, son 1,63 MB. El disco
deja ~763 KB de video una vez descontado el audio. Achicar 2x con perdida
por pixel produce salpicado inaceptable (ver arriba).

**Opciones que se le presentaron a Az, todas medidas y miradas:**

| | cadencia | perdida | pixeles con error visible |
|---|---|---|---|
| 30 s | hasta 8,3 fps | umbral 0,124 | 13,2 %, salpicado visible |
| 20 s | hasta 12,5 fps | ninguna | 2,3 %, limpio |
| 24 s | hasta 12,5 fps | umbral 0,109 | 3,8 % |

Mas "invertir en el encoder" (resultado incierto) y "dos disquetes" (fuera
del alcance de la V1 segun `CLAUDE.md`).

**Decision:** Az eligio **~22 s a 12,5 fps**. Con eso:

- **8 colores, no 16.** Revierte lo que Az eligio en el Hito 1 mirando el
  preview sin comprimir. Con un solo disquete no hay caso: hasta el caso mas
  holgado ocuparia ~1 MB con 16 colores contra ~800 KB disponibles.
- `--min-hold 2`: la imagen cambia como mucho cada 2 huecos. El anime esta
  animado en dos, asi que se nota solo en los paneos.
- `--stability 0.07`: ver la entrada siguiente.
- Primeros 22 s del opening: 528 frames de fuente, 550 huecos, 22,035 s.

**Resultado final del Hito 2:** 774 048 bytes de video contra 795 499 de
presupuesto (sobran 21 KB), **sin ninguna perdida** (umbral base 0,012), 54 %
de repeticiones, 11 cambios de paleta, 1407 bytes por frame de media y 6086
el peor. Peor frame segun el modelo sin calibrar: 39,8 ms. El decoder de
referencia verifica los 550 frames.

---

## 2026-09-11 — Histeresis 0,07: el punto donde el zoom no se raya

La histeresis del cuantizador (`--stability`) es la que mas bytes ahorra,
pero en un zoom o un fundido "pega" los colores viejos. Con 0,10 el primer
plano de los anteojos (12,5 s) sale con manchas blancas pegadas en la cara.

Medido y mirado (20 s, `--min-hold 2`):

| `--stability` | casi sin perdida | zoom de los anteojos |
|---|---|---|
| 0 | 908 KB | limpio |
| 0,05 | 791 KB | limpio |
| 0,07 | (22 s: 774 KB) | limpio |
| 0,10 | 634 KB | **manchas pegadas** |

0,07 es lo mas alto que se probo sin el defecto, y es lo que permite los 22 s
sin perdida. Default del encoder.

---

## 2026-09-11 — El presupuesto por defecto ya descuenta el audio

Olvidar el audio al presupuestar paso una vez. Ahora, si no se pasa
`--budget`, el encoder calcula el video disponible como el disco menos el
audio que se va a llevar la duracion real en pantalla, a fib4 (4 bits por
muestra) y 3546895/`--audio-period` Hz. Para 22 s: 883 712 − 88 213 =
**795 499 bytes**.

*Reemplazado en el Hito 5:* el audio viaja dentro de los paquetes y el
presupuesto es uno solo (ver "Presupuesto unico" mas abajo).

---

## 2026-09-11 — El control de tasa deja presupuesto sin usar

La busqueda binaria sobre el umbral de calidad supone que mas umbral siempre
da menos bytes, y en promedio es asi, pero no de forma suave: una repeticion
de mas cambia el contenido del buffer oculto para todos los frames que
siguen, y el tamano salta. Con 8 colores la busqueda termino en 799 KB contra
un tope de 883 KB: **84 KB (10 %) sin usar** que podrian ir a calidad. Hay
que mejorarlo (por ejemplo, un ajuste fino por escena despues de la busqueda
global).

---

## 2026-09-11 — Hito 3: doblado vertical por modulo, no por punteros

**Decision:** el Copper duplica cada fila logica alternando `BPL1MOD` y
`BPL2MOD` entre −40 y 0 en cada linea de pantalla. `CLAUDE.md` proponia
reescribir los punteros de bitplane cada dos lineas y marcaba como duda el
momento exacto del WAIT horizontal, con un framebuffer de 256 filas como
alternativa si no salia estable.

**Motivo:** el modulo se suma a los punteros al terminar el fetch de la
linea, cerca de hpos $D8. Con −40, la linea siguiente vuelve a leer la misma
fila; con 0 avanza. Escribirlo al principio de la linea (WAIT a hpos $06)
deja **casi una linea entera de margen**: la escritura solo tiene que caer
entre el fin del fetch de la linea anterior y el fin del de esta, ~200 color
clocks. Reescribir punteros exige caer en el blanking horizontal, entre
DDFSTOP y DDFSTRT, y son 6 escrituras por par de lineas con 3 planos (12 con
4) contra 2 por linea. No hizo falta el alternativo de 256 filas.

**Costo:** 256 lineas x (WAIT + 2 MOVE) = 3 KB de copper list por
framebuffer, en Chip.

**Verificado en WinUAE (A500, 68000 cycle-exact, KS 1.2):** el disco de
prueba (`build.ps1 still`) carga el frame 182 de `final22.a5v` como un DELTA
desde negro y lo muestra fijo. El framebuffer que dejo el decodificador en
ensamblador es **identico byte a byte** al del decoder de referencia (15 360
bytes), la paleta coincide y consumio exactamente los 6064 bytes del delta.
En la captura la imagen sale entera, en bloques de 2x2, con la proporcion y
los colores de la referencia. Queda confirmar en la A500 real (Hito 7).

---

## 2026-09-11 — Capturas: PrintWindow, nunca copiar la pantalla

El primer intento de captura copiaba los pixeles de la pantalla en el
rectangulo de la ventana de WinUAE, despues de pedirle a Windows que la
trajera al frente. Windows no deja que un proceso de fondo robe el foco, asi
que se capturo **otra aplicacion que estaba encima**. Esa captura se borro.

Ahora `build.ps1` y `tools\shot.ps1` usan `PrintWindow` con
`PW_RENDERFULLCONTENT`: la ventana de WinUAE se dibuja sola en un bitmap. No
depende de que haya encima y no puede capturar nada que no sea el emulador.

---

## 2026-09-11 — Bug: leer IO_ERROR a traves de A1 despues de DoIO

`memcheck.s` (Hito 0) leia `IO_ERROR(a1)` despues de `DoIO`. Por la
convencion de exec, `a0/a1/d0/d1` no se preservan en una llamada: funcionaba
por casualidad. Ahora se lee a traves de `a5`, que guarda el `IOStdReq` todo
el programa. Salio al escribir el volcado del Hito 3, que hace lo mismo.

---

## 2026-09-11 — Hito 4: carga y reproduccion medidas

**Metodo:** disco de medicion (`build.ps1 play`). El reproductor ensamblado
con `-DBENCH=1` carga `final22.a5v` (22 s, 774 048 bytes), lo reproduce
entero, le devuelve la maquina al sistema y graba en el disquete lo que
midio (formato en `FORMAT.md`). La carga se cronometra con el TOD del CIA-A
(cuenta VSYNC y solo se lee); cada decodificacion, con el haz de video: VBL
contados por la interrupcion propia, linea y posicion horizontal, con
resolucion de un color clock. WinUAE A500, 68000 cycle-exact, KS 1.2,
512 KB chip + 512 KB slow, disquetera al 100 %.

| | medido |
|---|---|
| Carga | 774 048 bytes en 42,1 s = **17,9 KB/s** (2103 VSYNC) |
| Bloque 1 (slow RAM, `$C04A18`) | 497 318 bytes de paquetes |
| Bloque 2 (Chip, `$0135E8`) | 276 698 bytes de paquetes |
| Reproduccion | **1100 VBL para 1100 esperados**: sin deriva |
| Frames mostrados tarde | 1, por 2 VBL |
| Decodificacion media | 34,9 ms por delta |
| Peor decodificacion | **60,3 ms** (frame 541: 96 filas, 1920 columnas, pantalla entera) |

**Carga:** 17,9 KB/s, dentro de lo que estimaba `CLAUDE.md` (15–25 KB/s).
Con el audio del Hito 5 (88 KB mas) la espera antes del video va a ser de
~47 s. Hay margen barato: hoy la CPU copia cada byte del rebote al bloque
(~2,4 s en total) sin que el disco lea mientras tanto; con lecturas
asincronicas se solaparia. El trackloader propio que `CLAUDE.md` deja como
opcional es la mejora grande.

**El modelo de costo estaba mal por 1,6x.** El contado a mano predecia en
promedio el 61 % de lo medido. Ajuste por minimos cuadrados sobre los 253
deltas:

```
ciclos = 5580 + 851,6 x filas + 177,5 x columnas     R2 = 1,0000
```

con residuo maximo de 0,14 ms. El R2 perfecto no es casualidad: el hardware
emulado cycle-exact es determinista y el costo es de verdad lineal en filas
y columnas. Lo que la cuenta a mano no veia: recorrer los 20 bits de la
mascara cuesta ~36 ciclos por columna aunque no este marcada, 720 por fila.
Si alguna vez hace falta, se puede saltar de a 8 columnas cuando un byte de
la mascara vale 0. Las constantes nuevas estan en `encoder\stream.h`.

**El limite de 40 ms por frame no es la regla que importa.** Un frame
completo tarda 60 ms y aun asi hubo un solo frame tarde. El reproductor
empieza a decodificar un delta cuando el intercambio anterior libera el
buffer oculto, y tiene hasta el VBL de ese delta: con `--min-hold 2` son
80 ms, salvo en los cortes de escena, que pueden venir pegados. Los 40 ms de
`CLAUDE.md` son el peor caso de dos deltas seguidos, no un limite por frame.
Ver la entrada siguiente.

---

## 2026-09-11 — El encoder simula la linea de tiempo del reproductor

**Decision:** en vez de un tope fijo de 40 ms por frame, el encoder simula
lo que hace el reproductor. Cada delta empieza a decodificarse cuando el
intercambio anterior libera el buffer oculto y tiene que estar listo para el
VBL en que le toca verse; con el modelo calibrado, el encoder sabe cuanto
tarda. Si un delta llegaria tarde por mas de `--max-late` VBL (2 por
defecto), se degrada; si no, se lo deja llegar tarde. `--frame-ms` queda
como tope fijo opcional, apagado por defecto.

**Esto se aparta de `CLAUDE.md`**, que pedia degradar o repartir todo frame
que pasara de 40 ms. Motivos, los dos medidos:

1. 40 ms no es el limite real, es el caso de dos deltas seguidos. Con
   `--min-hold 2` cada delta tiene 80 ms, salvo en los cortes de escena.
2. Degradar es subir el umbral de perdida por pixel, y eso deja salpicado
   persistente (Hito 2). Un frame que se ve 40 ms tarde, con la sincronia
   intacta, se nota mucho menos. Con el modelo calibrado y el tope fijo de
   40 ms se habrian degradado todos los frames de pantalla completa del
   opening, sin necesidad.

**Validacion:** con el modelo calibrado, la simulacion sobre `final22.a5v`
predice **1 frame tarde, por 2 VBL** (el 548, un corte de escena pegado a
otro delta): exactamente lo que midio la Amiga. Con `--max-late 2` no se
degrada nada y el stream sale identico byte a byte al que se midio. El peor
delta previsto, 60,5 ms, coincide con el medido, 60,3 ms.

"Repartir un frame en los siguientes", la otra opcion de `CLAUDE.md`, sigue
sin implementar: con estos numeros no hace falta.

---

## 2026-09-11 — Hito 5: el audio es un solo flujo continuo (formato v3)

**Decision:** el audio de todos los paquetes, puesto uno detras del otro, es
un unico flujo. El paquete `n` lleva las muestras `S(n-1)..S(n)-1`, con
`S(n) = 2 floor((n+1) x Hz_Paula / 24,960205 / 2)`: acumulacion fraccional
con las frecuencias exactas, redondeada a una cantidad par. En fib4 el
acumulador arranca en 0 y **sigue de un paquete al otro**. La cabecera gana
un byte de formato (offset 28: 0 ninguno, 1 fib4, 2 pcm8) y la version pasa
a 3. Todo en `FORMAT.md`.

**Motivos:**

- Muestras pares por paquete: en fib4 un paquete termina en un byte entero,
  y en el reproductor no hace falta llevar la cuenta de medio byte.
- Acumulador continuo: si se reiniciara en cada paquete, cada 40 ms habria
  un salto al centro, que suena como un zumbido de 25 Hz.
- El lector de audio del reproductor camina los paquetes por su cuenta,
  independiente del video. Si un frame llega tarde, el audio no se entera:
  la politica que pedia `CLAUDE.md` sale gratis.
- El error acumulado por el redondeo a par es siempre menor que 2 muestras
  (0,25 ms), dure lo que dure el video.

El encoder escribe en el `.crc` un CRC mas, el de todas las muestras que
tienen que sonar. El decoder de referencia decodifica el audio del bitstream
como el reproductor, lo compara (**176 424 muestras identicas** en el opening
de 22 s) y lo mezcla en el preview: lo que se oye en `preview.mp4` es fib4 de
verdad, no la fuente.

---

## 2026-09-11 — Remuestreo fino en C, no en ffmpeg

ffmpeg solo entrega frecuencias enteras, y Paula con periodo 443 toca a
8006,535 Hz. Pedirle 8006 o 8007 dejaria una deriva de 1,5 ms por minuto que
no hace falta. El encoder le pide a ffmpeg la entera de arriba (8007 Hz, con
su filtro antialias, que es bueno) y hace el ultimo paso con interpolacion
lineal a la frecuencia **exacta**. La razon es 1,00006: la interpolacion casi
no filtra y la duracion cuadra al microsegundo con la del video.

Con `--rate pal` (video 4 % mas rapido) el audio se lee mas rapido y sube de
tono como en la TV; se le pide a ffmpeg 8006,5/1,041 Hz para que el
antialias corte donde corresponde despues de acelerar. Con la cadencia
`native` que eligio Az no se toca.

---

## 2026-09-11 — fib4: la ganancia optima es 1, y subirla empeora

El opening llega a pico 38 de 127 en 8 bits (−10 dB). Lo obvio seria
normalizar, pero fib4 no puede subir mas de 21 ni bajar mas de 34 por
muestra: con mas amplitud, los agudos fuertes no alcanzan (sobrecarga de
pendiente). Medido sobre los 22 s, SNR de lo que suena contra la entrada:

| ganancia | pico | SNR fib4 | SNR pcm8 |
|---|---|---|---|
| 0,5 | 19 | 19,7 dB | |
| 0,85 | 33 | 20,8 dB | |
| **1,0** | 38 | **20,9 dB** | 28,4 dB |
| 1,25 | 48 | 20,8 dB | |
| 2,0 | 77 | 17,8 dB | |
| 3,3 | 126 | 12,1 dB | 38,8 dB |

Meseta entre 0,85 y 1,25; queda **1,0** (`--audio-gain`). pcm8 a 3,3 suena
mucho mejor (38,8 dB) pero ocupa el doble: 176 KB en vez de 88, o sea 88 KB
menos de video (11 %). Queda como opcion (`--audio-format pcm8
--audio-gain 3.3`); es una decision de Az, no tecnica.

El codificador fib4 es codicioso (el mejor nibble para cada muestra, sin
mirar adelante). Uno con busqueda hacia adelante daria varios dB mas al
mismo tamano, sin tocar el formato ni el reproductor: candidato para el
Hito 6.

---

## 2026-09-11 — Presupuesto unico y el disco lo arma el encoder

**Decision:** `--budget` cuenta cabecera mas paquetes, **audio incluido**. El
audio no se negocia con el control de tasa: se codifica primero y el video
se ajusta a lo que queda. Con `--adf`, el encoder arma el disco entero
(bootblock, reproductor, datos) y el presupuesto por defecto es exactamente
lo que queda en el disco despues del reproductor, no una estimacion.

La memoria no limita: en el Hito 0 se midieron 975 KB libres entre Chip y
slow. Descontando framebuffers, copper lists, buffers de Paula, rebote,
reproductor y el margen de Chip quedan ~915 KB para datos, contra 872 KB de
disco.

`build.ps1 disk` reserva al final del disco lo que necesita el disco de
medicion para usar **el mismo** `video.a5v`: su reproductor ocupa 7 sectores
mas (tablas de tiempos) y graba 6 sectores de resultados. Son 13 sectores,
6,5 KB. Para el disco final se podria recuperar, pero entonces lo que se
mide no es lo que se graba.

Resultado con el opening de 22 s: 889 856 bytes de presupuesto, 88 212 de
audio y 862 380 usados. Sobran 27 KB: la busqueda del control de tasa
arranca en `--quality 8` y solo sabe subir la perdida, y a esa calidad ya
entra. Aprovechar ese resto es trabajo del Hito 6.

---

## 2026-09-11 — Hito 5: audio en el reproductor y sincronia medida

**Diseno.** Dos buffers de 512 muestras en Chip (64 ms cada uno a
8006,5 Hz). Paula toca uno mientras la interrupcion de nivel 4 (AUD0) llena
el otro y lo encola en `AUD0LC`/`AUD1LC`: los dos canales tocan el mismo
buffer, uno de cada lado. El lector de audio recorre los paquetes por su
cuenta, con su propio puntero (pasa del bloque 1 al 2 en el mismo limite que
el video), y de cada paquete solo mira los bytes de audio. Cuando el flujo se
acaba repite la ultima muestra: silencio. El DMA de audio arranca dentro de
la interrupcion de VBL del frame 0. El filtro pasabajos queda encendido.

Al empezar a llenar, el nivel 4 baja la prioridad a 2 (`move.w #$2200,sr`):
llenar dura 4,5 ms, y sin eso el VBL se atrasaria y el intercambio de copper
list podria caer ya dentro de la pantalla.

**Medido** con el disco de medicion (WinUAE A500 cycle-exact, KS 1.2):

| | |
|---|---|
| Buffers pedidos por Paula | 345, exactamente los que caben en 1100 VBL |
| Primer buffer | 0,12 ms despues del VBL del frame 0 |
| Intervalo medio | 226 814,1 color clocks (esperado 512 x 443 = 226 816) |
| Diferencia acumulada | −0,185 ms en 22 s |
| Fin del audio contra fin del video | 0,04 ms |
| Reproduccion | 1100 VBL para 1100; 1 frame tarde por 2 VBL, el que predice el encoder |
| Llenar un buffer fib4 | 4,51 ms de media (32 015 ciclos), 4,63 el peor: **7,1 % de la CPU** |

**Sin deriva perceptible.** −0,185 ms en 22 s son 1,5 muestras en total, no
por buffer. Encaja con que Paula pida la primera interrupcion un periodo de
muestra mas tarde que las siguientes (un corrimiento fijo). Con solo la
primera y la ultima estampa no se lo puede separar de una deriva lenta, pero
aun si lo fuera serian 0,5 ms por minuto, cien veces menos de lo que se
nota.

**Bug encontrado midiendo.** La primera corrida daba −27 ms de "deriva": 346
interrupciones en vez de 345, la ultima justo al terminar. El reproductor
cortaba el DMA de audio antes de deshabilitar la interrupcion, y al cortar el
DMA Paula pide una interrupcion mas. Ahora se deshabilita primero y se acusa
la pendiente.

**El modelo de costo, con audio.** Con el audio la decodificacion media paso
de 34,9 a 37,6 ms y el peor delta de 60,3 a 65,2 ms: cada llenado le roba
tiempo al delta que este en curso. El encoder ahora lo simula: los llenados
llegan cada 512 x periodo color clocks desde el VBL del frame 0, cada uno
cuesta `A5_CYC_AUDIO_FILL` = 32 300 ciclos (32 015 medidos dentro de la
rutina, mas ~300 de entrar y salir de la interrupcion, contados a mano), y
un delta que empieza con un llenado en curso espera a que termine.

Validacion contra los 253 deltas medidos, reconstruyendo la linea de tiempo
del reproductor con los tiempos de la Amiga:

- el modelo predice en promedio el **99,9 %** de lo medido (sin el audio,
  93,7 %);
- la reconstruccion da 1 frame tarde, lo mismo que conto la Amiga;
- descontando los llenados, el ajuste da `4202 + 885,5 x filas + 176,5 x
  columnas` (R2 = 0,9981). Para un frame completo difiere del Hito 4 en un
  0,01 %: se mantienen las constantes del Hito 4, medidas sin el ruido del
  audio;
- el residuo maximo, 4,4 ms, es un caso de borde: el delta del frame 26
  empieza justo cuando llega el primer llenado, 0,12 ms despues del VBL, y
  no se puede saber si la estampa quedo antes o despues. Ahi el modelo
  sobreestima: se equivoca del lado seguro.

Con el costo del audio en la linea de tiempo, el encoder produce **el mismo
stream, byte por byte**, que el que se midio: no hubo que degradar nada. La
medicion valida al encoder tal como queda.

**pcm8, medido igual** (el opening con `--audio-format pcm8 --audio-gain
3.3`): llenar un buffer cuesta 3,94 ms (27 918 ciclos, el peor 4,05 ms), el
**6,2 %** de la CPU; `A5_CYC_AUDIO_FILL_PCM8` = 28 200. Sincronia identica a
fib4 (345 buffers, −0,186 ms). La Amiga mostro 4 frames tarde, por 2 VBL el
peor, y la reconstruccion da los mismos 4; con la constante, el encoder
predice esos mismos 4 y genera el mismo stream que se midio, byte por byte
(el modelo da el 99,9 % de lo medido). pcm8 casi no ahorra CPU porque el
bucle es de a una muestra; copiar de a palabras lo bajaria a una decima.

**Segundo bug encontrado midiendo.** La primera medicion de pcm8 dio "el
peor llenado: 1 210 893 ms". La estampa de salida del llenado se tomaba con
la prioridad ya bajada a 2, y el VBL podia entrar entre la lectura del
contador de VBL y la de la posicion del haz: estampa un frame atrasada, resta
negativa. `stamp_irq` corrige un VBL pendiente, no uno que la interrumpe a
mitad. Ahora se vuelve a prioridad 4 antes de estampar, y el decoder avisa si
un llenado mide mas de un frame. Solo afecta al disco de medicion. La media
de fib4 es limpia (su peor llenado, 4,63 ms, es posible); la de pcm8 se
repitio.

**7 % de CPU es bastante.** El bucle fib4 hace por byte dos sumas con tabla,
un corrimiento y una mascara. Con dos tablas de 256 bytes (el paso del nibble
alto y el del bajo, indexadas por el byte entero) bajaria casi a la mitad.
Hoy no hace falta, porque no cambia el atraso previsto; queda anotado.

---

## 2026-09-11 — Hito 6: paleta por franjas, medida

**Que es.** El area activa se parte en franjas horizontales de N filas
logicas, y cada una tiene su paleta de 8 colores por escena. El negro (color
0) es comun a todas, porque tambien es el color del borde. El Copper cambia
los colores 1–7 al principio de la primera linea de cada franja. Formato v4
(`FORMAT.md`).

**Por que entra en el Copper.** Cada linea de pantalla ya tiene un WAIT en
hpos $06 y dos MOVE de modulo (el doblado vertical del Hito 3). En la linea
donde empieza una franja van 7 MOVE de color entre el WAIT y los de modulo:
9 MOVE son 36 color clocks, terminan hacia el color clock 44, y la imagen
empieza en el 64. Con 16 colores serian 17 MOVE, ~74 color clocks, y los
ultimos colores cambiarian con la linea ya empezada: por eso el formato no
admite franjas con 4 planos.

**Medido** con el opening de 22 s y el presupuesto del disco. Hace falta
una metrica nueva, el error de lo que se ve **contra la fuente**: el error
contra el frame cuantizado ideal no sirve para comparar paletas distintas,
porque el ideal cambia con la paleta.

| franjas de | bytes a calidad 8 | umbral final | pixeles a > 0,10 del original | error medio |
|---|---|---|---|---|
| (una paleta) | 862 380 | 0,012 | 12,48 % | 0,0584 |
| 48 filas | 896 388 | 0,045 | 11,14 % | 0,0564 |
| 32 | 892 132 | 0,036 | 10,22 % | 0,0546 |
| 24 | 904 712 | 0,069 | 10,11 % | 0,0541 |
| **16** | 889 744 | 0,012 | **9,62 %** | 0,0535 |
| 12 | 912 110 | 0,074 | 9,95 % | 0,0541 |
| 8 | 889 906 | 0,018 | 9,64 % | 0,0528 |
| 4 | 913 468 | 0,074 | 10,04 % | 0,0538 |
| 1 | 921 744 | 0,078 | 10,19 % | 0,0540 |

Casi toda la ganancia llega con franjas de 16. Mas finas no mejoran: cuestan
mas bytes, y el control de tasa lo paga con perdida. Con 16 filas son 6
franjas, 96 bytes de paleta por corte de escena y 35 MOVE mas de Copper.

**Costuras.** A la vista, las franjas recuperan colores que con una paleta
se perdian (el pelo castano y el vestido de Winry, que con una sola paleta
salen grises). Pero dejan rayas horizontales en las zonas lisas: el fondo
rojo del titulo cae en un tono en una franja y en otro casi igual en la de
abajo. Juntar histogramas de franjas vecinas (`--band-overlap`) no las
quita. Lo que funciona es `--band-snap`: un color a menos de δ de uno de la
franja de arriba pasa a ser exactamente ese.

| δ | pixeles a > 0,10 | bytes | a la vista |
|---|---|---|---|
| 0 | 9,62 % | 889 744 | rayas en el fondo del titulo |
| **0,04** | 10,04 % | 889 576 | la raya grande desaparece |
| 0,06 | 11,17 % | 873 456 | liso, pero la cara pierde color |
| 0,08 | 11,81 % | 874 544 | vuelve al gris de una paleta |

**Decision (confirmada por Az el 2026-09-11,** despues de mirar una paleta y
franjas lado a lado, costuras incluidas**):** franjas de 16 filas con
δ = 0,04 por defecto. Conserva casi
toda la mejora (de 12,5 % a 10,0 % de pixeles lejos del original) y entra en
el disco practicamente sin perdida: la tabla de franjas agrando el
reproductor un sector, y el control de tasa subio el umbral de 0,012 a
0,0128, sin cambio medible en el error ni en el salpicado. `--band-rows 0`
vuelve a la paleta unica, y el stream sale byte por byte igual al del
Hito 5, salvo el numero de version.

**En la Amiga** (disco de medicion, WinUAE A500 cycle-exact, KS 1.2): carga
de 888 954 bytes en 48,3 s; 1100 VBL para 1100; 1 frame tarde por 2 VBL, el
mismo que predice el encoder; el modelo de costo predice el 99,9 % de lo
medido y su ajuste sin el audio (`4435 + 878,1 x filas + 177,3 x columnas`)
es el mismo del Hito 5. Las franjas no le cambian el costo al reproductor:
las paletas se escriben solo en los cortes de escena (~50 palabras) y los 35
MOVE de Copper por frame no se notan.

**Verificacion.** El decoder arma, con la misma receta que `build_copper` +
`write_palette`, el copper list que tendria que tener el reproductor, y lo
compara byte por byte con el que vuelca el disco de prueba
(`build.ps1 still`): 3312 bytes, 6 franjas, identico. La captura de WinUAE
muestra los cambios de paleta sin cortes a mitad de linea.

---

## 2026-09-11 — Dither ordenado: medido, queda apagado

`--dither bayer2|bayer4` existia desde el Hito 1; ahora se midio con el
bitstream real. Cuesta ~5 KB (868 412 y 867 224 bytes contra 862 380) y el
error contra la fuente no mejora (0,0588 contra 0,0584). A la vista, con la
fuerza por defecto casi no se distingue. Queda opcional y apagado, como
pedia `CLAUDE.md`.

---

## 2026-09-11 — Control de tasa: ahora tambien baja la perdida

Si el stream entra y sobra disco, el encoder baja la perdida (busqueda
binaria entre 0 y la calidad pedida) para usarlo. Con el opening no cambia
nada: el umbral de `--quality 8` (0,012) ya es sin perdida, porque dos
colores RGB444 distintos estan siempre mas lejos que eso, y con umbral 0 sale
el mismo stream. Si alguna vez sobra disco sin perdida, lo que queda para
gastar es la histeresis del cuantizador (`--stability`), que es la que mas
bytes ahorra; queda anotado.

---

## 2026-09-11 — Dos bugs de este hito

- `frame_crc` del decoder serializaba las paletas en un buffer de 32 bytes
  (una paleta de 16 colores). Con franjas se desbordaba y los CRC no
  coincidian. La verificacion lo detecto en los tres streams de prueba con
  franjas; los de una paleta pasaban.
- El disco de prueba volcaba el copper list con un segundo `DoIO` y perdia
  el error del primero, porque `DoIO` no preserva d1. Solo afectaba al
  informe.

---

## 2026-09-11 — Hito 7: funciona en la A500 real

Az grabo `work\a500vp.adf` (el opening de 22 s, con franjas de paleta y
audio fib4) en un disquete y lo probo en su A500: segun Az, "funciono a la
perfeccion". No reporto diferencias con WinUAE, asi que no hubo nada que
corregir. No hay mediciones propias del hardware real (tiempo de carga,
sincronia); si alguna vez hacen falta, el disco de medicion (`build.ps1
play`) graba sus resultados en el mismo disquete y se leen desde el PC.

Antes de entregarlo, el disco final (no el de medicion) se arranco entero en
WinUAE con `tools\shot.ps1 -Every 5`: carga al mismo ritmo que el de
medicion y reproduce desde los ~55 s de encendida la maquina.

---

## 2026-09-12 — Imagen real: la receta se da vuelta

**Metodo:** Az probo `btf.mp4` (1920x1080, 25 fps exactos, progresivo,
22,29 s, imagen real) con los parametros del anime y "se veia horrible".

**Por que cuesta tanto mas que el anime.** Tres cosas, las tres medidas:

| | anime (lo que anduvo) | btf.mp4 |
|---|---|---|
| Frames casi identicos al anterior | 45,6 % | **18,1 %** |
| Escenas | 15 en 30 s | **36 en 22 s** |
| Tamano sin perdida | 1,63 MB (30 s) | **1,27 MB (22 s)** |

A 25 fps contra los 24,96 de la pantalla la cadencia es 1:1: no hay
repeticiones regaladas. El anime esta animado en dos y de ahi salia medio
disco. Ademas, imagen real es tono continuo: 8 colores por escena estan
pensados para colores planos de cel.

Con los defaults el control de tasa tuvo que subir el umbral a **0,216**,
que es exactamente el territorio que el Hito 2 dio por inaceptable:
**34,6 % de los pixeles** con restos del frame anterior pegados.

**Medido** (22 s salvo aviso, error contra la fuente):

| variante | sin perdida | umbral | vs. fuente |
|---|---|---|---|
| 8 col, min-hold 2 (default) | 1 267 420 | 0,216 | 35,2 % |
| 8 col, min-hold 3 | 986 268 | 0,122 | 22,9 % |
| 8 col, min-hold 4 | 816 404 | ninguno | 19,6 % |
| 8 col, min-hold 3 + dither bayer4 | 995 822 | 0,126 | 23,8 % |
| 8 col, min-hold 2, 15 s | 790 200 | ninguno | 9,4 % |
| 16 col, min-hold 2, 12 s | 818 548 | ninguno | 7,7 % |
| 16 col, min-hold 2, 13 s | 890 610 | 0,058 | 8,2 % |
| **16 col, min-hold 2, sharpen 0, 13 s** | **824 164** | **ninguno** | **6,7 %** |
| 16 col, min-hold 2, sharpen 0, 15 s | 975 400 | 0,100 | 10,5 % |
| 16 col, min-hold 2, sharpen 0, 17 s | 1 146 994 | 0,148 | 22,4 % |

**Decision para imagen real: `--planes 4 --min-hold 2 --sharpen 0` y acortar
el clip.** Con 13 s entra sin ninguna perdida y queda en 6,7 %, mejor que el
disco de anime (9,6 %). Los tres cambios contradicen lo decidido para el
anime, y cada uno por su motivo:

1. **16 colores.** El Hito 2 los descarto porque el 4.º plano cuesta 33 %
   mas bytes y el anime no los necesitaba. Aca el error de cuantizacion baja
   de 0,036 a 0,026 solo por tener la paleta: la piel y los degrades la
   piden.
2. **Sin franjas de paleta** (con 16 colores el encoder las apaga solo,
   `encode.c:814`, porque el Copper no llega). En imagen real las franjas
   son **contraproducentes**: sobre un cielo liso cada franja elige un azul
   apenas distinto y queda una costura horizontal durisima, mucho peor que
   lo que `--band-snap` corrige en el anime. Se ve en `work\btf8.mp4`.
3. **`--sharpen 0`.** El default 1,2 existe porque el anime vive de la linea
   negra de contorno; en imagen real solo realza grano de pelicula. Bajo el
   error de 8,2 % a 6,7 % **y** libero 66 KB.

**Lo que no funciono:** el dither ordenado perdio las tres veces que se
probo (igual que en el Hito 6). Y `--stability 0.15` parecia mejorar
(salpicado 29,4 % contra 34,6 %) pero **empeoraba contra la fuente**
(38,2 %): la histeresis pega colores viejos, y la metrica de salpicado no lo
ve porque compara contra el frame cuantizado ideal, no contra el original.
Es la misma trampa que obligo a inventar la metrica "vs. fuente" en el
Hito 6.

**Costo:** con 16 colores el 68000 va mas justo. El modelo predice 39 frames
tarde (el peor por 2 VBL) y 3 degradados, contra 1 solo en el disco de
anime.

**Bug encontrado:** con una fuente mas rapida que 24,96 fps los frames no se
repiten, se **descartan**, y la resta sin signo informaba "4294967295 frames
repetidos".

---

## 2026-09-12 — Video en streaming desde disquete: no da

**Esto contradice a `CLAUDE.md`**, que deja como opcion futura "soporte
multidisco en streaming con cambio de disco sin cortar el video". Para
**video** eso no existe, y no es un problema de software.

| | bytes/s |
|---|---|
| Lectura medida (Hito 4, trackdisk con rebote) | 17,9 KB/s |
| Techo fisico de una disquetera DD | ~28 KB/s (11 sectores x 512 por vuelta, 5 vueltas/s) |
| Anime, como quedo | 39 KB/s |
| btf, la receta de 13 s | 63 KB/s |

El video se consume 2 a 3,5 veces mas rapido de lo que el disco entrega.
**Medido** lo que se podria mostrar si el bitstream se limitara a la
velocidad de lectura (btf, 22 s, `--min-hold 4 --sharpen 0`):

| presupuesto | umbral | vs. fuente |
|---|---|---|
| 403 000 (17,9 KB/s, trackdisk de hoy) | 0,330 | **50,7 %** |
| 528 000 (24 KB/s, trackloader propio) | 0,243 | **40,5 %** |

Las dos son peores que la version que Az llamo horrible (35 %). Ni con el
trackloader propio con Blitter alcanza.

**Lo que si se puede: multidisco por tandas.** Cargar, tocar, pedir el disco
siguiente, cargar, seguir: ~13-22 s de video por disquete con **~50 s de
intermedio**. Es un formato de capitulos, no una pelicula continua. El
diseno tendria un `stream_id` de 32 bits en la cabecera (para rechazar el
disco 2 de otra pelicula), corte en limite de paquete y preferentemente en
un cambio de escena, y los dos framebuffers simulados en negro al empezar
cada disco (primer frame como delta completo, ~6 KB, gratis si cae en un
corte).

**Precarga parcial:** no hace falta cargar todo antes de empezar, alcanza
con juntar la diferencia entre consumo y lectura. Para el anime,
`(39 - 17,9) x 22 = 464 KB`, o sea **26 s de espera en vez de 50**. Pero
exige leer mientras se reproduce, y hoy no hay CPU: la decodificacion media
ya esta en 37,6 ms de los 40. La precarga parcial y el trackloader propio
son el mismo proyecto.

**Donde el multidisco si funciona: el audio.** ADPCM son 3,91 KB/s contra
17,9 de lectura, 4,6x de margen. En A5MU se podria arrancar la musica a los
2 segundos en vez de esperar la carga entera, y encadenar discos **sin
cortar**: con 60 s de buffer (235 KB de los ~900 disponibles) sobra tiempo
para cambiar el disquete. Si el multidisco se hace alguna vez, conviene
hacerlo primero ahi.

---

## 2026-09-12 — El techo no lo pone el disco, lo pone la CPU

**Pregunta de Az:** si el tamano del archivo no fuera problema, cuanta
calidad se podria tener.

**Medido** (btf, 13 s, `--quality 0 --budget 0 --min-hold 1 --sharpen 0`,
o sea sin ninguna perdida, sin tope de tamano y actualizando los 24,96
huecos por segundo):

| | 16 colores | 8 colores + franjas |
|---|---|---|
| Error de cuantizacion | 0,0263 | 0,0321 |
| vs. fuente | **1,35 %** | 3,64 % |
| Salpicado | 0,04 % | — |
| Tasa | **84,3 KB/s** | 62,3 KB/s |
| Frames tarde | **304 de 324** | 281 de 324 |
| Peor atraso | **129 VBL (2,6 s)** | 42 VBL |

O sea: la imagen puede llegar a **1,35 % de pixeles lejos del original**
(contra 6,7 % de la receta que entra en un disquete), pero **el 68000 no
puede mostrarla**. Con el modelo de costo calibrado del Hito 4
(`5580 + 852 x filas + 177 x columnas` con 3 planos), repintar la pantalla
entera cuesta 60,3 ms con 8 colores y ~75 ms con 16. Un hueco de pantalla
dura 40,06 ms.

**El limite duro: el 68000 repinta la pantalla entera unas 13 veces por
segundo con 16 colores y 16 con 8 colores.** Donde cambia poco se puede ir a
los 24,96 fps; donde cambia todo, no. Por eso `--min-hold 2` no es una
concesion al disco, es lo que el procesador puede.

**Y la RAM tambien pone un techo:** a 84,3 KB/s, los ~900 KB que entran en
memoria son **10,7 segundos** de video. Aun con el disquete fuera de la
ecuacion, cargar y reproducir no pasa de ahi.

---

## 2026-09-12 — H10: el decodificador desenrollado, 22 % mas rapido

**Metodo:** el mismo stream (`fmab_op.a5v`, 22 s, 3 planos, franjas, audio
fib4) en el disco de medicion, antes y despues. WinUAE A500 68000
cycle-exact, KS 1.2.

**Dos cambios en `apply_delta` (`player/video_code.i`), sin tocar el
formato:**

1. **Saltar 8 columnas de una** cuando el byte de mascara vale 0, en vez de
   caminar los 20 bits a ~36 ciclos cada uno. Es el H10 de la hoja de ruta,
   propuesto por Az.
2. **Desenrollar la escritura de planos.** El lazo pagaba un `dbf` y un
   `lea` por plano y por columna marcada (~22 ciclos cada uno). Ahora hay
   una instancia de macro por cantidad de planos (3 y 4, las unicas que se
   usan; 1 y 2 siguen con el lazo generico) y el plano se direcciona con el
   desplazamiento `d16(An)`, que entra de sobra en 16 bits.

**Medido:**

| | antes | despues |
|---|---|---|
| Decodificacion media | 38,63 ms | **29,96 ms** (−22 %) |
| Peor delta | 65,26 ms | **50,54 ms** (−23 %) |
| Frames tarde | 1, por 2 VBL | 1, por 1 VBL |
| Ciclos por columna | 177,3 | **126,6** |
| Ciclos por fila | 877,5 | 900,5 |

**El atajo de columnas casi no sirvio, y conviene decirlo.** El costo por
fila **subio** de 877,5 a 900,5: en material real las filas marcadas son
densas, asi que el atajo casi nunca dispara y se pagan los dos `cmp.l` de
mas. Toda la ganancia vino del desenrollado, que no estaba en la hoja de
ruta. Se deja igual porque en material mas disperso puede pagar, y cuesta
24 ciclos por fila.

**Correccion:** el reproductor crecio 504 bytes (3544 → 4048).

**Verificacion byte a byte** con el disco de frame fijo, que compara el
framebuffer contra el decoder de referencia en C:

| stream | delta | framebuffer |
|---|---|---|
| `video.a5v`, 3 planos, frame 182 | 8080 de 8080 | 15 360 identicos |
| `btf_final.a5v`, 4 planos, frame 100 | 7790 de 7790 | 20 480 identicos |
| `btf_final.a5v`, 4 planos, frame 300 | 7818 de 7818 | 20 480 identicos |

Que el delta consuma **exactamente** los bytes esperados es lo que prueba
que el atajo de 8 columnas no se desincroniza con la mascara.

**Modelo de costo recalibrado** (`encoder/stream.h`). Con las constantes
del Hito 4 el encoder predecia el **123 %** de lo medido, o sea que habria
degradado frames sin necesidad. Nuevas constantes: `A5_CYC_FRAME` 0,
`A5_CYC_ROW` 900, `A5_CYC_COL` 19, `A5_CYC_BYTE` 36.

**Primera medicion con 4 planos** (nunca se habia hecho; `btf_final.a5v`,
13 s, 16 colores):

| | |
|---|---|
| Ajuste | `4003 + 795,7 x filas + 168,5 x columnas` (R2 = 0,9954) |
| Ciclos por columna: modelo 19 + 4 x 36 = 163 | medido **168,5** |
| Media de 165 deltas | 39,06 ms |
| Peor delta | 60,22 ms (modelo: 59,68) |
| Frames tarde | 12 de 324, el peor por 2 VBL |

El reparto entre columna y byte salia de contar instrucciones sobre el caso
de 3 planos; que con 4 planos de 168,5 contra 163 previstos confirma el
reparto. El costo por byte literal es 42,2 ciclos con 3 planos y 42,1 con
4: practicamente el mismo.

Con las dos cantidades de planos el modelo predice el **100 %** de lo
medido en promedio.

**Bug encontrado midiendo:** la tabla de tiempos se grababa con
`IOERR_BADLENGTH` (−4). trackdisk exige longitudes multiplo de 512 y
`TIMING_MAX` habia quedado en 576 (576 x 4 = 2304). Tiene que ser multiplo
de 128; quedo en 512, que ademas es lo que hace entrar el stream del Hito 6
ahora que el reproductor es mas grande.

---

## 2026-09-12 — Mas colores no es lo que falta: el error es temporal

**Pregunta de Az:** despues del H10, ir por mas colores (H16/H17).

**Lo que dice la medicion: no.** Con la receta de imagen real (btf, 13 s,
16 colores, `--min-hold 2`) el stream entra **sin ninguna perdida
espacial**, asi que el 6,61 % de error contra la fuente no se reparte como
uno esperaria:

| | vs. fuente |
|---|---|
| Techo de cuantizacion con 16 colores (sin perdida, 24,96 fps) | 1,35 % |
| La receta real (`--min-hold 2`, 13 s) | 6,61 % |

Los ~5,3 puntos de diferencia **no son de color**: son frames sostenidos
dos huecos. Es error temporal, no cromatico.

**Medido, subiendo la cadencia en vez de los colores:**

| 16 colores | vs. fuente | frames tarde |
|---|---|---|
| min-hold 2, 13 s | 6,61 % | 7 |
| min-hold 2, 9 s | 6,39 % | 6 |
| **min-hold 1, 10 s** | **2,70 %** | **186, el peor por 11 VBL** |
| min-hold 1, 9 s | 2,85 % | 161, el peor por 11 VBL |
| min-hold 1, 11 s | 5,25 % | 132 (y ya con perdida) |

Ir a 24,96 fps baja el error **2,4 veces**. Mas colores compraria un 25 %
de los 1,35 puntos de cuantizacion (medido con el encoder HAM de A5MU sobre
tres frames de la pelicula: HAM da 0,0168-0,0178 contra 0,0189-0,0231 de 16
colores), o sea unas **3 decimas de punto**. No hay comparacion.

**Pero el premio todavia no se puede cobrar:** a min-hold 1 la CPU no
llega. 186 de 250 frames tarde y 31 degradados sin conseguir cumplir el
`--max-late 2`.

**Y mas colores empuja en contra.** Seis planos son 1,5x mas bytes *y*
1,5x mas tiempo por columna: con el modelo recalibrado el repintado
completo pasa de 56 ms (4 planos) a 76 ms, de los 80 que da min-hold 2. Se
cierra la puerta a min-hold 1 para siempre.

**Decision: H12 (vector de movimiento global con el Blitter) antes que
H16/H17.** H12 es justo lo que hace falta: abarata el delta, que es lo
unico que puede hacer viable min-hold 1. Si despues sobra presupuesto, ahi
se discuten los colores.

**Nota sobre el H10:** con las constantes viejas 6 planos daban 102 ms y
eran imposibles; ahora dan 76 y entran. O sea que mas colores paso de
imposible a posible-pero-contraproducente.

---

## 2026-09-12 — H12 medido en C: el vector de movimiento no sirve, la copia si

**Metodo:** antes de tocar el formato o el ensamblador se midio la ganancia
en el encoder, con dos opciones nuevas y **experimentales** (`--predict
visible` y `--motion R`): no producen un stream valido, porque el
reproductor todavia no hace la copia. Solo sirven para decidir si vale la
pena escribir el asm.

**La idea tenia dos mitades:**

1. **Predecir desde el buffer visible** en vez del oculto. Hoy el delta va
   contra el penultimo frame distinto: con `--min-hold 2` son 160 ms de
   movimiento. Copiando el visible al oculto antes del delta se predice
   desde 80 ms.
2. **Correr esa copia (dx,dy)** con un vector global, para seguir un paneo.
   Era la mitad que la hoja de ruta llamaba "la mas prometedora".

**Medido** (btf, 13 s, 16 colores, `--min-hold 2`, sin perdida):

| | bytes |
|---|---|
| Delta contra el oculto (hoy) | 816 590 |
| **Contra el visible** | **641 448 (−21,4 %)** |
| Contra el visible + vector (radio 3) | 641 376 (−0,01 % mas) |

**El vector de movimiento no sirve, y conviene decirlo fuerte porque era la
apuesta.** Setenta y dos bytes en todo el video, y solo 36 de 495 deltas
eligieron un vector distinto de cero. El motivo mas probable: a 160 px de
ancho un paneo de camara es casi siempre movimiento sub-pixel, y ademas lo
que se compara son **indices de paleta**, que no se conservan al correr la
imagen (el cuantizador elige por pixel). Toda la ganancia esta en la mitad
trivial.

**Lo que compra esa ganancia: la cadencia completa.** Con el presupuesto
liberado, `--min-hold 1` (24,96 fps) pasa a entrar:

| btf, 16 colores | bytes | vs. fuente | frames tarde |
|---|---|---|---|
| min-hold 2, 13 s (hoy) | 816 590 | 6,61 % | 7 |
| min-hold 1, 13 s, visible | 858 066 | **2,18 %** | 58, el peor por 4 VBL |
| min-hold 1, 10 s, visible | 635 084 | **1,47 %** | 32, el peor por 3 VBL |

El error baja **tres veces**, y es justo el error temporal que se habia
identificado como dominante. Para comparar, el techo absoluto de la maquina
medido el mismo dia es 1,35 %.

**Lo que falta antes de creerle a estos numeros:** la linea de tiempo del
encoder **todavia no cuenta el costo de la copia**. En la Amiga la haria el
Blitter: 15 360 bytes del area activa con 4 planos son ~7 680 palabras, o
sea ~4,3 ms si el Blitter tuviera todos los slots y mas cerca de 8-9 ms
compitiendo con 4 bitplanes en lowres. La CPU tiene que esperarla, porque
el delta escribe en el mismo buffer. Con 40 ms de presupuesto por delta a
min-hold 1, esos 8 ms importan: hay que medirlos y meterlos en el modelo
antes de decir que esto es reproducible.

**Consecuencia de diseno:** la copia tiene que ser **opcional por frame**
(un opcode nuevo, no un cambio global). En un corte de escena predecir
desde el visible no sirve de nada y la copia seria trabajo tirado; el
encoder ya simula la linea de tiempo, asi que puede elegir por frame entre
"delta" y "copia + delta" mirando bytes **y** milisegundos.

---

## 2026-09-12 — H12: cuanto cuesta la copia con el Blitter, medido

**Metodo:** el disco de medicion, al terminar la reproduccion y **con la
imagen todavia en pantalla**, copia 16 veces el area activa de un
framebuffer al otro con el Blitter (un blit por plano, A → D, minterm $F0)
y lo cronometra con el haz. Con la imagen puesta el Blitter compite por los
slots de DMA con los bitplanes, que es la condicion real; y la CPU se queda
esperandolo con `btst #6,DMACONR`, que tambien es el caso real, porque el
delta escribe en el mismo buffer. WinUAE A500 68000 cycle-exact, KS 1.2,
`btf_final.a5v`: 4 planos, 94 filas activas.

| | media | peor | ciclos de CPU |
|---|---|---|---|
| Blitter normal | 8,814 ms | 10,191 ms | 62 527 |
| Blitter **nasty** (`BLTPRI`) | **6,685 ms** | 7,358 ms | 47 422 |

**Nasty sale gratis y conviene.** Le da al Blitter prioridad sobre la CPU,
que normalmente seria malo, pero aca la CPU no tiene nada que hacer: lo
esta esperando. Ahorra un 24 %.

Por palabra copiada son ~3,2 color clocks con nasty (el minimo teorico son
2), o sea ~126 ciclos de CPU por plano y por fila. Para 3 planos y 96 filas
la copia saldria ~36 300 ciclos = 5,1 ms.

**Con esto, la idea cierra.** Con `--min-hold 1` el presupuesto por delta
es 40,06 ms. Un delta tipico con prediccion desde el visible son ~938 bytes
(~234 columnas con 4 planos), o sea ~10,6 ms, mas 6,7 de copia: **17 ms de
40**. El caso que no entra es el corte de escena (pantalla completa, 56 ms),
pero justamente ahi predecir desde el visible no sirve de nada y el encoder
no tiene que pedir la copia.

**Confirma la decision de diseno:** la copia va como **opcode opcional por
frame**, no como cambio global. El encoder ya simula la linea de tiempo;
con esta constante puede elegir por frame entre "delta" y "copia + delta"
mirando bytes y milisegundos.

**Falta:** meter la constante en el modelo (`A5_CYC_BLIT`, ~126 ciclos por
plano y por fila con nasty), hacer la eleccion por frame, y recien despues
el formato nuevo y el reproductor.

---

## 2026-09-12 — 5 bitplanes: 32 colores, implementado y medido

**Por que se reabrio.** Az insistio en que a su ojo mas colores es mas
calidad que mas fps, y tenia razon en algo que la metrica no ve: el
*banding* en un degradado son muchos pixeles apenas corridos, todos por
debajo del umbral de 0,1, asi que el "% de pixeles lejos de la fuente" no
lo cuenta. El ojo en cambio ve el **borde** entre dos escalones, que es una
linea larga y coherente.

**Simulacion honesta primero** (`encoder/ehbtest.c`, fuera del build): el
mismo frame a la geometria real del video (160x96 logicos), con el k-means
en Oklab y el redondeo a RGB444 del proyecto:

| | error medio |
|---|---|
| 16 colores (4 planos) | 0,0269 |
| **32 colores (5 planos)** | **0,0202** |
| 64 EHB (32 + sus mitades) | 0,0192 |
| 64 libres (no existen en OCS) | 0,0172 |

Dos cosas: **EHB se queda con el 78 % de lo que darian 64 colores libres**
(la restriccion del hardware sale barata), y **32 colores ya capturan el
86 % de lo que da EHB**, por la mitad del sobrecosto. Mirando las imagenes,
con 16 el cielo tiene tres escalones con bordes duros; con 32 casi
desaparecen.

**Implementacion:** `A5_MAX_COLORS` 16 → 32, `A5_MAX_PLANES` 4 → 5,
`--planes` acepta 5, y el reproductor gana una instancia mas de la macro
`DELTA_ROWS`. Con 5 planos no hay franjas de paleta (ya se apagaban solas
con mas de 3).

**Medido con el encoder real** (btf, `--sharpen 0 --min-hold 2`):

| | cuantizacion | sin perdida | vs. fuente |
|---|---|---|---|
| 4 planos, 13 s | 0,0263 | 816 590 | 6,61 % |
| **5 planos, 13 s** | 0,0212 | 1 017 266 | **12,10 %** |
| 4 planos, 10 s | | 621 440 | 6,12 % |
| **5 planos, 10 s** | | 774 412 | **5,28 %** |
| 5 planos, 11 s | | 839 928 | 5,09 % |
| 5 planos, 13 s + prediccion (H12) | | 797 234 | **5,81 %** |

**El resultado importa y es contraintuitivo: a 13 s, 5 planos es peor que
4** (12,10 % contra 6,61 %). El color mejora lo previsto, pero el stream se
va a 1,02 MB contra 883 KB de disco y el control de tasa tiene que meter
perdida, que cuesta mas de lo que el color gana.

**A igual duracion, 5 planos gana claro** (5,28 % contra 6,12 % a 10 s). Y
con la prediccion desde el visible del H12, entra a 13 s con 5,81 %: se
queda con los segundos **y** con el color.

**Conclusion: 5 planos solo conviene con H12 hecho, o acortando a ~11 s.**

**Costo de decodificacion:** el peor delta pasa de 55,1 ms (4 planos) a
64,7 ms, contra los 80 ms que da `--min-hold 2`. Entra, pero el margen para
la copia del Blitter (8,5 ms con 5 planos) queda justo.

**Verificacion byte a byte** con el disco de frame fijo, en las tres
cantidades de planos: 3 (con franjas) 15 360 bytes, 4 (20 480) y 5
(25 600), todos identicos al decoder de referencia, y el delta consumiendo
exactamente los bytes esperados.

**Bug del arnes, no del reproductor.** La primera corrida con 5 planos dio
1997 bytes distintos a partir del offset 20480. El volcado del disco de
prueba reservaba los sectores 1680–1719, o sea 40 sectores = 20 480 bytes =
exactamente 4 planos: el plano 4 se escribia encima del volcado del copper
list. La zona pasa a 1660–1709 (50 sectores) y el copper a 1710.

---

## 2026-09-12 — H12 hecho: la copia del visible, medida y verificada

**Que se implemento.** Formato v5, bit 1 de los flags del paquete: antes
de aplicar el delta, copiar el area activa del buffer visible al oculto.
El delta entonces predice desde el ultimo frame distinto (80 ms) en vez
del penultimo (160 ms con `--min-hold 2`). El encoder codifica las **dos**
variantes por frame y elige mirando bytes y milisegundos (`--predict
auto`); el reproductor copia con un blit A -> D por plano y espera al
Blitter con `BBUSY`. `BLTPRI` (nasty) queda encendido toda la
reproduccion: mientras copia, la CPU solo lo espera.

El vector de movimiento global que este hito proponia **no se
implemento**: medido el mismo dia, ahorraba 72 bytes en todo el video.

**Lo que gana, con el encoder real** (btf, 13 s, `--sharpen 0`):

| | vs. fuente, sin H12 | con H12 |
|---|---|---|
| 4 planos, `--min-hold 2` | 6,61 % | 6,61 % (el disco ya no era el limite: sobran 242 KB) |
| **5 planos, `--min-hold 2`** | 12,10 % | **5,89 %** |
| **4 planos, `--min-hold 1`** | no entraba | **2,81 %** |

O sea: H12 no mejora el caso que ya entraba holgado; **hace entrar los dos
que no entraban**. Con 5 planos el stream sin perdida pasa de 1,02 MB a
797 KB y deja de necesitar perdida; con `--min-hold 1` la cadencia completa
(24,96 fps) entra con 24 KB de sobra. 149 de 165 deltas piden la copia; los
16 que no son los cortes de escena, donde el visible no predice nada.

**Verificacion byte a byte de toda la reproduccion, no de un frame.** El
unico control exacto que habia era el disco de frame fijo: un delta desde
negro, que no pasa por el doble buffer ni por ninguna copia. Ahora el
reproductor de medicion calcula el CRC32 de los dos framebuffers tal como
quedaron en Chip y lo deja en el sector de informacion; `a500vp-dec
--measure` calcula el mismo sobre los suyos (doblando cada byte logico) y
falla si no coincide.

- btf, 5 planos, 13 s, **151 copias**: `3DFB25DC` y `C01DCA3A` en la Amiga
  y en el decoder de referencia. Identicos.
- Y el disco de frame fijo sigue dando 25 600 bytes identicos con 5 planos.

**Lo que costo, medido en el propio lazo de reproduccion** (no en un banco
sintetico como la vez anterior): 151 copias, **9,259 ms de media**, la peor
14,885 ms. De ahi hay que descontar los llenados de audio que caen dentro
de la espera, que la linea de tiempo ya cuenta aparte: si un llenado
(5,04 ms) cae con probabilidad copia/64 ms, la copia pura queda en 8,58 ms
= **130 ciclos por plano y por fila** con 5 planos (126 con 4, medido antes
sin audio). La peor copia es justo una con un llenado adentro.

**El bug que encontro esta medicion, y no era el H12.** La primera corrida
con 5 planos dio 33 frames tarde donde el modelo preveia 12, y el peor
delta 84,0 ms contra 73,7 previstos: el modelo predecia el 90 % de lo
medido. La causa es que `A5_CYC_BYTE` se habia calibrado con 3 y 4 planos.
En lowres el DMA de bitplanes usa slots que con 4 planos o menos le sobran
a la CPU, y **con 5 se los empieza a sacar**:

| planos en pantalla | ciclos por byte | por columna (modelo) | medido |
|---|---|---|---|
| 3 | 36 | 127 | 126,6 |
| 4 | 36 | 163 | 168,5 |
| **5** | **43** | **234** | **235,6** |

Con el byte a 43 el modelo pasa a predecir el **99,1–99,4 %** de lo medido
y el peor frame 83,0 ms contra 83,6 reales. Es la tercera vez que el modelo
de costo hay que recalibrarlo despues de cambiar algo: **ninguna cifra del
modelo vale para una cantidad de planos que no se midio.**

**Estado final con 5 planos a 13 s** (`h12b_p5_mh2.a5v`, medido en WinUAE):
648 VBL para 648 esperados, 30 frames tarde el peor por 2 VBL, audio con
0,10 ms de deriva en 13 s, carga 797 KB en 43,3 s a 18,0 KB/s. El peor
delta real es 83,6 ms contra los 80 que da `--min-hold 2`: por eso quedan
esos frames tarde, y el reproductor los absorbe con las repeticiones.

---

## 2026-09-12 — Cuanto se puede estirar la duracion con 32 colores

**Pregunta de Az:** con el H12 hecho y 32 colores andando, que mas se puede
hacer para estirar la reproduccion.

**Medido** (btf, 5 planos, `--sharpen 0 --predict auto`, presupuesto real
del disco), error contra la fuente:

| duracion | `--min-hold 2` | `--min-hold 3` | `--min-hold 4` |
|---|---|---|---|
| 13 s | **5,89 %** | | |
| 14 s | **6,01 %** | | |
| 15 s | 10,05 % | | |
| 16 s | 14,53 % | **10,24 %** | |
| 18 s | 25,92 % | 18,03 % | |
| 20 s | | 27,60 % | |
| 22 s | | | 26,62 % |

**El segundo 14 es gratis** (+0,12 puntos): a 13 s sobraban 83 KB de disco.
Del 15 en adelante el disco empieza a morder y cada segundo cuesta caro.

**Pasado el limite del disco, bajar la cadencia gana a meter perdida:** a
16 s, `--min-hold 3` da 10,24 % contra 14,53 % de `--min-hold 2`. Con menos
actualizaciones cada una puede ir sin perdida.

**El techo es aritmetico y no lo mueve ninguna opcion del encoder:**

- Un disquete DD da 883 712 bytes de datos. A 63 KB/s (la receta de btf con
  32 colores) son **14 s**. Duracion = bytes del disco / tasa.
- **La RAM no es el limite:** 975 KB libres menos 68 KB de framebuffers,
  copper lists y buffers de audio con 5 planos = 907 KB para datos, mas que
  los 883 KB del disco. Por eso **cargar mientras reproduce no estira
  nada** hoy: no hay bytes que no quepan en RAM. Solo serviria para empezar
  a ver antes (hoy la carga son 43 s de barra) o con un disco mas grande.
- **Multidisco tampoco duplica.** El consumo (63 KB/s) es 3,5 veces la
  lectura (18 KB/s medidos), asi que reproduciendo se drena la RAM: la
  duracion total es RAM / (tasa − lectura) = 907 / 45 = **20 s** con dos
  discos, no 28. Con el H8 (formato de pista propio, ~26 KB/s) seria
  907 / 37 = **24,5 s**.
- Para un video largo de verdad hay que **bajar la tasa**, y el 87,5 % de
  los bytes son datos literales de planos. Comprimirlos mas cuesta CPU, y
  el peor delta ya esta en 83,6 ms de los 80 que hay. No hay margen.

**El material pesa mas que cualquier opcion.** btf es el peor caso
(accion real, camara en movimiento, pantalla entera cambiando). El anime
medido el 2026-09-11 daba 39 KB/s con 8 colores contra 63 KB/s de btf con
32: para el opening de 30 s de la meta original, la cuenta es otra.

---

## 2026-09-12 — Mas colores sin mas planos: franjas parciales con el Copper

**Pregunta de Az:** medilo hipoteticamente y mostrame como se veria.

### Cuantos MOVE entran (aritmetica, no medicion)

La ventana del Copper para cambiar colores esta **antes de DDFSTRT**, y por
eso **no la achican los planos**: con 5 planos el DMA de bitplanes usa 6 de
cada 8 slots, pero solo desde el color clock 56 en adelante.

| | color clocks |
|---|---|
| Linea PAL | 227 |
| Hasta DDFSTRT ($38 = 56) | 56 |
| − refresco de memoria (4 slots) | −4 |
| − audio, 2 canales | −2 |
| − el WAIT de la franja | −6 |
| − los 2 MOVE del doblado vertical (BPL1MOD/BPL2MOD) | −8 |
| **Queda** | **36** |

Un MOVE del Copper son 2 palabras y el Copper accede a memoria un ciclo de
cada dos: **4 color clocks por MOVE**. 36 / 4 = **9 MOVE**. El disco y los
sprites estan apagados, asi que sus 3 y 16 slots quedan libres.

Coincide con lo medido en el Hito 6, que es el unico ancla real: con 3
planos **7 colores entran de sobra y 15 no**. Conclusion hipotetica: **6
seguros, 8 optimistas**. Antes de implementarlo hay que medirlo con la
misma prueba de temporizacion que decidio las franjas.

### Cuanto se ganaria (simulado, `encoder/bandtest.c`)

Franja 0 con su paleta completa (la escribe el delta, hay vertical blank de
sobra) y cada franja siguiente hereda la anterior y cambia K colores. La
busqueda prueba las 31x31 combinaciones (entrada, color candidato) y se
queda con la que mas baja el error; el color 0 nunca se toca, porque es el
borde. Frames de btf, 160x96 logicos, 6 franjas de 16 filas:

| | t = 2 s | t = 9 s |
|---|---|---|
| 16 colores | 0,0297 | 0,0278 |
| **32 colores, una paleta (hoy)** | **0,0230** | **0,0217** |
| 64 EHB (6 planos) | 0,0209 | 0,0209 |
| 32 + franjas de 4 colores | 0,0208 | 0,0203 |
| 32 + franjas de 6 colores | 0,0195 | 0,0194 |
| **32 + franjas de 8 colores** | **0,0190** | **0,0186** |
| 32 + paleta entera por franja (no entra) | 0,0193 | 0,0184 |
| 64 libres (no existen en OCS) | 0,0184 | 0,0189 |

**Ocho colores por franja llegan al techo de la paleta entera por franja**,
le ganan a los 64 de EHB y empatan con 64 colores libres, que en OCS no
existen. Y a diferencia de EHB **no cuesta ni un byte de bitstream ni un
ciclo de decodificacion**: son 8 MOVE del Copper por franja.

Franjas mas finas dan mas (t = 9 s, 8 colores por franja): 8 filas 0,0179;
16 filas 0,0186; 32 filas 0,0199. Az ya eligio 16 filas en el Hito 6
mirando las costuras, asi que la tabla usa 16.

### El artefacto que encontro el preview, y su arreglo

Con las franjas, la pantalla del autocine (una zona lisa grande y clara) se
va a **rosa**. No es la busqueda: pasa igual con la paleta entera por
franja, o sea es **el cuantizador por franja**. Cuantizada sola, la franja
le da a esa zona un centroide distinto que el del frame entero, y el error
medio no lo castiga (muchos pixeles apenas corridos) pero el ojo lo ve
enseguida. Es el mismo trampa que el banding, al reves.

El encoder ya tiene el arreglo para las franjas de 3 planos: `--band-snap`
(un color a menos de F de uno de la paleta de referencia se vuelve ese
mismo). Con 0,04, lo medido:

| | t = 2 s | t = 9 s |
|---|---|---|
| una paleta (hoy) | 0,0230 | 0,0217 |
| 8 colores por franja, sin snap | 0,0190 | 0,0186 |
| **8 colores por franja, snap 0,04** | **0,0205** | **0,0198** |

El snap **cuesta un tercio de la ganancia y saca el rosa**: queda −11 % y
−9 % contra la paleta unica, todavia a la altura de EHB y sin el sexto
plano. Las imagenes estan en `workand_snap_t9.png` (hoy / franjas /
franjas + snap).

### Lo que esta simulacion NO dice

- Es por frame. En el formato la paleta viaja con el delta y cambia **por
  escena**, no por frame: la ganancia real va a ser algo menor.
- No simula costuras en movimiento, que es lo que hizo elegir franjas de 16
  filas en el Hito 6.
- Los 9 MOVE son aritmetica de slots, no una medicion en la maquina.

---

## 2026-09-12 — melissa.mp4: anime que llena la pantalla, y un bug del control de tasa

**Pedido de Az:** probar 32 colores con `melissa.mp4`, que es anime pero le
costaba con el metodo anterior.

**Por que le cuesta, y no es el anime:** la fuente es **4:3** (960x720,
23,976 fps). Con letterbox quedan **120 filas logicas activas** contra las
94 de `btf.mp4`, que es 16:9. Son **28 % mas de imagen por frame**, en bytes
y en milisegundos, antes de tocar un solo parametro. El material es facil
(el error de cuantizacion con 32 colores es 0,0204, mejor que el 0,0212 de
btf); lo caro es que usa toda la pantalla.

**Medido** (presupuesto real del disco, `--predict auto`):

| | error vs. fuente | peor delta | frames tarde | disco libre |
|---|---|---|---|---|
| 8 colores, franjas, 13 s *(el metodo anterior)* | 7,42 % | 67,0 ms | 3 | 324 KB |
| **16 colores, 13 s** | **4,84 %** | 74,9 ms | 7 | 98 KB |
| 32 colores, 11 s | **3,92 %** | 95,4 ms | **28, el peor por 4 VBL** | 53 KB |
| **32 colores, `--min-hold 3`, 13 s** | **6,30 %** | 100,7 ms | 3 | 85 KB |
| 16 colores, `--min-hold 3`, 16 s | 7,62 % | 75,0 ms | 3 | 113 KB |
| 32 colores, 13 s | 20,14 % | — | — | 19 KB |
| 16 colores, 16 s | 16,07 % | — | — | 9 KB |

**La lectura honesta:** con 120 filas activas, 32 colores **no entran a 13 s
con la cadencia de siempre**. Las dos salidas buenas son 16 colores a 13 s
(4,84 %, tiempos holgados) o 32 colores bajando a `--min-hold 3` (6,30 %,
8,3 imagenes por segundo). La fila de 3,92 % es la mejor del cuadro y
**igual no se recomienda**: 28 frames tarde, el peor por 4 VBL, es
tartamudeo visible. El peor delta, 95,4 ms, se pasa de los 80 que da
`--min-hold 2`.

**Bug encontrado midiendo: el control de tasa dejaba disco sin usar.** Con
32 colores a 10 s daba 9,31 % de error **sobrando 105 KB de disco**, peor
que el mismo video a 11 s. La causa: la busqueda binaria sobre el umbral de
perdida asume que **mas umbral = menos bytes**, y eso no es cierto — una
repeticion de mas o de menos cambia el buffer oculto de todo lo que sigue.
Cuando un umbral intermedio no entraba, la busqueda descartaba para siempre
todo lo que estaba por debajo, aunque ahi hubiera streams que entraban.

**Arreglo:** la busqueda sigue siendo binaria, pero ya no devuelve "el
umbral mas bajo que entro" sino **el mejor stream que entro**, comparando el
error que cada uno midio (`rc_try` en `encode.c`). No cuesta ni una
codificacion mas: son los mismos streams, mejor elegidos.

| melissa, 32 colores, 10 s | error vs. fuente |
|---|---|
| antes | 9,31 % |
| **despues** | **5,87 %** |

Sin cambios en btf (5,89 % antes y despues): ahi la busqueda ya caia bien.

---

## 2026-09-12 — Franjas parciales de paleta con 32 colores (formato v6)

**Implementado.** Cada franja **hereda** la paleta de la de arriba y cambia
como mucho `band_colors` entradas; el paquete lleva esos cambios como pares
(indice, color) y el reproductor los convierte en un `MOVE` del Copper cada
uno, en el borde horizontal de la primera linea de la franja. Las ranuras
que sobran van con indice 0 y se escriben como `MOVE` al registro `$01FE`,
que no hace nada. Con `band_colors = colores - 1` sale exactamente el
comportamiento de las franjas de 8 colores de antes.

Esto saca la restriccion de "franjas solo con 3 planos o menos": ahora hay
franjas con 32 colores.

**La cuenta de slots del Copper, corregida.** La entrada del dia decia 9
MOVE, contando desde el principio de la linea. Estaba mal medida la ventana:
el WAIT de la franja despierta en el **color clock 6**, no en el 0, y la
imagen empieza en el 64 (DIWSTRT $2C81 = 129 pixeles lores). Son 58 color
clocks, a 4 por MOVE = **14 MOVE**, de los cuales 2 se van en los modulos
del doblado vertical: **12 colores**, menos lo que roben el refresco y el
audio en esa ventana. El encoder usa 8, con margen.

**Y el dato que importa: el material no llega ni cerca de ese tope.** Con
la paleta heredada, la busqueda encuentra que cambiar mas de 6 colores por
franja ya no baja el error:

| | colores cambiados por franja |
|---|---|
| btf, franjas de 8 filas | 4,7 |
| btf, franjas de 16 filas | 5,8 |
| btf, franjas de 32 filas | 6,8 |
| melissa, franjas de 8 filas | 3,9 |

Con `--band-colors 16` o `24` el promedio se queda en 6,1: **la ventana del
Copper no es la restriccion**. Por eso no hizo falta medir el tope exacto
con el haz: esta lejos.

(Intento fallido, vale anotarlo: se hicieron discos de prueba con
`--band-colors 24` esperando ver el retraso en pantalla. No se ve, y no
porque el Copper llegue: los cambios **reales** van primero en la lista y el
relleno despues, asi que lo unico que llega tarde son los `MOVE` que no
hacen nada.)

**Lo que gana** (13 s, 5 planos, `--predict auto`):

| | error de cuantizacion | vs. fuente |
|---|---|---|
| btf, sin franjas | 0,0212 | 5,89 % |
| btf, franjas de 32 filas | 0,0206 | 5,78 % |
| btf, franjas de 16 filas | 0,0205 | 5,80 % |
| **btf, franjas de 8 filas** | **0,0201** | **5,77 %** |
| melissa, sin franjas | 0,0204 | 6,30 % |
| **melissa, franjas de 8 filas** | **0,0196** | **6,19 %** |

**Menos de lo que prometia la simulacion, y por una razon concreta.**
`bandtest` daba −9 % de error de cuantizacion sobre un frame suelto; lo real
es −5 %. La diferencia es que en el formato **la paleta es por escena, no
por frame**: el histograma de una franja a lo largo de toda una escena es
mucho mas parecido al de las otras franjas que en un frame solo, sobre todo
con la camara moviendose. Franjas mas finas (8 filas) recuperan algo.

**Cuesta casi nada:** cero bytes en los deltas, ~2 KB mas de paletas en todo
el video (los pares de las franjas), cero tiempo de decodificacion (lo hace
el Copper) y 160 bytes mas de copper list.

**Verificado byte a byte:**

- Decoder de referencia contra el encoder: los 324 frames coinciden, con
  franjas de 8, 16 y 32 filas.
- Disco de frame fijo en la Amiga (5 planos, 6 franjas, 8 colores por
  franja): framebuffer 25 600 bytes identico y **copper list 3444 bytes
  identico** al que arma el decoder de referencia. Tambien con
  `--band-colors 16` (3604 bytes) y `24` (3764).
- Regresion del Hito 6 (3 planos, franjas de 16 filas): el encoder anterior
  y el nuevo dan exactamente el mismo error de cuantizacion (0,0321) y el
  mismo error contra la fuente (7,53 %).

**Bug encontrado al verificar, y el que se agrego para que no vuelva.** La
primera version daba 105 frames de 324 distintos en el decoder. El motivo:
el `band-snap` del Hito 6 seguia corriendo **despues** de elegir los
cambios, y mover una entrada mas de las que el paquete puede llevar hace que
el reproductor muestre una paleta distinta de la que simulo el encoder. En
el camino que hereda, el snap va sobre las **candidatas**, no sobre la
paleta ya armada. Ademas, `put_packet` ahora **aborta** si una franja cambia
mas colores de los que entran, en vez de truncar en silencio.

---

## 2026-09-12 — Como ganar duracion: la ecuacion, y dos caminos cerrados

**Pregunta de Az:** hipoteticamente, como se gana mas tiempo de
reproduccion.

### La ecuacion

Hoy el reproductor carga todo antes de empezar, asi que la duracion es
`min(disco, RAM) / tasa`. Con 883 KB de disco, ~885 KB de RAM util (975 KB
libres menos 82 KB de framebuffers, copper lists, buffers de audio, rebote
y margen con 5 planos) y 61 KB/s, son **14,5 s**.

**Disco y RAM son casi iguales, y por eso hoy el streaming no daria nada.**
Si el reproductor leyera mientras reproduce, el limite pasa a ser que el
buffer no se vacie:

```
T = RAM / (tasa - lectura)          (con tasa > lectura)
```

Y ahi cada KB/s importa muchisimo, porque la resta esta en el
denominador. Tasas medidas sin perdida, 13 s, `--predict auto`, franjas de
8 filas:

| | tasa total |
|---|---|
| btf, 32 colores, `--min-hold 2` | 61,2 KB/s |
| btf, 32 colores, `--min-hold 3` | 50,8 KB/s |
| btf, 16 colores, `--min-hold 2` | 48,9 KB/s |
| melissa, 32 colores, `--min-hold 3` | 54,5 KB/s |

Duracion que sale de la ecuacion, con 885 KB de RAM:

| tasa | lectura 18 KB/s (trackdisk) | lectura 27 KB/s (trackloader propio) |
|---|---|---|
| 61 KB/s (32 colores, min-hold 2) | 20,6 s | **26,0 s** |
| 51 KB/s (32 colores, min-hold 3) | 26,8 s | **36,9 s** |
| 49 KB/s (16 colores) | 28,5 s | **40,2 s** |
| 40 KB/s (anime tranquilo) | 40,2 s | **68,1 s** |

Contra los 14,5 s de hoy. **El multiplicador mas grande no es el disco: es
la velocidad de lectura**, porque entra restando.

### Lo que hace falta para eso, en orden de rendimiento

1. **Trackloader propio con decodificacion MFM por Blitter (H13).** La
   lectura medida de trackdisk es 18,0 KB/s contra un techo fisico de
   28,2 KB/s (11 sectores x 512 x 5 vueltas/s): trackdisk deja el 36 % en
   el camino, y ademas decodifica MFM con la CPU, que es justo lo que no
   sobra. Con el Blitter la decodificacion de una pista (5632 bytes) cuesta
   unos pocos ms cada 200 ms: ~5 % de la maquina. **Sin esto el streaming
   no es viable**, no por los bytes sino por la CPU.
2. **Streaming mientras reproduce.** Sin el, la RAM es el techo y el disco
   mas grande no sirve de nada.
3. **Formato de pista propio (H8, Greaseweazle).** 12 sectores por pista
   son 964 KB (+9 %) y 13 son 1044 KB (+18 %), y ademas suben el techo
   fisico de lectura a 30,7 KB/s. Solo rinde junto con el streaming: sin
   el, la RAM tapa la ganancia.
4. **Multidisco.** Con streaming, el disco deja de ser el limite y la
   formula manda. **Ojo con el cambio de disco:** mientras el usuario
   cambia, no se lee, asi que hace falta buffer = tasa x segundos de
   cambio. A 61 KB/s, 10 segundos de cambio son 610 KB, el 70 % de la RAM.
   A 40 KB/s son 400 KB y entra comodo. **El multidisco solo funciona a
   tasas bajas.**

### Dos caminos cerrados, medidos

**1. Mascara de planos por columna: no sirve.** Hoy cada columna marcada
escribe todos los planos, cambien o no. Medido sobre btf con 5 planos:
**cambian 4,02 de 5** (el 19,7 % de los bytes literales escriben lo que ya
estaba). Una mascara de planos costaria 1 byte por columna para ahorrar
0,98: empate exacto. Con 3 planos cambian 2,28 de 3, mismo resultado. El
decoder de referencia ahora reporta esta cifra.

**2. Comprimir los literales: no hay CPU.** El 87 % del stream son bytes
literales de planos (la mascara de columnas es 5,1 %, el mapa de filas
0,3 %, las cabeceras 0,2 %, las paletas 0,4 %). Comprimirlos de verdad pide
entropia, y el peor delta ya esta en 83,6 ms de los 80 que hay. No hay
margen.

**Y uno barato que si sirve: menos filas activas.** Los bytes son
proporcionales a las filas activas. btf (16:9) usa 94 de 128; melissa (4:3)
usa 120, y por eso cuesta 28 % mas. Un recorte a 2,35:1 dejaria 68 filas:
**-28 % de tasa, +38 % de duracion**, gratis y sin tocar una linea de
codigo. Es una decision de encuadre, no de ingenieria.

---

## 2026-09-13 — Audio: la mezcla a mono borraba un elemento en contrafase

**Sintoma.** En house.mp4 Az no escuchaba el agudo caracteristico de la
intro en ninguna prueba: ni fib4, ni pcm8, ni ADPCM, ni a 16 kHz, ni sin
filtros. Tampoco en `oir_0`, que era la fuente sin codec.

**Causa.** El encoder pedia el audio a ffmpeg con `-ac 1`, que suma L+R.
Energia por banda de los primeros 6 s (dB relativos a fondo de escala):

| banda | L+R | L-R | L | R |
|---|---|---|---|---|
| 0-2 kHz | -27,1 | -38,3 | -26,7 | -27,0 |
| 2-4 kHz | -36,9 | -38,3 | -34,6 | -34,5 |
| 4-6 kHz | -42,5 | -43,6 | -40,4 | -39,7 |
| 6-8 kHz | -46,0 | -49,6 | -44,9 | -44,0 |

Arriba de 2 kHz la parte en contrafase es casi tan fuerte como la parte en
fase. **Prueba de escucha de Az** (6 s, sin codec ni filtros): no lo oye en
la suma; si lo oye en solo izquierdo, solo derecho, solo L-R y el estereo
original. El elemento esta en contrafase y la suma lo cancela.

**Decision.** `--audio-channel mix|left|right` (mix por defecto, como
antes). left/right usan `pan=mono|c0=c0` / `c0=c1`, por indice para que
left ande tambien con fuentes mono. No cuesta bytes ni CPU. Verificado: el
audio decodificado de un stream con `left` (ADPCM 16 kHz) correlaciona
0,983 con el canal izquierdo de la fuente arriba de 2 kHz, y 0,649 con la
suma.

Todas las pruebas de codec anteriores (fib4 vs ADPCM, frecuencia,
pre-enfasis) partian de la suma: siguen valiendo para la calidad general,
pero no explican lo que faltaba.

---

## 2026-09-13 — Formato v6 en la A500 real

Az grabo `delorean_32c_mh2_pcm11k.adf` y lo probo en su A500. Es el disco
que mas cosas nuevas junta desde el Hito 7: formato v6, 32 colores (5
planos), franjas parciales de paleta de 8 filas, copia del buffer visible
con el Blitter (`--predict auto`) y audio pcm8 a 11 015 Hz. Az no reporto
diferencias con WinUAE. Como en el Hito 7, no hay mediciones propias del
hardware real (tiempos, sincronia): la verificacion byte a byte sigue
siendo la de WinUAE y el decoder de referencia.

Con esto la hoja de ruta cambia de orden (`ROADMAP.md`, revision del
2026-09-13): la rama que no necesita hardware nuevo tiene base probada.

---

## 2026-09-13 — H22: el error contra la fuente, separado por causa

**Problema.** "% de pixeles a mas de 0,10 del original" mezclaba tres
cosas y no servia para elegir configuracion: en Doctor Who los discos sin
perdida marcaban 45-50 %.

**Regla.** Cada pixel que se ve mal se atribuye a una causa, en este orden:

1. **colores:** el frame cuantizado ideal de ese instante ya estaba a mas
   de 0,10 de la fuente. Faltan colores; ni un disco infinito lo arregla.
2. **imagen sostenida:** lo que se ve es exactamente el ideal del ultimo
   frame que se escribio en el buffer visible. La fuente se movio y la
   imagen no (repeticion, `--min-hold`, frame tarde).
3. **compresion:** lo demas. Perdida del delta o degradacion por tiempo.

Las tres suman el total, que no cambia. Es solo reporte del encoder: el
bitstream sale identico.

**Medido** (misma configuracion que los discos entregados):

| clip | total | colores | sostenida | compresion |
|---|---|---|---|---|
| Evangelion, 32c mh5, 22 s | 11,80 % | 1,00 % | 10,42 % | 0,37 % |
| Doctor Who, 8c mh6, 27 s | 44,96 % | 4,16 % | **40,80 %** | 0,01 % |
| Tren 6:29, 32c mh4, 16 s | 25,82 % | 0,48 % | 21,04 % | 4,29 % |

Confirma lo que se sospechaba mirando: en estos clips casi todo el error es
temporal, y 32 colores ya casi no deja error de color (0,5-1 %).

---

## 2026-09-13 — H23: el reloj arranca con el primer frame ya dibujado

**Problema.** El reproductor fijaba el VBL del frame 0 (`V_START`) dos VBL
despues de tomar el hardware y recien entonces dibujaba el frame 0. Con 32
colores y la pantalla entera eso son ~100 ms (5 VBL): en casi todos los
discos de esta semana el frame 0 llegaba 2-4 VBL tarde, y como es un corte
de escena, a veces el encoder lo degradaba.

**Cambio.** El reproductor dibuja el primer delta con el reloj parado
(`V_START` = 0) y recien despues llama a `start_clock`, que fija `V_START`
dos VBL adelante y arranca el audio en ese VBL. El encoder simula igual: el
frame 0 no tiene apuro ni llenados de audio encima, y el frame 1 empieza a
dibujarse en el VBL del 0. La reconstruccion de `a500vp-dec --measure`
tambien. **El formato no cambia**; la espera antes del video crece lo que
tarda en dibujarse un frame (~0,1 s).

**Medido.** Prediccion del encoder, antes y despues, con presupuesto de
disco:

| clip | antes | despues |
|---|---|---|
| Evangelion 10 s, 32c mh5 | 1 tarde, frame 0 por 2 VBL | **0 tarde** |
| melissa 16 s, 32c mh3 | 5 tarde, frame 0 por 3 VBL, 3 degradados | 4 tarde, el peor por 1 VBL, 2 degradados; 5,47 % -> 5,39 % |

**Verificado en WinUAE** con el disco de medicion (`build.ps1 play`,
Evangelion 10 s, 32c mh5, sin perdida): 500 VBL para 500 esperados, **0
frames tarde** (la reconstruccion tambien da 0), el audio arranca 0,12 ms
despues del VBL del frame 0, y los CRC de los dos framebuffers al final
son identicos a los del decoder de referencia.

---

## 2026-09-13 — H20: audio automatico (canal, ganancia y formato)

Tres ajustes que se hicieron a mano en los videos de esta semana pasan a
ser el comportamiento por defecto. Los tres se pueden forzar como antes
(`--audio-channel mix|left|right`, `--audio-gain F`, `--audio-format
fib4|pcm8|adpcm`).

**Canal (`--audio-channel auto`).** Se lee la fuente en estereo a 16 kHz,
se pasa por un pasaaltos de 2 kHz (biquad de segundo orden) y se compara la
energia de (L+R)/2 con la de (L-R)/2. Si la contrafase esta a menos de 5 dB
de la fase, se usa un solo canal: el que tenga mas agudos. El umbral sale
de los casos medidos a mano:

| clip | contrafase bajo la fase | decision | a mano habia hecho falta |
|---|---|---|---|
| house | 3,8 dB | un canal (derecho) | si (izquierdo; Az oia el agudo en los dos) |
| See You in 30 Years | 3,6 dB | un canal (izquierdo) | si |
| Evangelion | 6,4 dB | mezcla | no |
| delorean | 16,5 dB | mezcla | no |
| Caniggia | 21,7 dB | mezcla | no |

(Las cifras difieren un poco de las del 2026-09-13 porque aquellas eran
por banda de 2-4 kHz y estas son todo lo que esta arriba de 2 kHz.)

**Ganancia (`--audio-gain auto`).** fib4 satura por pendiente: cuanto mas
fuerte, peor. Se prueban 1, 0,85, 0,7, 0,6, 0,5, 0,42, 0,35, 0,3 y 0,25, y
se queda con la mas fuerte que este a menos de 1 dB del mejor SNR (bajar
tambien baja el volumen; muy abajo el SNR vuelve a caer por el redondeo a
8 bits). pcm8 y adpcm mejoran con el volumen: pico a -1 dB, hasta 4x.

| clip | ganancia elegida | SNR fib4 con 1,0 | SNR elegido |
|---|---|---|---|
| house | 1,00 | 22,9 | 22,9 |
| Evangelion | 1,00 | 18,2 | 18,2 |
| See You in 30 Years | 0,70 | 14,8 | 17,5 |
| Caniggia (22,5 s) | 0,35 | ~9 | 19,8 |
| delorean | 0,35 | 8,0 | 15,6 |

**Formato (`--audio-format auto`).** fib4, salvo que despues del control
de tasa el video ya este **sin perdida** (el stream elegido no tiene mas
error que el de umbral 0) y el disco que sobra alcance para pcm8. Entonces
se rearma el stream con pcm8 al mismo umbral y se acepta si entra y el
error del video no sube mas de 0,1 % (pcm8 cuesta menos CPU que fib4, asi
que la linea de tiempo solo puede mejorar). Medido:

| clip | antes | con auto |
|---|---|---|
| See You in 30 Years, 16c mh3 | fib4 17,5 dB, sobraban 102 KB | **pcm8 34,3 dB**, sobran 34 KB, mismo video |
| delorean, 32c mh2 | fib4 15,6 dB, sobraban 119 KB, 14 tarde | **pcm8 35,2 dB**, sobran 76 KB, 12 tarde |
| house, 32c mh4 26 s | fib4, sobraban 43 KB | fib4 (pcm8 no entra) |

Los cinco pasan la verificacion del decoder de referencia (frames y
muestras de audio identicas).

---

## 2026-09-13 — H19: frames repetidos de la conversion de la fuente

**Que hace.** Con `--source auto` (por defecto), despues de decodificar y
escalar la fuente se buscan frames repetidos a intervalos regulares y se
sacan; la frecuencia de la fuente se recalcula con los que quedan, asi la
duracion y el audio no cambian. `--source raw` lo apaga.

**Deteccion.** Diferencia media absoluta entre frames consecutivos en el
area activa. Un frame es candidato si su diferencia esta bajo el 10 % de la
mediana (entre 0,05 y 0,3). Hay patron si la mediana del intervalo entre
candidatos es 4 o mas y el 70 % de los intervalos cae a +-1 de ella. El
anime animado en dos o en tres repite cada 2-3 frames y las imagenes quietas
dan intervalos de 1: no pasan. Con el patron reconocido se lo sigue ciclo
por ciclo: en cada uno se saca el frame mas parecido a intervalo +-1 del
ultimo, si su diferencia esta bajo el 25 % de la mediana; si en un ciclo no
hay ninguno (corte de escena), el patron sigue corriendo.

**Dos intentos descartados:**

1. `decimate=cycle=N` de ffmpeg con deteccion por fase fija. Sirve para 5
   y 6, pero 23,976 -> 25 repite cada **24,4** frames: la fase se corre y el
   tren no se detectaba (y decimate habria sacado frames buenos).
2. Umbral fijo para sacar los repetidos: se perdia algunos por el ruido de
   compresion (Caniggia 94 de 100, whoo 101 de 124). Seguir el patron ciclo
   por ciclo los encuentra todos.

**Medido** (fuentes crudas):

| fuente | detectado | sacados | fps resultante |
|---|---|---|---|
| Caniggia, 30 fps | cada 6, 97 % regular | 99 de 600 | 25,050 |
| el tren, 25 fps | cada 24, 93 % regular | 16 de 400 | 24,000 |
| BTTF III, 29,97 fps | cada 5, 100 % regular | 84 de 420 | 23,976 |
| whoo, 25 fps | cada 5, 84 % regular | 123 de 619 | 20,032 |
| house, melissa, See You in 30 Years, delorean, Doctor Who | nada | 0 | sin cambio |
| Evangelion, 59,94 fps | nada: aviso de posible interpolacion | 0 | sin cambio |

whoo no estaba en la lista de sospechosos: verificado con `tblend` sobre la
fuente, repite de verdad un frame cada 5 (0,02-0,28 contra 2-11 del
resto). Era una animacion de 20 fps subida a 25.

**Lo que mejora, y lo que no.** El "% de pixeles lejos de la fuente" no lo
ve, porque compara contra el frame de la fuente de ese instante, y si ese
es un repetido, mostrarlo congelado no cuenta como error (Caniggia 16c mh3
22,5 s: 9,42 % crudo, 9,78 % corregido; whoo: 12,64 % contra 12,68 %). Lo
que arregla es el **ritmo**. Con `--min-hold 1`, 3 colores, sin perdida,
repeticiones en pantalla:

| fuente | cruda | corregida | lo que corresponde |
|---|---|---|---|
| Caniggia 20 s | 15 (congelamientos: tirones) | **0** | 0 (25 fps en 24,96) |
| BTTF III 14 s | 25, mal repartidas | **10** | 10 (23,976 en 24,96) |
| whoo 24,8 s | 1 (los repetidos iban como deltas casi vacios) | 122 | ~124 (20 en 24,96) |

En whoo el ritmo en pantalla es el mismo en los dos casos (los repetidos de
la fuente ya estaban parejos); lo que se ahorra son los deltas de los
repetidos.

**Nombres con Unicode.** En la misma tanda: el nombre del archivo del tren
lleva `｜` (U+FF5C) y el encoder no lo podia abrir (argv y `_popen` usan la
pagina de codigos ANSI). Ahora `a500vp-enc` y `a500vp-dec` toman argv en
UTF-16 y lo pasan a UTF-8, y los pipes y archivos se abren con `_wpopen` y
`_wfopen`. Verificado con el archivo original del tren.

---

## 2026-09-13 — H21: `--auto`, la busqueda de configuracion

**Que hace.** Lanza el propio encoder 15 veces (32, 16 y 8 colores x
`--min-hold` 2 a 6) en paralelo, de a `NUMBER_OF_PROCESSORS - 1`, cada uno
con su salida a un archivo temporal y el presupuesto del disco final. Lee
de cada salida el error contra la fuente y su reparto (H22), los frames
tarde, el audio (H20) y el margen, muestra la tabla y codifica la elegida
con las salidas pedidas. Todas las demas opciones pasan tal cual; si no se
dio `--band-rows`, usa 8 con 16 y 32 colores y 16 con 8.

**Criterio.** Entre las que entran y no pasan de `--max-late` VBL de
atraso, la de menos pixeles lejos de la fuente; pero si una con mas colores
esta a menos de **2 puntos**, esa. La tolerancia sale de las elecciones de
Az: con house eligio 32 colores con `--min-hold 4` (8,89 %) sobre 16 colores
con `--min-hold 3` entero (7,62 %), o sea 1,3 puntos por mas colores. Con 1
punto, en "See You in 30 Years" elegia 8 colores (12,12 %) sobre 16 (14,15 %).
Es una preferencia, no una medicion: la tabla queda impresa para decidir
otra cosa.

**Medido.** Con 16 nucleos, las 15 configuraciones de un clip de 17 s mas la
codificacion final tardan **22 s**. Los resultados coinciden con los
barridos a mano de los dias anteriores (mismas cifras de error y de margen
para las mismas configuraciones), y el disco elegido pasa la verificacion
del decoder de referencia.

| clip | elegida | la de menos error |
|---|---|---|
| See You in 30 Years 7:45, 17 s | 8 colores mh2, 12,12 % (con tolerancia de 1 punto) | la misma |
| whoo entero | 32 colores mh4, 14,52 % | 16 colores mh3, 12,68 % |

---

## 2026-09-15 — Pixeles fantasma, granulado y cortes degradados

Az: "pixeles que se quedan del frame anterior que arruinan la escena", sobre
todo en Caniggia, "pero aparece en todos". Tres causas distintas, medidas
por separado.

### 1. Los fantasmas vienen del cuantizador, no de la compresion

**Metodo.** Se volco un mismo frame (Caniggia 32 colores mh4, frame 225) en
cada etapa: fuente, ideal cuantizado y buffer visible. **El jugador ya esta
a medio borrar en el ideal**, antes de comprimir. Con `--stability 0` el
ideal sale limpio.

La histeresis del cuantizador deja el indice del frame anterior si su color
esta a menos de `--stability` (0,07) del mejor. El margen es contra la
paleta y no mira si la fuente cambio: un objeto que se mueve sobre un fondo
parecido deja su color pegado. Esos pixeles casi nunca pasan de 0,10 contra
la fuente, asi que H22 los cuenta poco ("por colores").

**Lo que cuesta sacarlos** (Caniggia 20 s, 32 colores mh4, bytes al umbral
0,012; el presupuesto es 889 KB):

| variante | bytes | resultado |
|---|---|---|
| histeresis 0,07 (lo de siempre) | 880 KB | fantasmas en el ideal |
| sin histeresis | 1356 KB | ideal limpio |
| histeresis 0,03 | 1029 KB | ideal limpio, algun resto tenue |
| ancla: solo si la fuente cambio menos de 0,04 (promedio 3x3) | 1079 KB | quedan estelas de jugadores de 2 px |
| ancla doble (promedio 0,025 + pixel 0,05) | 1206 KB | limpio |
| refresco forzado de pixeles que quedan mal 2 deltas | — | peor: umbral 0,19, 9,8 % de compresion |
| denoise 4-8 + histeresis 0,03 | 989-1004 KB | ahorra 2-4 % |

Las anclas y el refresco no ganaron nada frente a bajar la histeresis y se
descartaron (no quedaron en el codigo).

**Conclusion.** El fantasma es la forma que toma la falta de espacio: con
el ideal limpio el video no entra, el control de tasa sube el umbral y la
compresion deja sus propios restos (arcos del circulo central durante un
paneo). Lo que lo arregla es liberar bytes: imagen mas chica (probado con
la fuente escalada y con borde negro), mas `--min-hold` o menos colores.
Con histeresis 0,02-0,03 y la imagen al 60-80 % entraron limpios Caniggia
(25 s mh2 al 60 %), el tren, btf, bttf3, House (27,35 s mh3 al 75 %) y
Fringe. El valor por defecto de `--stability` sigue en 0,07 hasta medirlo
en todos los clips: en clips quietos (Fringe) casi no deja fantasmas.

### 2. El umbral subido se ve granulado y H22 no lo ve

Fringe al 90 %, 32 colores mh2: "de esos" marcaba **0,44 % por
compresion** y Az lo vio "mucho mas pixelado". El control de tasa habia
subido el umbral a **0,0645**: todos los errores que deja quedan bajo 0,10,
el limite de H22. El error medio contra el ideal lo delata: 0,0148 contra
0,0028 del mismo clip al 80 % (umbral normal).

Revision de los 24 discos del dia (recodificados, identicos byte a byte):
6 habian subido el umbral (0,030 a 0,068) con 0,1-1 % "por compresion".

**Decision.** El reporte muestra siempre `perdida : umbral` y avisa cuando
se subio; `--auto` lo muestra en la tabla y solo elige una configuracion
que lo subio si ninguna entra sin subirlo. El criterio de "se ve limpio"
es el umbral, no el porcentaje.

### 3. Los cortes de escena no se degradan

DeLorean con histeresis 0,02 y fib4 11 kHz: fib4 le cuesta mas CPU al
reproductor que pcm8, el corte del frame 199 no llegaba a tiempo y se
degrado (umbral x64): **dos frames de mosaico** con media escena anterior.

**Decision.** Un corte acepta `--max-late` + 4 VBL antes de degradarse
(el tope evita que un corte carisimo atrase todo). Ese atraso se informa
en una linea aparte (`cortes :`) para que `--auto` no descarte la
configuracion por eso. Medido: el mismo corte llega 3 VBL tarde y entero;
House 75 % mh3 sale identico byte a byte.

---

## 2026-09-15 — H26: el disco que sobra se lo lleva el audio

Az: "la idea es que nunca sobre nada en el disco". Con la imagen reducida y
el umbral normal, los discos quedaban con 30-100 KB sin usar: H20 pasa el
audio a pcm8 solo si entra a la misma frecuencia, y lo que sobra despues se
perdia.

**Decision.** Despues de H20, si el video entro sin subir el umbral y
sobra mas de 1 KB, se sube la frecuencia del audio sin cambiar el formato
hasta llenar el disco. Se rearma el stream y se acepta solo si entra, no
suma frames degradados y el error contra el ideal no sube mas de 0,1 %
(mas muestras son mas llenados de audio, que le quitan CPU al delta). Si
se dio `--audio-rate` o `--audio-period`, no se toca.

**Tope: 11025 Hz.** El pasabajos fijo de la A500 corta cerca de 5 kHz;
muestrear a mas de 11 kHz no agrega nada que se oiga en el hardware real.
Con el tope puede seguir sobrando disco: eso le toca a la imagen (`--size`,
pendiente).

**Medido** (con `--adf`, presupuesto 888832):

| clip | antes | despues |
|---|---|---|
| See30 68 % mh2 | pcm8 8 kHz, sobraban 55 KB | pcm8 11015 Hz, SNR 35,1 dB, sobran 4 KB |
| See30 72 % mh2 | fib4 8 kHz, sobraban 18 KB | fib4 9880 Hz, sobran 2 KB |
| House 80 % mh4 | fib4 8 kHz, sobraban 90 KB | fib4 11015 Hz (tope), sobran 48 KB |
| Fringe 90 % mh2 (umbral subido) | sin cambio | sin cambio |
| DeLorean 85 % con `--audio-rate 11000` | sin cambio | sin cambio |

Los cinco pasan la verificacion del decoder de referencia.

---

## 2026-09-15 — H27: `--size` y tamanos en `--auto`

**Por que.** Achicar la imagen fue lo unico que saco los fantasmas y el
granulado sin perder colores ni fluidez (ver la entrada de fantasmas). Se
hacia a mano: escalar la fuente con ffmpeg, pegarle borde negro, bajar
`--scene-threshold` en proporcion al area y corregir cada porcentaje del
reporte. Az: "la idea es mejorar calidad de imagen/tamano y despues y solo
despues audio".

**`--size P`.** La imagen se escala al P % del cuadro, centrada con borde
negro (en el mismo filtro de ffmpeg que el letterbox). Las filas del borde
quedan fuera del area activa: no cuestan bytes, la copia con Blitter es mas
corta y las franjas de paleta se reparten solo sobre la imagen. Las columnas
del borde son negro exacto en la fuente y en el disco: se descuentan de la
deteccion de cortes, de la de frames repetidos (H19) y de todos los
porcentajes del reporte. Con borde se reserva el negro.

Verificado con See30 al 68 %: 22 escenas con el umbral normal (0,12) y
6,67 % contra la fuente, lo mismo que la correccion a mano de la fuente con
borde (7,0 % con 61 filas en vez de 64).

**`--auto`.** Suma `--size` 100, 90, 80, 70 y 60 (`--auto-sizes`) a colores
x `--min-hold`: 75 configuraciones. Criterio, en orden:

1. sirven las que entran sin subir el umbral y no pasan de `--max-late`;
2. entre las que estan a 2 puntos de la de menos error, la de mas colores,
   despues la mas grande, despues la de menos error;
3. con la elegida se prueba la imagen de a 2 % mas grande hasta el tamano
   siguiente de la lista, y queda la mas grande que sigue sirviendo;
4. recien ahi el audio (H20, H26) usa lo que sobra.

**Medido.** See30 (17 s, grano de pelicula, 16 nucleos): 75 + 4
configuraciones en **81 s**. Elegida 32 colores `--min-hold 2` al **72 %**
(a mano se habia llegado al 68 %); 74 % ya sube el umbral a 0,027. El audio
se llevo lo que sobraba: fib4 a 11 kHz, quedan 10 KB. Verificacion del
decoder de referencia OK.

| tamano, 32 colores mh2 | umbral | sirve |
|---|---|---|
| 80 % | 0,0707 | no |
| 78 % | 0,0625 | no |
| 76 % | 0,0547 | no |
| 74 % | 0,0272 | no |
| **72 %** | **0** | **si** |
| 70 % | 0 | si |

---

## 2026-09-10 — Pendiente de medir

- ~~Velocidad de lectura de trackdisk.~~ Medida en el Hito 4: 17,9 KB/s.
- ~~Costo de decodificacion por frame.~~ Medido y calibrado en el Hito 4.
- ~~WAIT horizontal del Copper para el doblado vertical.~~ Resuelto en el
  Hito 3 con el truco del modulo (ver arriba).
- ~~Sincronia de audio y costo del audio en la CPU.~~ Medidos en el Hito 5.
