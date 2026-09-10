# A500VP: reproductor de video para Amiga 500 desde un disquete

## Contexto

Este proyecto es de Az. La idea está inspirada en GBVideoPlayer2 (https://github.com/LIJI32/GBVideoPlayer2): un encoder en PC que convierte cualquier video compatible con FFmpeg a un formato propio, y un reproductor en ensamblador que lo muestra en hardware real. Acá el objetivo es el mismo, pero para una Amiga 500.

**Meta concreta de la primera versión:** reproducir un opening de anime de ~30 segundos, con audio, desde **un solo disquete DD**, en una **A500 real**.

Hablale a Az en español. Los identificadores y comentarios del código pueden ir en inglés.

---

## Hardware objetivo (fijo)

- Amiga 500 PAL, chipset OCS, CPU 68000 a ~7 MHz.
- **512 KB de Chip RAM + 512 KB de slow RAM (expansión A501, en $C00000). Sin Fast RAM real. Nada más.**
  - La slow RAM **no es accesible por DMA**: ni Agnus, ni el Blitter, ni Paula, ni el Copper la ven. Solo sirve para datos que lee la CPU.
  - Exec la reporta como memoria no-chip (entra con MEMF_FAST). Para la CPU es igual de rápida que la Chip.
- Kickstart 1.3. Tiene que funcionar ahí; si además anda en 2.04, mejor, pero no es requisito.
- Una sola disquetera (DF0).
- El 68000 no tiene VBR: los vectores de interrupción van en direcciones absolutas (nivel 3 en $6C, nivel 4 en $70).

Si algo del diseño necesita más hardware que esto, el diseño está mal.

---

## Entorno de desarrollo

- **Az trabaja en Windows.** No asumas shell POSIX, `make`, `chmod`, `/dev/null` ni rutas con `/`. Los scripts de build van en **PowerShell** (`build.ps1`) y hay que citar bien las rutas con espacios.
- **Encoder:** C11 portable, un único ejecutable de consola. Compilador: MinGW-w64 (vía MSYS2) o clang. Elegí uno, documentalo y no mezcles.
- **Ensamblador:** vasm (`vasmm68k_mot`, sintaxis Motorola), con salida binaria plana. El código del reproductor tiene que ser **independiente de posición** (PC-relativo), porque se carga donde AllocMem decida.
- **FFmpeg** para Windows en el PATH. El encoder lo invoca como subproceso y lee frames y audio crudos por pipe.
  - **Trampa de Windows:** abrí el pipe en modo binario (`_popen(cmd, "rb")`). En modo texto se corrompen los bytes 0x0D/0x0A y se desalinean los frames.
- **Emulador:** WinUAE, configurado como A500 fiel: 68000 cycle-exact, OCS, KS 1.3, 512K chip + 512K slow, 0 fast, una disquetera, **velocidad de disquete al 100% (sin turbo)**. Dejá el archivo `.uae` en el repo.
- **Kickstart:** nunca descargues ROMs de Kickstart. Az provee la suya; el script solo recibe la ruta.
- Antes de instalar herramientas, verificá qué hay instalado y proponé los comandos a Az en lugar de ejecutarlos a ciegas.

---

## Arquitectura general

```
video.mkv ──► ffmpeg ──► encoder (C) ──► video.adf ──► A500 / WinUAE
                              │
                              └──► decoder de referencia (C) ──► preview en PC
```

Hay tres piezas:

1. **Encoder (C, PC):** decodifica con FFmpeg, escala, cuantiza, comprime, controla la tasa y genera el `.adf` completo (bootblock + reproductor + datos).
2. **Decoder de referencia (C, PC):** implementa bit a bit lo mismo que el reproductor de la Amiga. Comparte código con el encoder y sirve para generar un preview (PNG o MP4 vía FFmpeg) y validar el bitstream **antes** de tocar ensamblador.
3. **Reproductor (asm 68000):** bootblock + programa principal. Carga todo en RAM, toma el hardware y reproduce.

**Regla central:** el encoder simula exactamente el estado del decoder (los dos framebuffers, los copper lists, el audio). Cualquier decisión con pérdida se toma contra el estado **decodificado** simulado, nunca contra el frame fuente. Si no, los errores se acumulan.

---

## Diseño de imagen

- **Frame lógico de 160×128**, mostrado a 320×256 en lowres PAL:
  - Doblado horizontal: cada byte lógico se expande a una palabra con una tabla de 256 entradas (512 bytes). Se aplica al escribir en el framebuffer.
  - Doblado vertical: el framebuffer tiene 128 filas y el Copper reescribe los punteros de bitplane (BPLxPTH/L) cada dos líneas, para que cada fila se vea dos veces sin costo de CPU.
  - ⚠️ **A verificar en el Hito 3:** el momento exacto del WAIT horizontal para reescribir los punteros sin glitches en OCS. Si no se logra de forma estable, el fallback es un framebuffer de 256 filas donde la CPU escribe cada fila dos veces. Documentá qué se eligió y por qué.
- **Relación de aspecto:** el opening probablemente es 16:9. Por defecto se usa **letterbox** (unas 90 filas lógicas activas, centradas), que además ahorra bytes. Parámetro del encoder: `--aspect letterbox|crop|stretch`.
- **3 bitplanes (8 colores)** por defecto, configurable a 2 o 4 (`--planes`).
- **Paleta:**
  - V1: una paleta de 8 colores de 12 bits (RGB444) **por escena**, que cambia solo en cortes de escena detectados. Nada de paleta por frame: produce parpadeo.
  - V2 (Hito 6): paleta por franjas horizontales vía Copper, al estilo del truco de colores de GBVP2. Cada framebuffer tiene su propio copper list con su paleta.
  - Cuantización: k-means u otro método en un espacio perceptual (Lab o similar), con resultado final en RGB444.
- **Dithering:** apagado por defecto. **Prohibida la difusión de error** (Floyd–Steinberg y similares), porque rompe la compresión delta. Como opción se admite dithering ordenado estable en el tiempo (Bayer 2×2/4×4).

---

## Compresión de video

- **Dos framebuffers en Chip RAM (A y B)**, cada uno con su copper list. Cada frame nuevo se decodifica en el buffer oculto y el cambio se hace escribiendo COP1LC durante el vertical blank.
- **Delta contra el contenido del buffer oculto**, que es el penúltimo frame *distinto* mostrado (no necesariamente N−2 en frames de video, porque las repeticiones no intercambian buffers). Así nunca se copia un frame entero. El encoder simula ambos buffers para saber exactamente qué hay en cada uno.
- Al inicio los dos buffers están en negro. El primer frame y los cortes de escena son simplemente deltas grandes: no hacen falta keyframes ni acceso aleatorio.
- **Comando de repetición:** "mantener el frame actual K frames" (sin intercambio de buffers). El anime está animado en dos o en tres, así que esto se usa todo el tiempo. Hay que detectar frames casi idénticos con un umbral.
- **Bitstream base (punto de partida, se puede mejorar midiendo):**
  - Por frame: mapa de bits de filas lógicas modificadas (16 bytes para 128 filas).
  - Por fila modificada y por plano: secuencia de tramos (saltar N bytes, escribir M bytes literales), en bytes lógicos.
  - Todo big-endian. Operaciones simples para el 68000: nada de entropía por bits en el camino caliente.
- **Calidad con pérdida** (`--quality N`, como en GBVP2): permite dejar sin actualizar diferencias pequeñas (bytes o filas cuyo error perceptual quede bajo un umbral). Siempre medido contra el estado decodificado.
- **Límite de costo de decodificación:** cada frame debe decodificarse en menos de 40 ms en un 68000 real. El encoder necesita un modelo de costo (bytes escritos, tramos, filas) calibrado midiendo el reproductor en WinUAE cycle-exact. Si un frame excede el límite, el encoder degrada ese frame o lo reparte en frames siguientes.

---

## Audio

- Mono, reproducido igual en un canal izquierdo y uno derecho de Paula (canales 0 y 1).
- **Formato por defecto:** Fibonacci-delta de 4 bits (el de 8SVX; tabla `-34,-21,-13,-8,-5,-3,-2,-1,0,1,2,3,5,8,13,21`) a ~8 kHz, unos 4 KB/s. Configurable con `--audio-rate` y `--audio-format fib4|pcm8`.
- Frecuencia real de Paula en PAL = 3546895 / período. Elegí el período entero y usá la frecuencia **exacta** resultante en el encoder (período 443 da ≈ 8006,5 Hz).
- Paula solo lee muestras en Chip RAM: se decodifica de a bloques chicos a un buffer en Chip y se encola con la interrupción de audio (nivel 4).
- Recordá que el filtro pasabajos de la A500 (atado al LED de power) afecta el sonido. Con ~8 kHz conviene dejarlo encendido.

---

## Temporización y sincronización

- **Reloj maestro:** el VBL. En PAL no entrelazado la Amiga refresca a ≈ 49,92 Hz, no 50 exactos. **Confirmá el valor exacto con documentación de hardware** y parametrizalo. Video a 1 frame cada 2 VBL (≈ 24,96 fps).
- **Aceleración PAL:** los frames de la fuente se toman 1:1. Si la fuente es 23,976 fps, el video dura ~4% menos y el audio se remuestrea para durar exactamente lo mismo (con el cambio de tono, como en la TV PAL).
  - Verificá la frecuencia real de la fuente con `ffprobe`. Si viene a 29,97 con telecine, hay que hacer telecine inverso primero (filtros `fieldmatch,decimate` de FFmpeg).
- **Sincronización audio/video:** el encoder asigna a cada paquete de frame la cantidad exacta de muestras que corresponde, usando acumulación fraccional con las frecuencias exactas de VBL y Paula, para que no haya deriva. Si un frame se decodifica tarde, el reproductor no debe atrasar el audio: se muestra ese frame tarde, y si el retraso se acumula se saltea la repetición siguiente. Documentá la política elegida.

---

## Contenedor y disco

- **ADF:** 901.120 bytes (80 cilindros × 2 caras × 11 sectores × 512 bytes). El encoder lo genera completo.
- **Bootblock** (sectores 0–1, 1024 bytes):
  - Empieza con `DOS\0`, lleva el checksum en el offset 4 y el código en el offset 12.
  - Checksum: suma de las 256 longwords big-endian (con el campo de checksum en cero) con acarreo circular; el valor final es el complemento (NOT) de esa suma.
  - Al entrar: A6 = ExecBase, A1 = IOStdReq de trackdisk.device ya abierto.
  - Su única tarea es leer el reproductor desde sectores fijos con `CMD_READ` y saltar a él.
- **Kickstart 1.3:** trackdisk exige que el buffer de lectura esté en **Chip RAM**. Se lee a un buffer de Chip y se copia al destino.
- **Disposición:** bootblock → reproductor (sectores fijos, tamaño en una cabecera) → datos. El disco no será un volumen AmigaDOS válido, y está bien.
- **Datos:** cabecera (magic, versión, cantidad de frames, período de audio, dimensiones, planos, tamaño total) + secuencia de **paquetes con prefijo de longitud**. Cada paquete lleva el audio de ese frame y la operación de video (delta, repetición o cambio de paleta).
- **Los datos se reparten en dos bloques de memoria** (slow RAM y el resto de Chip). **Ningún paquete puede quedar partido entre bloques:** el cargador recorre los prefijos de longitud y pasa al segundo bloque en un límite de paquete. Así el lector del reproductor no necesita chequear límites byte a byte.

---

## Reproductor (flujo)

1. Bootblock: carga el reproductor y salta.
2. Reproductor, con el sistema operativo todavía vivo:
   - AllocMem de framebuffers, copper lists y buffers de audio (MEMF_CHIP), y de los bloques de datos (primero el máximo posible con MEMF_FAST, que cae en slow RAM; el resto en Chip).
   - Carga de todos los datos vía trackdisk.
   - Apagado del motor (`TD_MOTOR` 0).
   - Muestra un indicador de progreso simple durante la carga, por ejemplo una barra con el color del borde.
3. Toma del hardware: `Forbid()`, guardar DMACON/INTENA/vectores, `LoadView(NULL)` + dos `WaitTOF()`, instalar las interrupciones propias de nivel 3 y 4, instalar el copper list.
4. Reproducción.
5. Al terminar: pantalla negra y lazo infinito, o reinicio. Volver al sistema operativo no es necesario en V1.

---

## Presupuestos (estimaciones, hay que medirlas)

| Concepto | Estimación |
|---|---|
| Espacio de datos en disco | ~860–880 KB (901.120 − bootblock − reproductor) |
| Memoria libre al arrancar | Desconocida. Estimación optimista ~850–900 KB entre Chip y slow. **Medir en el Hito 0** |
| Framebuffers (2 × 320×128 × 3 planos) | ~31 KB de Chip |
| Audio (29 s a ~8 kHz, fib4) | ~116 KB |
| Video | Lo que sobre; objetivo ~700 KB |
| Lectura de disquete con trackdisk | ~15–25 KB/s. Medir; importa para el tiempo de carga |
| Decodificación por frame | ≤ 40 ms en 68000. Medir y calibrar el modelo de costo |

El **presupuesto real** es el mínimo entre el espacio del disco y la memoria libre medida. El encoder lo recibe como `--budget` y el control de tasa lo respeta con exactitud: el `.adf` nunca debe exceder lo que el reproductor puede cargar.

---

## Hitos (en orden; no saltees)

Cada hito termina con algo que Az puede ver funcionando y con la sección de decisiones actualizada.

**Hito 0: herramientas y medición.**
- Toolchain instalado y documentado (`docs/SETUP.md`).
- `build.ps1` que ensambla un bootblock mínimo, genera un `.adf` con checksum correcto y bootea en WinUAE cambiando el color de fondo.
- Medir, desde el contexto del bootblock, la memoria libre (total y bloque más grande) en Chip y no-Chip con `AvailMem`, y hacerla visible de alguna forma verificable (depurador de WinUAE, patrón de colores, etc.).

**Hito 1: pipeline de imagen en PC.**
- Encoder que lee el video con FFmpeg, aplica letterbox, escala a 160×H y cuantiza a 8 colores por escena con detección de cortes.
- Genera un preview MP4 de cómo se verá, sin compresión todavía.
- Az revisa el preview y elige parámetros.

**Hito 2: bitstream y decoder de referencia.**
- Formato de paquetes, delta contra el buffer oculto, repeticiones, calidad con pérdida y control de tasa contra `--budget`.
- El decoder de referencia en C reconstruye el video y genera el preview.
- Estadísticas: bytes por frame, repeticiones, frames que exceden el costo estimado.

**Hito 3: imagen estática en la Amiga.**
- El `.adf` bootea, carga y muestra un frame fijo con doblado vertical por Copper y paleta.
- Resuelve la duda del WAIT horizontal (o aplica el fallback).

**Hito 4: video sin audio.**
- Carga completa en RAM (con reparto en dos bloques) y reproducción a ≈ 25 fps con doble buffer y repeticiones.
- Medir el costo real de decodificación y calibrar el modelo del encoder.

**Hito 5: audio y sincronización.**
- Fibonacci-delta por interrupción de nivel 4.
- Sincronía verificada al final del video: sin deriva perceptible.

**Hito 6: calidad.**
- Paleta por franjas vía Copper, dithering ordenado opcional y ajuste del control de tasa con el opening real.

**Hito 7: hardware real.**
- Az graba el disco y prueba en su A500. Se corrige lo que difiera de WinUAE.

**Después, opcional:** trackloader MFM propio con decodificación por Blitter, precarga parcial para empezar antes, y soporte multidisco en streaming con cambio de disco sin cortar el video.

---

## Reglas de trabajo

- **Medí, no supongas.** Toda cifra de este documento es una estimación. Cuando midas algo, anotalo en `docs/DECISIONS.md` con fecha, método y resultado.
- `docs/DECISIONS.md` registra cada decisión de diseño no obvia y su motivo, incluidas las que contradigan este documento.
- **El formato del bitstream se define en un único lugar** (`docs/FORMAT.md`) y el encoder, el decoder de referencia y el reproductor lo siguen al pie de la letra. Si cambia el formato, cambia la versión en la cabecera.
- Probá primero en el decoder de referencia y después en la Amiga. Un bug de formato no se depura en ensamblador.
- Commits chicos, un tema por commit.
- **No subas al repo** videos fuente, audio extraído, ADFs generados con contenido de terceros ni ROMs de Kickstart. Todo eso va al `.gitignore`.
- Explicale a Az qué hiciste y por qué al cerrar cada hito, en español y sin rodeos. Si algo no se puede hacer con el hardware objetivo, decilo claramente en lugar de rebajar la meta en silencio.

---

## Estructura sugerida del repo

```
A500VP\
  CLAUDE.md
  build.ps1
  a500vp.uae            (config de WinUAE, sin ruta a la ROM hardcodeada)
  docs\SETUP.md
  docs\FORMAT.md
  docs\DECISIONS.md
  encoder\              (C: encoder + decoder de referencia + escritor de ADF)
  player\               (asm 68000: bootblock + reproductor)
  work\                 (ignorado por git: videos, previews, ADFs)
```
