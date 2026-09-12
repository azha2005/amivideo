# Hoja de ruta despues del Hito 7

`CLAUDE.md` deja como opcional "trackloader MFM propio con decodificacion
por Blitter, precarga parcial y soporte multidisco". Este documento lo
convierte en un plan, con lo medido el 2026-09-12 como punto de partida.

**Nada de esto esta hecho. Las ganancias marcadas como "estimado" son
cuentas, no mediciones, y hay que medirlas antes de darlas por buenas.**

---

## De donde partimos (medido)

| | |
|---|---|
| Datos por disquete | 893 440 bytes (1745 sectores) |
| Lectura (trackdisk + rebote) | 17,9 KB/s |
| Repintar la pantalla entera | 60,3 ms con 8 colores, ~75 ms con 16 |
| Hueco de pantalla | 40,06 ms |
| RAM util para datos | ~915 KB |
| Mejor resultado en imagen real | btf, 13 s, 16 colores, **6,7 %** de pixeles lejos de la fuente, sin perdida |
| Techo con disco infinito | **1,35 %**, pero 304 de 324 frames llegan tarde |

Las dos conclusiones que ordenan todo lo que sigue:

1. **El disco ya no es el cuello de botella principal: lo es la CPU.** Con
   16 colores el 68000 repinta la pantalla entera ~13 veces por segundo, y
   un hueco dura 40 ms. `--min-hold 2` no es una concesion al disquete.
2. **Video en streaming no existe** (17,9 KB/s de lectura contra 63-84 de
   consumo). Todo lo que sigue asume cargar y reproducir.

---

## El porton: fabricar el disco

**H8 — Confirmar que Az puede grabar un formato no estandar.**

Todo lo que da capacidad de verdad (12 o 13 sectores por pista) produce un
disquete que **no es un ADF** y que una disquetera de PC comun no puede
escribir. Hace falta un grabador a nivel de flujo (Greaseweazle, KryoFlux o
similar) o escribir el disquete desde la propia Amiga.

- **Entregable:** un disquete con una pista de 12 sectores escrita desde el
  PC y leida de vuelta sin errores.
- **Riesgo:** si no se puede, H12 y H13 se caen y queda solo la rama de
  CPU (H9-H11), que igual es la que mas calidad da.
- **Por eso va primero.** No tiene sentido escribir un trackloader para un
  formato que no se puede fabricar.

---

## Rama A: calidad sin tocar el hardware (no depende de H8)

Esta rama ataca la pared real, que es el tiempo de decodificacion. Ninguno
de estos hitos necesita el grabador.

### H9 — Audio ADPCM en vez de fib4 (barato, ya esta escrito)

En el fork A5MU se midio IMA ADPCM de 4 bits contra fib4 **con el mismo
tamano exacto**: 21,7 dB contra 12,8 dB a la misma ganancia. fib4 satura por
pendiente y obliga a dejar el volumen al minimo; ADPCM no.

- **Ganancia:** ~9 dB de margen de volumen a igual cantidad de bytes.
- **Costo:** cero bytes. Un poco mas de CPU en la interrupcion de nivel 4
  (hay que medirlo: fib4 cuesta 7,1 %).
- **Trabajo:** portar `encoder/adpcm.c` y la rutina del 68000 desde A5MU.
  El formato sube de version y gana un valor mas en el byte 28.

### H10 — Saltar columnas vacias de a ocho

Ya esta anotado en `DECISIONS.md` (Hito 4): recorrer los 20 bits de la
mascara cuesta ~36 ciclos por columna **aunque no este marcada**, o sea 720
ciclos por fila. Si un byte de la mascara vale 0, se pueden saltar 8
columnas de una.

- **Ganancia estimada:** en frames dispersos (la mayoria), buena parte de
  los 69 000 ciclos por frame que hoy se van en caminar la mascara.
- **Costo:** nada. Es una optimizacion del reproductor, sin cambio de
  formato.
- **Es el cambio con mejor relacion trabajo/beneficio de toda la lista.**

### H11 — Opcode de bloque crudo

