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

## 2026-09-10 — Pendiente de medir

- **Velocidad de lectura de trackdisk.** `CLAUDE.md` estima 15–25 KB/s. Sin
  medir: no se toco todavia el cargador de datos. Importa mucho (a 20 KB/s,
  cargar 880 KB son ~44 s de espera antes de que empiece el video). Se mide en
  el Hito 4, cuando exista la carga de verdad.
- **Costo de decodificacion por frame.** Se mide en el Hito 4 y calibra el
  modelo de costo del encoder.
- **WAIT horizontal del Copper para el doblado vertical.** Hito 3.
