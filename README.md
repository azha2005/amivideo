# A500VP

**Video con sonido en una Amiga 500, desde un solo disquete.**

A500VP convierte cualquier video que lea FFmpeg en un disquete DD booteable.
Lo metés en la disquetera de una Amiga 500 de serie, esperás a que cargue y
se reproduce con imagen y audio sincronizados. No hace falta ni AmigaDOS ni
Workbench.

Está inspirado en [GBVideoPlayer2](https://github.com/LIJI32/GBVideoPlayer2),
que hace lo mismo para Game Boy.

```
video.mp4 ──► ffmpeg ──► a500vp-enc ──► disco.adf ──► A500 / WinUAE
                              │
                              └──► a500vp-dec ──► preview.mp4 + verificación
```

---

## Características

- **Hasta 32 colores** (5 bitplanes) en 320×256 PAL, con paleta por franjas
  horizontales que cambia el Copper.
- **Audio mono** en fib4 (4 bits, ~4 KB/s) o pcm8, a la frecuencia exacta de
  Paula. **Sin deriva:** en la medición llegaron 1100 VBL de 1100 esperados.
- **Compresión delta** entre dos framebuffers. Cuando conviene, se usa el
  Blitter para copiar el frame anterior.
- **Control de tasa exacto:** el encoder llena el disquete sin pasarse ni un
  byte.
- **Tiempo real simulado:** el encoder calcula cuánto tarda el 68000 en
  decodificar cada frame, con un modelo medido en WinUAE cycle-exact. Si un
  frame no llegaría a tiempo, le baja la calidad.
- **Verificación byte a byte:** el decoder de referencia reconstruye todo y
  compara el CRC de cada frame y del audio con lo que simuló el encoder.
- **Funciona en hardware real:** probado en una A500 real y en WinUAE con
  Kickstart 1.2.

## Hardware

| | |
|---|---|
| Máquina | Amiga 500 PAL, OCS, 68000 a 7 MHz |
| Memoria | 512 KB Chip + 512 KB slow (expansión A501) |
| Kickstart | 1.2 (probado); pensado también para 1.3 |
| Disquetera | DF0, disquete DD (880 KB) |

## Cuánto entra en un disquete

Depende del material: cuánto se mueve la imagen, cuántos colores pedís y
cada cuánto cambia la imagen. Algunos casos reales:

| video | duración | colores | imágenes/s | audio |
|---|---|---|---|---|
| Opening de anime | 22 s | 8 | 12,5 | fib4 8 kHz |
| Escena de película, mucho movimiento | 12 s | 32 | 12,5 | pcm8 8 kHz |
| Intro de serie | 27 s | 32 | 6,2 | fib4 8 kHz |
| Intro de serie | 30 s entera | 16 | 8,3 | fib4 8 kHz |

La carga lee a ~18 KB/s. Un disco lleno tarda unos 50 s en arrancar a
reproducir.

---

## Requisitos

Todo se hace en **Windows** con PowerShell.

| Herramienta | Para qué | Dónde conseguirla |
|---|---|---|
| gcc (MSYS2 UCRT64) | encoder y decoder en C11 | [msys2.org](https://www.msys2.org/) → `pacman -S mingw-w64-ucrt-x86_64-gcc` |
| vasm (`vasmm68k_mot`) | reproductor en ensamblador 68000 | [sun.hasenbraten.de/vasm](http://sun.hasenbraten.de/vasm/), o `tools\get-vasm.ps1` |
| FFmpeg | leer el video y generar previews | `winget install Gyan.FFmpeg`, tiene que quedar en el PATH |
| WinUAE (opcional) | probar sin grabar un disquete | [winuae.net](https://www.winuae.net/) |

> **Kickstart:** el repo no incluye ROMs ni las descarga. Para WinUAE usá la
> de tu propia Amiga.

## Compilar

```powershell
git clone https://github.com/azha2005/amivideo.git
cd amivideo
.\build.ps1
```

Si tus herramientas están en otro lado, pasale las rutas:

```powershell
.\build.ps1 -Gcc 'C:\msys64\ucrt64\bin\gcc.exe' -Vasm 'C:\vbcc\bin\vasmm68k_mot.exe'
```

Queda todo en `work\`:

| archivo | qué es |
|---|---|
| `a500vp-enc.exe` | el encoder |
| `a500vp-dec.exe` | el decoder de referencia |
| `boot.bin`, `player.bin` | el bootblock y el reproductor que van dentro del disco |

---

## Uso

### 1. Codificar

La forma fácil es `--auto`: prueba 32, 16 y 8 colores, `--min-hold` de 2
a 6 y la imagen al 100, 90, 80, 70 y 60 % en paralelo, afina el tamaño de
a 2 %, muestra una tabla y codifica la elegida. Primero va la imagen: el
audio solo mejora con lo que sobra.

```powershell
.\work\a500vp-enc.exe --auto --in 'C:\Videos\mi_video.mp4' `
  --start 44 --duration 12 --sharpen 0 `
  --out work\mi_video.a5v --adf work\mi_video.adf `
  --boot work\boot.bin --player work\player.bin
```

```
  colores  hold  img/s  tamano   error   (colores/sostenida/compresion)  umbral  tarde      audio        disco
       32     2   12.5     80%   7.71%  (0.03/ 5.89/1.79)  0.0707     7, 1 VBL  fib4 17.6 dB  sobran 0 KB granulado
       32     2   12.5     70%   6.70%  (0.02/ 6.67/0.00)  0.0000     8, 1 VBL  pcm8 35.0 dB  sobran 2 KB <- elegida
       32     3    8.3     80%  10.83%  (0.03/10.81/0.00)  0.0000     8, 1 VBL  pcm8 35.1 dB  sobran 16 KB

auto       : afinando el tamano de 72% a 78% con 32 colores y --min-hold 2
             72%: sirve, umbral 0.0000, 6.69%, sobran 10 KB
             74%: no sirve, umbral 0.0272, 6.70%, sobran 0 KB
auto       : elegida 32 colores con --min-hold 2 y --size 72
```

Sirven las que entran sin subir el umbral de pérdida y llegan a tiempo. Las
que tuvieron que subirlo salen marcadas `granulado`: su error parece chico,
pero los degradados se ven granulados. Entre las que sirven y están a menos
de 2 puntos de la de menos error, gana la de más colores y después la más
grande. El error se mide sobre la imagen, sin el borde. El número no lo dice
todo: mirá el reparto (cuánto error viene de los colores, cuánto de sostener
la imagen y cuánto de la compresión) y la preview.

Para elegir a mano:

```powershell
.\work\a500vp-enc.exe --in 'C:\Videos\mi_video.mp4' `
  --start 44 --duration 12 `
  --planes 5 --min-hold 2 --sharpen 0 --predict auto --band-rows 8 `
  --out work\mi_video.a5v --adf work\mi_video.adf `
  --boot work\boot.bin --player work\player.bin
```

En la salida, fijate en tres líneas:

```
presupuesto: 888832 bytes, entra por 77068       ← tiene que decir "entra"
vs. fuente : error 0.0492; 6.66% de los pixeles  ← cuanto menos, mejor
  de esos  : 0.5% por colores, 6.1% por imagen sostenida, 0.1% por compresion
tiempo real: ... el peor por 2 VBL               ← 2 o menos está bien
```

### 2. Verificar y ver la preview

```powershell
.\work\a500vp-dec.exe --in work\mi_video.a5v --preview work\mi_video_preview.mp4
```

Tiene que decir `VERIFICACION: OK`. La preview muestra exactamente lo que va
a salir en la Amiga, con el audio ya decodificado. Solo le falta el filtro
pasabajos de la A500, así que en la máquina real suena un poco más apagado.

### 3. Reproducir

- **WinUAE:** abrí `a500vp.uae`, elegí tu Kickstart y poné el `.adf` en DF0.
- **Amiga real:** grabá el `.adf` en un disquete DD (con Greaseweazle,
  ADF-Copy, o desde la misma Amiga con un copiador de ADF) y booteá.

### Atajo

`build.ps1 disk` hace los tres pasos juntos con la configuración por defecto
(8 colores, los primeros 22 s):

```powershell
.\build.ps1 disk -Video 'C:\Videos\opening.mp4' -Duration 22
# -> work\a500vp.adf y work\preview.mp4
```

---

## Opciones

### Imagen

| opción | qué hace | consejo |
|---|---|---|
| `--planes N` | 2 a 5 bitplanes = 4, 8, 16 o 32 colores (por defecto 3) | 5 se ve mucho mejor y cuesta más bytes |
| `--band-rows N` | paleta por franjas de N filas lógicas | con 4 o 5 planos usá `8` |
| `--min-hold N` | una imagen nueva como mucho cada N×40 ms | 2 = 12,5/s, 3 = 8,3/s, 4 = 6,2/s. Subirlo alarga el video |
| `--sharpen F` | realce de bordes (por defecto 1.2) | con 32 colores, `0` |
| `--stability F` | histéresis del cuantizador (por defecto 0.07): ahorra bytes en el ruido, pero deja fantasmas de lo que se mueve | `0.02` si sin subir el umbral entra |
| `--aspect MODO` | `letterbox`, `crop` o `stretch` | letterbox además ahorra bytes |
| `--size P` | la imagen al P % del cuadro, centrada con borde negro (por defecto 100) | si se ve granulado o con fantasmas, achicarla rinde más que bajar colores; `--auto` la busca sola |
| `--dither MODO` | `none`, `bayer2` o `bayer4` | ordenado y estable; nunca difusión de error |
| `--predict auto` | usa el Blitter cuando conviene | dejalo así |

### Tiempo y tasa

| opción | qué hace |
|---|---|
| `--start S`, `--duration S` | recorte de la fuente, en segundos |
| `--quality N` | pérdida permitida; 0 = sin pérdida. Si no entra, el control de tasa la sube solo y avisa (`perdida : umbral`): se ve granulado |
| `--budget BYTES` | tope de bytes; con `--adf` es lo que queda en el disco |
| `--max-late N` | VBL de atraso tolerados antes de bajarle la calidad a un frame (por defecto 2; los cortes de escena aguantan 4 más) |
| `--rate pal` | frames 1:1, todo 4 % más rápido, como en la TV PAL |

### Audio

| opción | qué hace | consejo |
|---|---|---|
| `--audio-format F` | `auto`, `fib4` (4 bits), `pcm8` (8 bits) o `none` | `auto` (por defecto) usa fib4, y pcm8 si el video entra sin pérdida y sobra disco |
| `--audio-rate HZ` | frecuencia aproximada; se usa el período entero de Paula más cercano | 8006 por defecto, y si sobra disco sube sola hasta 11 kHz para no dejarlo sin usar; dándola, se respeta |
| `--audio-channel C` | `auto`, `mix`, `left` o `right` | `auto` (por defecto) usa un solo canal si la mezcla borraría lo que está en contrafase |
| `--audio-gain F` | ganancia antes de pasar a 8 bits, o `auto` | `auto` (por defecto): fib4 busca el mejor SNR, pcm8 lleva el pico a −1 dB |
| `--no-audio` | sin audio | |

La lista completa sale con `.\work\a500vp-enc.exe --help`.

---

## Fuentes convertidas de otra frecuencia

Muchos videos traen frames repetidos porque alguien los convirtió de una
frecuencia a otra: telecine (29,97 desde 24), PAL subido a 30, película de
24 pasada a 25, animación de 20 fps subida a 25. El encoder los detecta
solo y los saca antes de codificar (`--source auto`, por defecto):

```
fuente     : 1 frame repetido cada 6 (97% de los intervalos regulares):
             se sacan 99 de 600, 30.000 -> 25.050 fps
```

Si por algún motivo no querés que toque la fuente, usá `--source raw`.

Las fuentes de 50 o 60 fps **interpoladas** no tienen repetidos y no tienen
arreglo automático: el encoder avisa. Si existe la versión original a 24 o
25 fps, conviene usar esa.

Los nombres de archivo con caracteres Unicode (como los `｜` que ponen
yt-dlp y otros) funcionan directo.

---

## Cómo funciona

1. **Encoder (C).** FFmpeg decodifica el video. El encoder escala a 160×128
   lógicos, cuantiza en Oklab a la paleta de cada franja y genera deltas
   contra el estado *simulado* de los dos framebuffers de la Amiga. Así
   ninguna pérdida se acumula. Después arma el `.adf` entero: bootblock,
   reproductor y datos.
2. **Bootblock.** Lee el reproductor con `trackdisk.device` y salta a él.
3. **Reproductor (68000).** Carga todo el video a RAM, repartido entre slow
   RAM y Chip RAM, apaga el motor y toma el hardware. Cada frame se escribe en
   el framebuffer oculto y se intercambia en el vertical blank. El Copper dobla
   las líneas y cambia la paleta de cada franja. El audio se decodifica por
   bloques en la interrupción de nivel 4.
4. **Decoder de referencia (C).** Hace bit a bit lo mismo que el reproductor.
   Sirve para ver el resultado y para detectar bugs sin tocar ensamblador.

El formato del bitstream está definido en [`docs/FORMAT.md`](docs/FORMAT.md).

## Estructura

```
build.ps1          compilar, generar discos, probar en WinUAE
a500vp.uae         configuración de WinUAE: A500 fiel, sin ROM
encoder\           encoder, decoder de referencia y escritor de ADF (C11)
player\            bootblock y reproductor (ensamblador 68000, vasm)
tools\             get-vasm.ps1, shot.ps1
docs\FORMAT.md     formato del bitstream (la única definición)
docs\DECISIONS.md  decisiones de diseño y mediciones, con fecha y método
docs\ROADMAP.md    lo que sigue
docs\SETUP.md      entorno de desarrollo en detalle
work\              salida: discos, previews, binarios (ignorado por git)
```

## Limitaciones

- Solo PAL.
- Todo el video tiene que entrar en RAM: nada de streaming ni multidisco,
  por ahora.
- Al terminar queda la pantalla negra; no vuelve al sistema operativo.
- El formato `adpcm` ya lo generan el encoder y el decoder de referencia,
  pero el reproductor todavía no lo decodifica.

## Créditos

Hecho por Az. La idea viene de
[GBVideoPlayer2](https://github.com/LIJI32/GBVideoPlayer2), de LIJI32.
