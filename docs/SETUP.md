# Entorno de desarrollo de A500VP

Todo verificado en la maquina de Az el 2026-09-10 (Windows 10, PowerShell).
No hace falta MSYS2 shell, ni `make`, ni nada POSIX: `build.ps1` llama a los
ejecutables directamente.

## Lo que hay instalado

| Herramienta | Version | Ruta |
|---|---|---|
| vasm (Motorola) | 1.9 / backend m68k 2.5 | `C:\Users\JC\vbcc\bin\vasmm68k_mot.exe` |
| vbcc + vlink | (junto con vasm) | `C:\Users\JC\vbcc\bin\` |
| gcc | 16.1.0 (MSYS2 UCRT64) | `C:\msys64\ucrt64\bin\gcc.exe` |
| ffmpeg / ffprobe | 9.0 full build | en el PATH (winget, Gyan.FFmpeg.Shared) |
| WinUAE | (64 bits) | `C:\Program Files\WinUAE\winuae64.exe` |
| Python | 3.x | en el PATH |

vasm y vbcc ya estaban instalados de otro proyecto de Amiga (`amiga260k`).
Se usan esos: no hace falta instalar nada mas.

**Compilador de C elegido: gcc de MSYS2/UCRT64.** No mezclar con clang ni con
MSVC. El encoder es C11 portable y no usa nada de MinGW en particular.

Si algun dia falta vasm, `tools\get-vasm.ps1` lo descarga del sitio oficial y
lo compila con gcc, sin permisos de administrador y sin `make`. Se comprobo
que el binario que produce (vasm 2.0f) genera exactamente los mismos bytes que
el vasm 1.9 de vbcc para `player\boot.s`.

## Kickstart

**Nunca se descargan ROMs.** Az puso la de su propia maquina en la raiz del
repo como `kick12.rom` (Kickstart 1.2, 33.180, 256 KB), y `.gitignore` la
excluye.

Es una 1.2, no una 1.3. Todo lo que usa el reproductor existe desde la 1.2, con
una excepcion importante anotada en `DECISIONS.md`: `MEMF_TOTAL` de `AvailMem`
es V36 (Kickstart 2.0) y no se puede usar.

## Comandos

```powershell
.\build.ps1              # ensambla, compila y genera work\a500vp.adf
.\build.ps1 run          # ademas lo arranca en WinUAE y te deja mirarlo
.\build.ps1 check        # lo arranca, lo cierra y te imprime la medicion
.\build.ps1 still        # Hito 3: frame fijo en WinUAE, verificado byte a byte
                         # (-Stream work\final22.a5v -Frame 182 por defecto)
.\build.ps1 play         # Hito 4: disco de medicion. Carga y reproduce el
                         # stream entero (~2,5 min), captura la ventana cada
                         # -ShotEvery s en work\shots\ y ajusta el modelo de
                         # costo con lo que midio la Amiga
.\build.ps1 clean        # borra work\

.\tools\shot.ps1         # arranca WinUAE y saca una captura a work\shot.png
```

Rutas pisables: `-Vasm`, `-Gcc`, `-WinUAE`, `-Rom`, `-Timeout`.

### Encoder y decoder de referencia

`build.ps1` compila tambien `work\a500vp-enc.exe` y `work\a500vp-dec.exe`.

```powershell
# bitstream de los primeros 22 s. Los defaults son los parametros elegidos
# en el Hito 2 (8 colores, realce 1.2, hold 2, histeresis 0.07) y el
# presupuesto por defecto ya descuenta el audio.
.\work\a500vp-enc.exe --in "C:\ruta\al\opening.mp4" --duration 22 `
    --out work\video.a5v

# reconstruirlo, verificarlo contra el encoder y sacar el preview con audio
.\work\a500vp-dec.exe --in work\video.a5v --preview work\preview.mp4 `
    --audio "C:\ruta\al\opening.mp4" --audio-duration 22
```

El decoder termina con `VERIFICACION: OK` si reconstruyo todos los frames
exactamente igual que los simulo el encoder, y sale con codigo 1 si no.
`a500vp-enc --help` lista todas las opciones.

## WinUAE

`a500vp.uae` esta en el repo con la maquina objetivo configurada: OCS, 68000
cycle-exact, PAL, 512 KB chip (`chipmem_size=1`) + 512 KB slow en $C00000
(`bogomem_size=2`), cero fast, una disquetera, `floppy_speed=100` (sin turbo).

Los campos `kickstart_rom_file=` y `floppy0=` estan **vacios a proposito**:
no se versiona ninguna ROM ni ningun ADF. `build.ps1` genera una copia en
`work\` con esos dos campos completos. Si abris el `.uae` a mano en la GUI,
elegi vos la ROM y el disquete.

Detalle importante que costo encontrar: si a WinUAE se lo mata con `Kill()`,
**no vuelca al `.adf` lo que la Amiga escribio en el disquete**. Hay que
cerrarlo por la ventana (`CloseMainWindow`) y esperar a que salga. `build.ps1`
ya lo hace asi.
