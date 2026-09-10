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

## 2026-09-10 — Pendiente de medir

- **Velocidad de lectura de trackdisk.** `CLAUDE.md` estima 15–25 KB/s. Sin
  medir: no se toco todavia el cargador de datos. Importa mucho (a 20 KB/s,
  cargar 880 KB son ~44 s de espera antes de que empiece el video). Se mide en
  el Hito 4, cuando exista la carga de verdad.
- **Costo de decodificacion por frame.** Se mide en el Hito 4 y calibra el
  modelo de costo del encoder.
- **WAIT horizontal del Copper para el doblado vertical.** Hito 3.