Hoy hay un solo opcode de dibujo: el delta con mascara de columnas. En un
corte de escena cambia toda la pantalla, y el delta paga mascara y tabla de
doblado por cada columna igual.

Un opcode "bloque crudo" copia bytes directo al framebuffer. Dos variantes,
con distinto canje:

| | bytes (96 filas, 4 planos) | tiempo |
|---|---|---|
| Delta completo (hoy) | ~7 700 | ~75 ms |
| Crudo sin doblar | ~7 700 (igual) | estimado ~55 ms (se ahorra la mascara) |
| Crudo pre-doblado | 15 360 (el doble) | estimado ~10 ms por CPU, ~5 ms por Blitter |

- **Lo que compra:** mata los picos. Los frames que llegan tarde son los
  cortes de escena y el movimiento de pantalla completa.
- **Lo que cuesta:** el pre-doblado son el doble de bytes, pero solo en los
  frames donde se use. Con 16 cortes en 13 s serian ~123 KB extra.
- **Quien decide:** el control de tasa, que **ya simula la linea de tiempo
  del reproductor** (Hito 4). Se extiende para elegir por frame entre los
  tres opcodes mirando bytes **y** milisegundos.

### H12 — Vector de movimiento global y copia desplazada por Blitter

El candidato con mas potencial para imagen real. En un paneo de camara la
imagen entera se corre unos pixeles: hoy eso se codifica como un delta de
pantalla completa, lo mas caro que hay. El Blitter tiene barrel shifter de
0 a 15 bits: puede copiar el framebuffer desplazado casi gratis.

Opcode nuevo: "corre el buffer (dx, dy) y despues aplica este delta chico"
para el borde que quedo al descubierto.

- **Ganancia estimada, a medir:** en planos con paneo, un orden de magnitud
  menos de bytes y de tiempo. En `btf.mp4` hay paneos; es el material
  ideal para medirlo.
- **Trabajo:** estimacion de movimiento en el encoder (busqueda de bloque
  simple sobre el frame ya escalado) y el opcode en el reproductor.
- **Ojo:** el Blitter compite por ciclos de DMA con los bitplanes. Con 4
  planos en lowres quedan slots libres, pero hay que medirlo, no suponerlo.

---

## Rama B: capacidad y velocidad (depende de H8)

### H13 — Reproductor en disco aparte

Por si solo da **7 680 bytes (0,86 %)**, o sea ~0,1 segundos de video: como
mejora no se justifica. Lo que hace es **volver gratis el tamano del
reproductor**, que hoy son 3 544 bytes que le compiten al video. Con el
reproductor en su propio disquete puede ocupar 50 KB sin que importe, y eso
es lo que necesitan el trackloader y el decodificador por Blitter.

- **Se hace como parte de H14, no antes.** Solo tiene sentido junto al
  trackloader.
- **Costo para el usuario:** un disquete mas y un cambio de disco antes de
  empezar. Hoy un disquete es un video autonomo.

### H14 — Trackloader MFM propio

- **Velocidad:** el techo fisico son 11 sectores x 512 x 5 vueltas/s =
  28 160 B/s con el formato estandar. Un cargador que lea el cilindro
  entero (las dos caras) y decodifique MFM con el Blitter deberia dar
  22-25 KB/s contra los 17,9 de hoy: **estimado +25 a +40 %**.
- **Lo que habilita:** lectura asincronica, o sea **precarga parcial**. No
  hace falta cargar todo antes de empezar: alcanza con juntar la diferencia
  entre consumo y lectura. Para el anime, `(39 - 17,9) x 22 = 464 KB`, o
  sea **26 s de espera en vez de 50**.
- **El pero:** leer mientras se reproduce necesita CPU que hoy no existe
  (la decodificacion media ya esta en 37,6 ms de 40). Por eso la rama A va
  primero: H10, H11 y H12 son los que liberan ese tiempo.

### H15 — Formato de 12 o 13 sectores por pista

| formato | capacidad | ganancia | riesgo |
|---|---|---|---|
| 11 sectores (ADF estandar) | 901 120 | — | ninguno |
| 12 sectores | 983 040 | +9 % | bajo |
| 13 sectores | 1 064 960 | **+18 %** | medio: margen fino, depende del disquete y de la alineacion de la disquetera |

