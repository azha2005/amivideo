# A500VP

**English** · [Español](#espanol)

**Video with sound on an Amiga 500, from a single floppy disk.**

Turns any video FFmpeg can read into a bootable DD floppy. No AmigaDOS or
Workbench needed. Inspired by
[GBVideoPlayer2](https://github.com/LIJI32/GBVideoPlayer2).

```
video.mp4 ──► ffmpeg ──► a500vp-enc ──► disk.adf ──► A500 / WinUAE
                              │
                              └──► a500vp-dec ──► preview.mp4 + verification
```

- Up to **32 colours** at 320×256 PAL, with a per-band palette driven by the
  Copper.
- **Mono audio**, fib4 or pcm8, at Paula's exact rate, with no drift.
- **Delta coding** between two framebuffers, using the Blitter when it pays.
- The encoder **fills the disk exactly** and simulates the 68000's decode
  time (model measured in cycle-exact WinUAE).
- The reference decoder **verifies** every frame and the audio by CRC.
- Tested on a real A500 and in WinUAE with Kickstart 1.2.

**Hardware:** A500 PAL, OCS, 68000 at 7 MHz, 512 KB Chip + 512 KB slow (A501),
Kickstart 1.2 (1.3 intended too), DF0.

**What fits on one disk** (depends on motion and colours):

| video | length | colours | frames/s | audio |
|---|---|---|---|---|
| Anime opening | 22 s | 8 | 12.5 | fib4 8 kHz |
| Film, lots of motion | 12 s | 32 | 12.5 | pcm8 8 kHz |
| TV series intro | 27 s | 32 | 6.2 | fib4 8 kHz |
| TV series intro | 30 s | 16 | 8.3 | fib4 8 kHz |

Loading runs at ~18 KB/s: a full disk takes ~50 s before playback starts.

---

## Requirements (Windows, PowerShell)

| tool | used for |
|---|---|
| gcc from [MSYS2](https://www.msys2.org/) UCRT64 (`pacman -S mingw-w64-ucrt-x86_64-gcc`) | encoder and decoder |
| [vasm](http://sun.hasenbraten.de/vasm/) `vasmm68k_mot` (or `tools\get-vasm.ps1`) | player |
| FFmpeg on the PATH (`winget install Gyan.FFmpeg`) | reading video, previews |
| [WinUAE](https://www.winuae.net/) (optional) | testing without a floppy |

The repo neither includes nor downloads Kickstart ROMs: use your own.

## Building

```powershell
git clone https://github.com/azha2005/amivideo.git
cd amivideo
.\build.ps1        # or: .\build.ps1 -Gcc '...\gcc.exe' -Vasm '...\vasmm68k_mot.exe'
```

Leaves the encoder (`a500vp-enc.exe`), the decoder (`a500vp-dec.exe`) and the
Amiga binaries (`boot.bin`, `player.bin`) in `work\`.

> If PowerShell says *"running scripts is disabled"*:
> `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned` (once, no admin
> needed), or run each script with `powershell -ExecutionPolicy Bypass -File ...`.

---

## Usage

**Shortcut:** `.\build.ps1 disk -Video 'C:\Videos\opening.mp4' -Duration 22`
encodes, verifies and leaves `work\a500vp.adf` and `work\preview.mp4`
(8 colours, default settings).

### 1. Encode

`--auto` tries 32/16/8 colours, `--min-hold` 2 to 6 and image sizes from 100
down to 60 %, refines the size in 2 % steps and encodes the best one:

```powershell
.\work\a500vp-enc.exe --auto --in 'C:\Videos\my_video.mp4' `
  --start 44 --duration 12 --sharpen 0 `
  --out work\my_video.a5v --adf work\my_video.adf `
  --boot work\boot.bin --player work\player.bin
```

It discards candidates that run late or need a higher loss threshold (marked
`granulado`, grainy). Among those within 2 points of the lowest error, it
picks the most colours, then the largest image. Also check the error
breakdown (colours / held image / compression) and the preview.

To choose by hand, replace `--auto` with something like
`--planes 5 --min-hold 2 --predict auto --band-rows 8`, and check (the
encoder's output is in Spanish):

```
presupuesto: 888832 bytes, entra por 77068       ← must say "entra" (fits)
vs. fuente : error 0.0492; 6.66% de los pixeles  ← lower is better
tiempo real: ... el peor por 2 VBL               ← 2 or less is fine
```

### 2. Verify

```powershell
.\work\a500vp-dec.exe --in work\my_video.a5v --preview work\my_video_preview.mp4
```

It must print `VERIFICACION: OK`. The preview is exactly what the Amiga will
show, except for the A500's low-pass filter (the real thing sounds a bit
duller).

### 3. Play

- **WinUAE:** open `a500vp.uae`, pick your Kickstart and put the `.adf` in DF0.
- **Real Amiga:** write the `.adf` to a DD floppy (Greaseweazle, ADF-Copy or
  an ADF copier on the Amiga itself) and boot it.

### Scripts in `tools\`

They use `--sharpen 0 --stability 0.02 --max-late 4` and produce the `.adf`,
the preview and the verification.

**`max_duracion.ps1`**: the longest duration that fits cleanly (0.5 s
precision). If the whole clip fits and you didn't set `-Size`, it grows the
image.

```powershell
.\tools\max_duracion.ps1 -In 'C:\Videos\clip.mp4' -Name clip `
  -Start 10 -Planes 4 -Hold 3 -Size 80 -Extra '--audio-channel','left'
```

| parameter | meaning | default |
|---|---|---|
| `-Planes` | 3/4/5 = 8/16/32 colours | 5 |
| `-Hold` | 2/3/4 = 12.5/8.3/6.2 frames/s | 2 |
| `-Size` | image size, 30–100 | 60 (grows if there's room) |
| `-Start` | start second | 0 |
| `-Extra` | extra encoder options | — |

**`lote.ps1`**: encodes a list in `work\clips.psd1` with `--auto`:

```powershell
@{ Clips = @(
  @{ n = 'house';  in = 'C:\Videos\house.mp4';  x = @('--duration', '27.35') }
  @{ n = 'fringe'; in = 'C:\Videos\fringe.mp4' }
) }
```

`.\tools\lote.ps1` (or `-Solo house,fringe`) leaves `work\<n>_auto.adf`, its
preview, logs in `work\logs\` and a summary in `work\lote_resumen.txt`.

---

## Main options

Full list: `.\work\a500vp-enc.exe --help`.

| option | what it does |
|---|---|
| `--planes N` | 2–5 bitplanes (4–32 colours); default 3 |
| `--band-rows N` | palette bands of N rows; with 4–5 planes, `8` |
| `--min-hold N` | at most one new image every N×40 ms |
| `--size P` | image at P % with a black border; shrinking it pays off more than dropping colours |
| `--sharpen F` | edge enhancement (1.2); with 32 colours, `0` |
| `--stability F` | quantiser hysteresis (0.07); `0.02` if it fits, fewer ghosts |
| `--aspect M` | `letterbox`, `crop`, `stretch` |
| `--dither M` | `none`, `bayer2`, `bayer4` |
| `--predict auto` | uses the Blitter when it pays |
| `--start`, `--duration` | trim, in seconds |
| `--quality N` | allowed loss (0 = none); rises automatically if it doesn't fit |
| `--budget BYTES` | byte limit (with `--adf`, whatever is left on the disk) |
| `--max-late N` | VBLs of lateness tolerated per frame (2; scene cuts +4) |
| `--rate pal` | frames 1:1, everything 4 % faster |
| `--audio-format F` | `auto` (fib4, or pcm8 if there's room), `fib4`, `pcm8`, `none` |
| `--audio-rate HZ` | 8006 by default; rises up to 11 kHz if there's room |
| `--audio-channel C` | `auto`, `mix`, `left`, `right` |
| `--audio-gain F` | gain, or `auto` |
| `--source raw` | don't remove repeated frames from the source |

**Converted sources:** by default (`--source auto`) the encoder detects and
removes repeated frames from telecine and frame-rate conversions (e.g.
30 → 25.05 fps). Interpolated 50/60 fps sources can't be fixed: if a 24/25 fps
version exists, use that.

---

## How it works

1. **Encoder (C):** scales to 160×128 logical pixels, quantises in Oklab per
   band and emits deltas against the *simulated* state of both framebuffers,
   so loss never accumulates. Builds the complete `.adf`.
2. **Bootblock:** loads the player with `trackdisk.device`.
3. **Player (68000):** loads everything into slow + Chip RAM, stops the motor
   and takes over the hardware. Decodes into the hidden buffer and swaps on
   VBL; the Copper doubles lines and changes palettes; audio runs on the
   level 4 interrupt.
4. **Reference decoder (C):** does bit for bit what the player does.

Format: [`docs/FORMAT.md`](docs/FORMAT.md) (docs are in Spanish).

```
build.ps1          build, make disks, test in WinUAE
a500vp.uae         WinUAE config (faithful A500, no ROM)
encoder\           encoder, decoder and ADF writer (C11)
player\            bootblock and player (vasm)
tools\             get-vasm, shot, max_duracion, lote
docs\              FORMAT, DECISIONS, ROADMAP, SETUP
work\              output (ignored by git)
```

## Limitations

- PAL only.
- The whole video must fit in RAM: no streaming or multi-disk.
- The screen stays black when playback ends.
- `adpcm` exists in the encoder and decoder, but the player can't read it yet.

Made by Az, from the idea behind
[GBVideoPlayer2](https://github.com/LIJI32/GBVideoPlayer2) by LIJI32.

---

<a id="espanol"></a>

# A500VP (español)

[English](#a500vp) · **Español**

**Video con sonido en una Amiga 500, desde un solo disquete.**

Convierte cualquier video que lea FFmpeg en un disquete DD booteable. No
necesita AmigaDOS ni Workbench. Inspirado en
[GBVideoPlayer2](https://github.com/LIJI32/GBVideoPlayer2).

```
video.mp4 ──► ffmpeg ──► a500vp-enc ──► disco.adf ──► A500 / WinUAE
                              │
                              └──► a500vp-dec ──► preview.mp4 + verificación
```

- Hasta **32 colores** en 320×256 PAL, con paleta por franjas vía Copper.
- **Audio mono** fib4 o pcm8 a la frecuencia exacta de Paula, sin deriva.
- **Delta** entre dos framebuffers, con Blitter cuando conviene.
- El encoder **llena el disco exacto** y simula el tiempo de decodificación
  del 68000 (modelo medido en WinUAE cycle-exact).
- El decoder de referencia **verifica** cada frame y el audio por CRC.
- Probado en una A500 real y en WinUAE con Kickstart 1.2.

**Hardware:** A500 PAL, OCS, 68000 a 7 MHz, 512 KB Chip + 512 KB slow (A501),
Kickstart 1.2 (pensado también para 1.3), DF0.

**Qué entra en un disco** (depende del movimiento y los colores):

| video | duración | colores | img/s | audio |
|---|---|---|---|---|
| Opening de anime | 22 s | 8 | 12,5 | fib4 8 kHz |
| Película, mucho movimiento | 12 s | 32 | 12,5 | pcm8 8 kHz |
| Intro de serie | 27 s | 32 | 6,2 | fib4 8 kHz |
| Intro de serie | 30 s | 16 | 8,3 | fib4 8 kHz |

Carga a ~18 KB/s: un disco lleno tarda ~50 s en arrancar.

---

## Requisitos (Windows, PowerShell)

| herramienta | para qué |
|---|---|
| gcc de [MSYS2](https://www.msys2.org/) UCRT64 (`pacman -S mingw-w64-ucrt-x86_64-gcc`) | encoder y decoder |
| [vasm](http://sun.hasenbraten.de/vasm/) `vasmm68k_mot` (o `tools\get-vasm.ps1`) | reproductor |
| FFmpeg en el PATH (`winget install Gyan.FFmpeg`) | leer video, previews |
| [WinUAE](https://www.winuae.net/) (opcional) | probar sin disquete |

El repo no incluye ni descarga ROMs de Kickstart: usá la tuya.

## Compilar

```powershell
git clone https://github.com/azha2005/amivideo.git
cd amivideo
.\build.ps1        # o: .\build.ps1 -Gcc '...\gcc.exe' -Vasm '...\vasmm68k_mot.exe'
```

Deja en `work\` el encoder (`a500vp-enc.exe`), el decoder
(`a500vp-dec.exe`) y los binarios de la Amiga (`boot.bin`, `player.bin`).

> Si PowerShell dice *"running scripts is disabled"*:
> `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned` (una vez, sin admin),
> o corré cada script con `powershell -ExecutionPolicy Bypass -File ...`.

---

## Uso

**Atajo:** `.\build.ps1 disk -Video 'C:\Videos\opening.mp4' -Duration 22`
codifica, verifica y deja `work\a500vp.adf` y `work\preview.mp4`
(8 colores, configuración por defecto).

### 1. Codificar

Con `--auto` prueba 32/16/8 colores, `--min-hold` 2 a 6 y la imagen del 100
al 60 %, afina el tamaño de a 2 % y codifica la mejor:

```powershell
.\work\a500vp-enc.exe --auto --in 'C:\Videos\mi_video.mp4' `
  --start 44 --duration 12 --sharpen 0 `
  --out work\mi_video.a5v --adf work\mi_video.adf `
  --boot work\boot.bin --player work\player.bin
```

Descarta las que no llegan a tiempo o necesitan subir el umbral de pérdida
(marcadas `granulado`). Entre las que están a menos de 2 puntos de la de
menos error, elige la de más colores y después la más grande. Mirá también
el reparto del error (colores / imagen sostenida / compresión) y la preview.

A mano, reemplazá `--auto` por algo como
`--planes 5 --min-hold 2 --predict auto --band-rows 8`, y revisá:

```
presupuesto: 888832 bytes, entra por 77068       ← tiene que decir "entra"
vs. fuente : error 0.0492; 6.66% de los pixeles  ← cuanto menos, mejor
tiempo real: ... el peor por 2 VBL               ← 2 o menos está bien
```

### 2. Verificar

```powershell
.\work\a500vp-dec.exe --in work\mi_video.a5v --preview work\mi_video_preview.mp4
```

Tiene que decir `VERIFICACION: OK`. La preview es exactamente lo que se verá
en la Amiga, salvo el filtro pasabajos de la A500 (suena algo más apagado).

### 3. Reproducir

- **WinUAE:** abrí `a500vp.uae`, elegí tu Kickstart y poné el `.adf` en DF0.
- **Amiga real:** grabá el `.adf` en un DD (Greaseweazle, ADF-Copy o un
  copiador desde la Amiga) y booteá.

### Scripts en `tools\`

Usan `--sharpen 0 --stability 0.02 --max-late 4` y generan `.adf`, preview y
verificación.

**`max_duracion.ps1`**: la duración más larga que entra limpia (precisión de
0,5 s). Si el clip entra entero y no fijaste `-Size`, agranda la imagen.

```powershell
.\tools\max_duracion.ps1 -In 'C:\Videos\clip.mp4' -Name clip `
  -Start 10 -Planes 4 -Hold 3 -Size 80 -Extra '--audio-channel','left'
```

| parámetro | significado | defecto |
|---|---|---|
| `-Planes` | 3/4/5 = 8/16/32 colores | 5 |
| `-Hold` | 2/3/4 = 12,5/8,3/6,2 img/s | 2 |
| `-Size` | tamaño de imagen, 30–100 | 60 (crece si sobra) |
| `-Start` | segundo inicial | 0 |
| `-Extra` | opciones extra del encoder | — |

**`lote.ps1`**: codifica con `--auto` una lista en `work\clips.psd1`:

```powershell
@{ Clips = @(
  @{ n = 'house';  in = 'C:\Videos\house.mp4';  x = @('--duration', '27.35') }
  @{ n = 'fringe'; in = 'C:\Videos\fringe.mp4' }
) }
```

`.\tools\lote.ps1` (o `-Solo house,fringe`) deja `work\<n>_auto.adf`, su
preview, logs en `work\logs\` y un resumen en `work\lote_resumen.txt`.

---

## Opciones principales

Lista completa: `.\work\a500vp-enc.exe --help`.

| opción | qué hace |
|---|---|
| `--planes N` | 2–5 bitplanes (4–32 colores); defecto 3 |
| `--band-rows N` | paleta por franjas de N filas; con 4–5 planos, `8` |
| `--min-hold N` | una imagen nueva cada N×40 ms como mucho |
| `--size P` | imagen al P % con borde negro; achicarla rinde más que bajar colores |
| `--sharpen F` | realce de bordes (1.2); con 32 colores, `0` |
| `--stability F` | histéresis del cuantizador (0.07); `0.02` si entra, deja menos fantasmas |
| `--aspect M` | `letterbox`, `crop`, `stretch` |
| `--dither M` | `none`, `bayer2`, `bayer4` |
| `--predict auto` | usa el Blitter cuando conviene |
| `--start`, `--duration` | recorte en segundos |
| `--quality N` | pérdida permitida (0 = ninguna); sube sola si no entra |
| `--budget BYTES` | tope de bytes (con `--adf`, lo que queda en el disco) |
| `--max-late N` | VBL de atraso tolerados por frame (2; cortes de escena +4) |
| `--rate pal` | frames 1:1, todo 4 % más rápido |
| `--audio-format F` | `auto` (fib4, o pcm8 si sobra), `fib4`, `pcm8`, `none` |
| `--audio-rate HZ` | 8006 por defecto; sube hasta 11 kHz si sobra disco |
| `--audio-channel C` | `auto`, `mix`, `left`, `right` |
| `--audio-gain F` | ganancia o `auto` |
| `--source raw` | no sacar frames repetidos de la fuente |

**Fuentes convertidas:** por defecto (`--source auto`) el encoder detecta y
saca los frames repetidos de telecine y conversiones de frecuencia (p. ej.
30 → 25,05 fps). Las fuentes de 50/60 fps interpoladas no tienen arreglo:
si existe la versión a 24/25 fps, usá esa.

---

## Cómo funciona

1. **Encoder (C):** escala a 160×128 lógicos, cuantiza en Oklab por franja y
   genera deltas contra el estado *simulado* de los dos framebuffers, así no
   se acumula pérdida. Arma el `.adf` completo.
2. **Bootblock:** carga el reproductor con `trackdisk.device`.
3. **Reproductor (68000):** carga todo a slow + Chip RAM, apaga el motor y
   toma el hardware. Decodifica en el buffer oculto y cambia en el VBL; el
   Copper dobla líneas y cambia paletas; el audio va por la interrupción de
   nivel 4.
4. **Decoder de referencia (C):** hace bit a bit lo mismo que el reproductor.

Formato: [`docs/FORMAT.md`](docs/FORMAT.md).

```
build.ps1          compilar, generar discos, probar en WinUAE
a500vp.uae         config de WinUAE (A500 fiel, sin ROM)
encoder\           encoder, decoder y escritor de ADF (C11)
player\            bootblock y reproductor (vasm)
tools\             get-vasm, shot, max_duracion, lote
docs\              FORMAT, DECISIONS, ROADMAP, SETUP
work\              salida (ignorado por git)
```

## Limitaciones

- Solo PAL.
- Todo el video entra en RAM: sin streaming ni multidisco.
- Al terminar queda la pantalla negra.
- `adpcm` existe en encoder y decoder, pero el reproductor todavía no lo lee.

Hecho por Az, a partir de la idea de
[GBVideoPlayer2](https://github.com/LIJI32/GBVideoPlayer2) de LIJI32.
