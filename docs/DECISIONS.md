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

## 2026-09-10 — Pendiente de medir

- **Velocidad de lectura de trackdisk.** `CLAUDE.md` estima 15–25 KB/s. Sin
  medir: no se toco todavia el cargador de datos. Importa mucho (a 20 KB/s,
  cargar 880 KB son ~44 s de espera antes de que empiece el video). Se mide en
  el Hito 4, cuando exista la carga de verdad.
- **Costo de decodificacion por frame.** Se mide en el Hito 4 y calibra el
  modelo de costo del encoder.
- ~~WAIT horizontal del Copper para el doblado vertical.~~ Resuelto en el
  Hito 3 con el truco del modulo (ver arriba).