- **Ganancia en lo que importa:** por la curva medida (~75 KB por segundo
  de clip en la receta de imagen real), +18 % son **~2,2 segundos mas** a
  igual calidad.
- **Empezar por 12 sectores**, que es el que usaban casi todos, y probar 13
  en la maquina real de Az antes de comprometerse.

---

## Rama C: mas calidad por pixel (opcional, medir antes)

### H16 — HAM6 para material lento

El encoder HAM6 **ya esta escrito** en A5MU (`encoder/ham.c`, busqueda en
haz por linea). En la tapa del disco de musica bajo los pixeles malos de
0,47 % a 0,04 %.

- **A favor:** 4096 colores en vez de 16. Para material quieto o lento la
  diferencia es enorme.
- **En contra:** son 6 planos, o sea el doble de bytes por columna que 3 y
  el doble de tiempo de decodificacion: repintar entero se iria a ~150 ms,
  unos 6,6 fps. Y los "flecos" de HAM se arrastran feo con movimiento
  horizontal.
- **Veredicto provisional:** sirve para un video muy lento o casi
  diapositivas, no para una escena de accion. Medir con material real antes
  de invertir.

### H17 — 16 colores con franjas de paleta

Hoy las franjas necesitan 3 planos o menos porque el Copper no llega a
escribir 17 MOVE antes de que empiece la linea (`FORMAT.md`). Se puede
resolver escribiendo los colores **al final de la linea anterior**, en el
borde derecho, en vez de al principio de la propia.

- **A favor:** juntaria lo mejor de las dos recetas (16 colores del caso
  imagen real, franjas del caso anime).
- **Ojo:** en imagen real las franjas fueron **contraproducentes** (costura
  en el cielo). Esto sirve para material con zonas horizontales bien
  distintas, no para todo.

---

## Orden recomendado

```
H8  fabricacion (porton)          ── si falla, seguir igual con la rama A
H9  ADPCM                         ── barato, ya escrito
H10 saltar columnas vacias        ── mejor relacion trabajo/beneficio
H11 opcode de bloque crudo        ── mata los picos
H12 vector de movimiento global   ── el mas prometedor para imagen real
H13 + H14 reproductor aparte + trackloader
H15 12 o 13 sectores
H16/H17 mas colores (medir primero)
```

La rama A va primero **aunque H8 salga bien**: da calidad sin depender de
hardware nuevo, y ademas libera la CPU que H14 necesita para leer mientras
reproduce.

---

## Cuanto se ganaria en total (estimado, sin medir)

| | ganancia |
|---|---|
| Capacidad (13 sectores + reproductor aparte) | +19 % de bytes ≈ +2,4 s |
| Velocidad de carga (trackloader) | +25 a +40 % → espera de 50 s a ~26 s con precarga parcial |
| Tiempo de decodificacion (H10 + H11) | los picos dejan de pasarse de 40 ms; permite `--min-hold 1` donde el contenido lo deje |
| Bytes en material con paneo (H12) | sin estimar; es el que hay que medir primero |

**Traducido a lo unico que se ve:** hoy son 13 s al 6,7 % de error. Con la
rama A y la B completas, la expectativa razonable es **20-25 s a la misma
calidad, o 13 s con bastante mas fluidez**. El techo absoluto de la maquina
sigue siendo 1,35 % y ~11 s de RAM: no se va a llegar ahi, pero se puede
recortar buena parte de la distancia.

---

## Lo que se decidio NO hacer

- **Video en streaming entre discos.** Medido: 50,7 % de error a 17,9 KB/s
  y 40,5 % a 24 KB/s, las dos peores que la version que Az llamo horrible.
  Contradice lo que `CLAUDE.md` deja como opcion futura.
- **Dither ordenado.** Perdio las cuatro veces que se midio (Hito 6 y las
  tres pruebas con imagen real).
- **Subir `--stability` para ahorrar bytes.** Mejora la metrica de
  salpicado y empeora el error contra la fuente: pega colores viejos.
- **320x256 sin doblado horizontal.** Cuadruplica los bytes por frame. Con
  un disquete no hay caso.
