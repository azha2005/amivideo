;----------------------------------------------------------------------
; player.s - reproductor de A500VP.
;
; Hito 4: video sin audio. Carga el bitstream entero en RAM, repartido en
; un bloque de slow RAM y otro de Chip sin partir ningun paquete, con una
; barra de progreso; despues toma el hardware y lo reproduce con doble
; buffer, repeticiones y temporizacion por VBL.
;
; Con BENCH definido (vasm -DBENCH=1: disco de medicion) cronometra la
; carga y cada decodificacion, al terminar le devuelve la maquina al
; sistema y graba las mediciones en el disquete. Formato en FORMAT.md.
;
; Entrada (desde boot.s): A6 = ExecBase, A1 = IOStdReq de trackdisk,
; A0 = base propia. Codigo independiente de posicion.
;
; Registros que viven todo el programa:
;   a4 = variables (V_*)          d6 = bitplanes
; Durante la carga a5 = IOStdReq; durante la reproduccion a5 = paquete.
;----------------------------------------------------------------------

        include "exec.i"
        include "video.i"

CHUNK_SECTORS equ 22                  ; un cilindro por lectura
CHUNK_BYTES   equ CHUNK_SECTORS*512
DISK_BYTES    equ 901120
CHIP_MARGIN   equ 4096                ; Chip que se le deja al sistema
HDR_SIZE      equ 32

BAR_TOP       equ $30                 ; barra de carga: lineas $30..$F0
BAR_LINES     equ 192
COL_BAR       equ $0036

TIMING_SECTOR equ 1754                ; tabla de tiempos, 5 sectores
TIMING_MAX    equ 640                 ; paquetes que entran en la tabla
INFO_SECTOR   equ 1759

AUD_SAMPLES   equ 512                 ; muestras por buffer de Paula (64 ms)
AUD_WORDS     equ AUD_SAMPLES/2
AFMT_FIB4     equ 1
AFMT_PCM8     equ 2

COL_NOMEM     equ $0f00               ; rojo: sin memoria / no entra
COL_DISK      equ $0f0f               ; magenta: fallo de trackdisk
COL_BADHDR    equ $0ff0               ; amarillo: bitstream invalido

; --- variables, relativas a a4 (la interrupcion tambien las usa) ---
V_VBL       equ 0       ; l  VBL contados por la interrupcion
V_PENDING   equ 4       ; w  hay un intercambio pedido
V_DUE       equ 6       ; l  VBL en el que toca mostrarlo
V_PCOP      equ 10      ; l  copper list a instalar
V_LATE      equ 14      ; l  frames mostrados tarde
V_MAXLATE   equ 18      ; l  mayor atraso, en VBL
V_HIDDEN    equ 22      ; w  buffer oculto: 0 o 1
V_FB        equ 24      ; 2l framebuffers
V_COP       equ 32      ; 2l copper lists
V_DIRTY     equ 40      ; 2w el copper list tiene la paleta vieja
V_BLK1      equ 44      ; l  bloque 1 (slow RAM)
V_BLK1END   equ 48      ; l  fin de lo usado del bloque 1
V_BLK2      equ 52      ; l  bloque 2 (Chip)
V_BLK2END   equ 56      ; l
V_BLK1SIZE  equ 60      ; l
V_BLK2SIZE  equ 64      ; l
V_INBLK2    equ 68      ; w  el cargador ya paso al bloque 2
V_STEP      equ 70      ; w  bytes por linea de la barra de carga
V_BOUNCE    equ 72      ; l  buffer de rebote en Chip
V_TOD0      equ 76      ; l
V_TODLOAD   equ 80      ; l  VSYNC que tardo la carga
V_OLDINT    equ 84      ; l  vector de nivel 3 del sistema
V_OLDINTENA equ 88      ; w
V_OLDDMA    equ 90      ; w
V_START     equ 92      ; l  VBL en el que se ve el frame 0
V_ENDVBL    equ 96      ; l
V_IOREQ     equ 100     ; l
V_FBSIZE    equ 104     ; l
V_PALBYTES  equ 108     ; w  bytes de paleta de un paquete (franjas x colores x 2)
V_STAMP0    equ 110     ; 8  VBL.l, linea.w, color clock.w
V_STAMP1    equ 118     ; 8
V_MAXDEC    equ 126     ; l  peor decodificacion, en color clocks
V_MAXDECF   equ 130     ; l  y en que frame
V_NDELTA    equ 134     ; l  deltas medidos
V_AFMT      equ 138     ; w  formato de audio (0 = sin audio)
V_APER      equ 140     ; w  periodo de Paula
V_AGO       equ 142     ; w  arrancar el audio en el VBL V_START
V_ABUF      equ 144     ; 2l buffers de Paula
V_ANEXT     equ 152     ; w  buffer que toca llenar
V_APKT      equ 154     ; l  proximo paquete para el lector de audio
V_APTR      equ 158     ; l  proximo byte de audio
V_ALEFT     equ 162     ; l  bytes de audio que quedan en el paquete
V_ANPKT     equ 166     ; l  paquetes que le quedan al lector de audio
V_AACC      equ 170     ; w  acumulador fib4 / ultima muestra
V_ACOUNT    equ 172     ; l  interrupciones de audio (medicion)
V_AFIRST    equ 176     ; 8  estampa de la primera
V_ALAST     equ 184     ; 8  estampa de la ultima
V_OLDINT4   equ 192     ; l  vector de nivel 4 del sistema
V_AFEND     equ 196     ; 8  estampa del final del ultimo llenado
V_AFSUM     equ 204     ; l  color clocks llenando buffers, sumados
V_AFMAX     equ 208     ; l  el llenado mas largo
V_PALPTR    equ 212     ; l  paletas vigentes, dentro de su paquete
VARS_SIZE   equ 216

;----------------------------------------------------------------------
; Cabecera del reproductor. mkadf escribe donde quedaron los datos.
;----------------------------------------------------------------------
player_start:
        bra.w   entry
        dc.b    "A5PL"
hdr_data_off:   dc.l    0             ; offset en bytes desde el inicio del disco
hdr_data_len:   dc.l    0             ; bytes

entry:
        move.l  4.w,a6
        lea     vars(pc),a4
        move.l  a1,V_IOREQ(a4)
        move.l  a1,a5                         ; a5 = IOStdReq durante la carga

        bsr     read_tod
        move.l  d2,V_TOD0(a4)

        ;--- buffer de rebote: trackdisk de KS 1.x solo lee a Chip --
        move.l  #CHUNK_BYTES,d0
        move.l  #MEMF_CHIP,d1
        jsr     _LVOAllocMem(a6)
        move.l  d0,V_BOUNCE(a4)
        beq     nomem

        ;--- primer pedazo: trae la cabecera del bitstream ---------
        move.l  hdr_data_len(pc),d0
        cmp.l   #HDR_SIZE,d0
        blo     badhdr
        move.l  hdr_data_off(pc),d0
        bsr     read_chunk
        bne     diskerr

        move.l  V_BOUNCE(a4),a0
        lea     header(pc),a1
        moveq   #HDR_SIZE/4-1,d0
.hdr:   move.l  (a0)+,(a1)+
        dbf     d0,.hdr

        lea     header(pc),a0
        cmp.l   #$41355650,(a0)               ; "A5VP"
        bne     badhdr
        cmp.w   #4,4(a0)                      ; version de formato
        bne     badhdr
        moveq   #0,d6
        move.b  12(a0),d6                     ; d6 = bitplanes
        beq     badhdr
        cmp.w   #4,d6
        bhi     badhdr
        tst.l   20(a0)                        ; al menos un paquete
        beq     badhdr
        bsr     set_bands                     ; franjas de paleta
        tst.w   d0
        beq     badhdr                        ; alguna que el Copper no puede
        add.w   d0,d0
        move.w  d0,V_PALBYTES(a4)

        moveq   #0,d0                         ; audio
        move.b  28(a0),d0
        move.w  d0,V_AFMT(a4)
        beq.s   .noahdr
        cmp.w   #AFMT_PCM8,d0
        bhi     badhdr
        move.w  14(a0),d0                     ; periodo: Paula no baja de 124
        cmp.w   #124,d0
        blo     badhdr
        move.w  d0,V_APER(a4)
.noahdr:

        ;--- framebuffers y copper lists, en Chip ------------------
        move.w  d6,d0
        mulu    #PLANE_BYTES,d0
        move.l  d0,V_FBSIZE(a4)
        move.l  #MEMF_CHIP|MEMF_CLEAR,d1
        jsr     _LVOAllocMem(a6)
        move.l  d0,V_FB(a4)
        beq     nomem
        move.l  V_FBSIZE(a4),d0
        move.l  #MEMF_CHIP|MEMF_CLEAR,d1
        jsr     _LVOAllocMem(a6)
        move.l  d0,V_FB+4(a4)
        beq     nomem
        move.l  #COPPER_SIZE,d0
        move.l  #MEMF_CHIP,d1
        jsr     _LVOAllocMem(a6)
        move.l  d0,V_COP(a4)
        beq     nomem
        move.l  #COPPER_SIZE,d0
        move.l  #MEMF_CHIP,d1
        jsr     _LVOAllocMem(a6)
        move.l  d0,V_COP+4(a4)
        beq     nomem

        ;--- buffers de Paula: solo lee Chip ------------------------
        tst.w   V_AFMT(a4)
        beq.s   .noabuf
        move.l  #2*AUD_SAMPLES,d0
        move.l  #MEMF_CHIP|MEMF_CLEAR,d1
        jsr     _LVOAllocMem(a6)
        move.l  d0,V_ABUF(a4)
        beq     nomem
        add.l   #AUD_SAMPLES,d0
        move.l  d0,V_ABUF+4(a4)
.noabuf:

        ;--- bloques de datos ---------------------------------------
        move.l  hdr_data_len(pc),d3
        sub.l   #HDR_SIZE,d3                  ; d3 = bytes de paquetes

        move.l  #MEMF_FAST|MEMF_LARGEST,d1     ; 1: el mayor bloque no-Chip,
        jsr     _LVOAvailMem(a6)              ;    que en una A500 es la slow
        cmp.l   d3,d0
        bls.s   .b1size
        move.l  d3,d0
.b1size:
        move.l  d0,V_BLK1SIZE(a4)
        beq.s   .b1none
        move.l  #MEMF_FAST,d1
        jsr     _LVOAllocMem(a6)
        move.l  d0,V_BLK1(a4)
        bne.s   .b1done
.b1none:
        clr.l   V_BLK1(a4)
        clr.l   V_BLK1SIZE(a4)
.b1done:
        move.l  #MEMF_CHIP|MEMF_LARGEST,d1     ; 2: Chip, con un margen
        jsr     _LVOAvailMem(a6)
        sub.l   #CHIP_MARGIN,d0
        bls     nomem
        cmp.l   d3,d0
        bls.s   .b2size
        move.l  d3,d0
.b2size:
        move.l  d0,V_BLK2SIZE(a4)
        move.l  #MEMF_CHIP,d1
        jsr     _LVOAllocMem(a6)
        move.l  d0,V_BLK2(a4)
        beq     nomem

        ;--- pantalla de carga: el borde se llena de arriba abajo ---
        move.l  hdr_data_len(pc),d0
        divu    #BAR_LINES,d0
        tst.w   d0
        bne.s   .step
        moveq   #1,d0
.step:  move.w  d0,V_STEP(a4)

        lea     gfxname(pc),a1
        moveq   #0,d0
        jsr     _LVOOpenLibrary(a6)
        tst.l   d0
        beq     nomem
        move.l  d0,a6                         ; a6 = GfxBase
        sub.l   a1,a1
        jsr     _LVOLoadView(a6)
        jsr     _LVOWaitTOF(a6)
        jsr     _LVOWaitTOF(a6)
        move.l  4.w,a6
        lea     loadcop(pc),a0
        lea     CUSTOM,a1
        move.l  a0,COP1LC(a1)
        move.w  d0,COPJMP1(a1)

        ;--- carga, pedazo por pedazo, sin partir paquetes ---------
        ; a0 = origen en el rebote   d2 = bytes disponibles del pedazo
        ; a2 = destino  a3 = fin del bloque   d4 = offset en disco
        ; d5 = bytes que faltan leer          d7 = bytes que faltan del paquete
        move.l  hdr_data_off(pc),d4
        move.l  hdr_data_len(pc),d5
        move.l  V_BLK1(a4),a2
        move.l  a2,a3
        add.l   V_BLK1SIZE(a4),a3
        clr.w   V_INBLK2(a4)
        moveq   #0,d7
        move.l  V_BOUNCE(a4),a0               ; el primer pedazo ya esta leido
        move.l  #CHUNK_BYTES,d2
        cmp.l   d5,d2
        bls.s   .first
        move.l  d5,d2
.first: sub.l   d2,d5
        lea     HDR_SIZE(a0),a0               ; la cabecera ya se copio
        sub.l   #HDR_SIZE,d2
        bra.s   .copy

.next:  add.l   #CHUNK_BYTES,d4
        move.l  d4,d0
        bsr     read_chunk
        bne     diskerr
        move.l  V_BOUNCE(a4),a0
        move.l  #CHUNK_BYTES,d2
        cmp.l   d5,d2
        bls.s   .take
        move.l  d5,d2
.take:  sub.l   d2,d5

.copy:  tst.l   d2
        beq.s   .chunkdone
        tst.l   d7
        bne.s   .body
        ; Empieza un paquete. Su longitud nunca queda partida entre dos
        ; pedazos: los paquetes arrancan en offset par y el pedazo es par.
        moveq   #0,d7
        move.b  (a0),d7
        lsl.w   #8,d7
        move.b  1(a0),d7
        tst.w   d7
        beq     badhdr
        move.l  a2,d0
        add.l   d7,d0
        cmp.l   a3,d0
        bls.s   .body
        tst.w   V_INBLK2(a4)                  ; no entra en el bloque 1:
        bne     nomem                         ; pasar al 2, una sola vez
        move.w  #1,V_INBLK2(a4)
        move.l  a2,V_BLK1END(a4)
        move.l  V_BLK2(a4),a2
        move.l  a2,a3
        add.l   V_BLK2SIZE(a4),a3
        move.l  a2,d0
        add.l   d7,d0
        cmp.l   a3,d0
        bhi     nomem
.body:  move.l  d7,d1                         ; copiar min(paquete, pedazo)
        cmp.l   d2,d1
        bls.s   .n
        move.l  d2,d1
.n:     sub.l   d1,d7
        sub.l   d1,d2
        subq.w  #1,d1
.cb:    move.b  (a0)+,(a2)+
        dbf     d1,.cb
        bra.s   .copy

.chunkdone:
        bsr     progress
        tst.l   d5
        bne     .next

        tst.l   d7                            ; el ultimo paquete quedo entero?
        bne     badhdr
        tst.w   V_INBLK2(a4)
        bne.s   .in2
        move.l  a2,V_BLK1END(a4)              ; todo entro en el bloque 1
        move.l  V_BLK2(a4),V_BLK2END(a4)
        bra.s   .loaded
.in2:   move.l  a2,V_BLK2END(a4)
.loaded:
        move.l  a5,a1                         ; apagar el motor
        move.w  #TD_MOTOR,IO_COMMAND(a1)
        clr.l   IO_LENGTH(a1)
        jsr     _LVODoIO(a6)
        bsr     read_tod
        sub.l   V_TOD0(a4),d2
        and.l   #$00ffffff,d2
        move.l  d2,V_TODLOAD(a4)

        ;--- copper lists de reproduccion, en negro ----------------
        lea     dbltab(pc),a3
        bsr     build_table
        move.l  V_COP(a4),a0                  ; en negro hasta el primer delta
        move.l  V_FB(a4),a2
        bsr     build_copper
        move.l  V_COP+4(a4),a0
        move.l  V_FB+4(a4),a2
        bsr     build_copper

        ;--- audio: el lector recorre los paquetes por su cuenta ----
        ; Independiente del video: si un frame llega tarde, el audio no se
        ; entera. Se llena el primer buffer; el otro lo llena la primera
        ; interrupcion, que llega apenas Paula engancha este.
        tst.w   V_AFMT(a4)
        beq.s   .noainit
        move.l  V_BLK1(a4),V_APKT(a4)
        lea     header(pc),a0
        move.l  20(a0),V_ANPKT(a4)
        clr.l   V_ALEFT(a4)
        clr.w   V_AACC(a4)
        move.l  V_ABUF(a4),a0
        bsr     audio_fill
        move.w  #1,V_ANEXT(a4)
.noainit:

        ;--- tomar el hardware -------------------------------------
        jsr     _LVOForbid(a6)
        lea     CUSTOM,a0
        move.w  INTENAR(a0),V_OLDINTENA(a4)
        move.w  DMACONR(a0),V_OLDDMA(a4)
        move.w  #$7fff,INTENA(a0)
        move.w  #$7fff,INTREQ(a0)
        move.w  #$7fff,DMACON(a0)
        move.l  $6c.w,V_OLDINT(a4)
        lea     level3(pc),a1
        move.l  a1,$6c.w
        move.l  $70.w,V_OLDINT4(a4)
        lea     level4(pc),a1
        move.l  a1,$70.w

        tst.w   V_AFMT(a4)                    ; Paula, lista pero sin DMA:
        beq.s   .nopaula                      ; arranca en el VBL del frame 0
        move.l  V_ABUF(a4),d0
        move.l  d0,AUD0LC(a0)
        move.l  d0,AUD1LC(a0)
        move.w  #AUD_WORDS,AUD0LEN(a0)
        move.w  #AUD_WORDS,AUD1LEN(a0)
        move.w  V_APER(a4),AUD0PER(a0)
        move.w  V_APER(a4),AUD1PER(a0)
        move.w  #64,AUD0VOL(a0)
        move.w  #64,AUD1VOL(a0)
        bclr    #1,CIAA_PRA                   ; filtro pasabajos encendido
.nopaula:
        move.l  V_COP(a4),COP1LC(a0)
        move.w  d0,COPJMP1(a0)
        move.w  #$8380,DMACON(a0)             ; MASTER|RASTER|COPPER
        move.w  #$c0a0,INTENA(a0)             ; INTEN|AUD0|VERTB

        ;--- reproduccion ------------------------------------------
        ; El frame n se ve en el VBL V_START + 2n. Un DELTA se dibuja en el
        ; buffer oculto y se pide el intercambio para su VBL; la
        ; interrupcion lo hace. Si llega tarde se ve tarde, y las
        ; repeticiones que siguen absorben el atraso.
        move.w  #1,V_HIDDEN(a4)               ; se ve el 0, se dibuja en el 1
        clr.w   V_PENDING(a4)
        clr.w   V_DIRTY(a4)
        clr.w   V_DIRTY+2(a4)
        move.l  V_VBL(a4),d0
        addq.l  #2,d0
        move.l  d0,V_START(a4)
        move.w  V_AFMT(a4),V_AGO(a4)          ; despues de fijar V_START

        lea     header(pc),a0
        move.l  20(a0),d5                     ; d5 = paquetes
        moveq   #0,d7                         ; d7 = frame
        move.l  V_BLK1(a4),a5                 ; a5 = paquete actual
        lea     dbltab(pc),a3

.frame: cmp.l   V_BLK1END(a4),a5              ; fin del bloque 1: seguir en el 2
        bne.s   .inblk
        move.l  V_BLK2(a4),a5
.inblk: tst.b   2(a5)                         ; op
        bne     .advance                      ; REPETICION: no se toca nada

.wait:  tst.w   V_PENDING(a4)                 ; el oculto todavia se ve
        bne.s   .wait

        lea     6(a5),a0
        btst    #0,3(a5)                      ; paleta nueva: se usa donde
        beq.s   .nopal                        ; esta, el paquete no se mueve
        move.l  a0,V_PALPTR(a4)
        add.w   V_PALBYTES(a4),a0
        move.w  #1,V_DIRTY(a4)
        move.w  #1,V_DIRTY+2(a4)
.nopal: add.w   4(a5),a0                      ; saltar el audio
        move.l  a0,-(sp)                      ; a0 = delta

        move.w  V_HIDDEN(a4),d1
        add.w   d1,d1                         ; indice de palabra
        tst.w   V_DIRTY(a4,d1.w)
        beq.s   .clean
        clr.w   V_DIRTY(a4,d1.w)
        add.w   d1,d1                         ; indice de longword
        move.l  V_COP(a4,d1.w),a0
        move.l  V_PALPTR(a4),a1
        bsr     write_palette
        bra.s   .fb
.clean: add.w   d1,d1
.fb:    move.l  V_FB(a4,d1.w),a2
        move.l  (sp)+,a0

        ifd     BENCH
        lea     V_STAMP0(a4),a1
        bsr     stamp
        endc

        bsr     apply_delta

        ifd     BENCH
        lea     V_STAMP1(a4),a1
        bsr     stamp
        bsr     elapsed                       ; d0 = color clocks
        addq.l  #1,V_NDELTA(a4)
        cmp.l   #TIMING_MAX,d7
        bhs.s   .notab
        lea     timing(pc),a1
        move.l  d7,d1
        lsl.l   #2,d1
        move.l  d0,0(a1,d1.l)
.notab: cmp.l   V_MAXDEC(a4),d0
        bls.s   .notmax
        move.l  d0,V_MAXDEC(a4)
        move.l  d7,V_MAXDECF(a4)
.notmax:
        endc

        move.w  V_HIDDEN(a4),d1               ; pedir el intercambio
        add.w   d1,d1
        add.w   d1,d1
        move.l  V_COP(a4,d1.w),V_PCOP(a4)
        move.l  d7,d0
        add.l   d0,d0
        add.l   V_START(a4),d0
        move.l  d0,V_DUE(a4)
        move.w  #1,V_PENDING(a4)              ; la interrupcion mira esto primero
        eor.w   #1,V_HIDDEN(a4)

.advance:
        moveq   #0,d0
        move.w  (a5),d0
        add.l   d0,a5
        addq.l  #1,d7
        cmp.l   d5,d7
        blo     .frame

.drain: tst.w   V_PENDING(a4)
        bne.s   .drain
        move.l  d5,d0                         ; el ultimo frame dura sus 2 VBL
        add.l   d0,d0
        add.l   V_START(a4),d0
.hold:  cmp.l   V_VBL(a4),d0
        bhi.s   .hold
        move.l  V_VBL(a4),V_ENDVBL(a4)
        ; Se termino: callar a Paula. Primero la interrupcion y despues el
        ; DMA: al cortar el DMA, Paula pide una interrupcion mas que no es
        ; un buffer (medido en el Hito 5: aparecia como deriva falsa).
        lea     CUSTOM,a0
        move.w  #$0080,INTENA(a0)             ; AUD0
        move.w  #$0003,DMACON(a0)             ; AUD0EN|AUD1EN
        move.w  #$0080,INTREQ(a0)
        move.w  #$0080,INTREQ(a0)

        lea     loadcop_end(pc),a0            ; pantalla negra: barra en cero
        move.w  #(BAR_TOP<<8)|$07,(a0)
        lea     loadcop(pc),a0
        move.l  a0,V_PCOP(a4)
        move.l  V_VBL(a4),d0
        addq.l  #1,d0
        move.l  d0,V_DUE(a4)
        move.w  #1,V_PENDING(a4)
.black: tst.w   V_PENDING(a4)
        bne.s   .black

        ifd     BENCH
        bsr     bench_finish
        endc
.forever:
        bra.s   .forever

;----------------------------------------------------------------------
; Errores: el color se reescribe en un lazo para que el copper list del
; sistema, si todavia esta, no lo tape.
;----------------------------------------------------------------------
nomem:  move.w  #COL_NOMEM,d0
        bra.s   stop
diskerr:
        move.w  #COL_DISK,d0
        bra.s   stop
badhdr: move.w  #COL_BADHDR,d0
stop:   lea     CUSTOM,a0
.loop:  move.w  d0,COLOR00(a0)
        bra.s   .loop

;----------------------------------------------------------------------
; level3 - interrupcion de VBL: cuenta, y hace el intercambio pedido si ya
; es su momento. Reescribe COP1LC y reinicia el Copper: en el blanking,
; antes de la linea $2C, es seguro.
;----------------------------------------------------------------------
level3:
        movem.l d0/a0-a1,-(sp)
        lea     CUSTOM,a0
        move.w  INTREQR(a0),d0
        and.w   #$0020,d0                     ; VERTB
        beq.s   .out
        move.w  d0,INTREQ(a0)
        move.w  d0,INTREQ(a0)
        lea     vars(pc),a1
        addq.l  #1,V_VBL(a1)
        tst.w   V_AGO(a1)                     ; arrancar el audio con el frame 0
        beq.s   .noago
        move.l  V_VBL(a1),d0
        cmp.l   V_START(a1),d0
        blt.s   .noago
        clr.w   V_AGO(a1)
        move.w  #$8203,DMACON(a0)             ; AUD0EN|AUD1EN
.noago: tst.w   V_PENDING(a1)
        beq.s   .out
        move.l  V_VBL(a1),d0
        sub.l   V_DUE(a1),d0
        bmi.s   .out                          ; todavia no
        move.l  V_PCOP(a1),COP1LC(a0)
        move.w  d0,COPJMP1(a0)
        clr.w   V_PENDING(a1)
        tst.l   d0
        beq.s   .out
        addq.l  #1,V_LATE(a1)                 ; llego tarde: d0 VBL
        cmp.l   V_MAXLATE(a1),d0
        bls.s   .out
        move.l  d0,V_MAXLATE(a1)
.out:   movem.l (sp)+,d0/a0-a1
        rte

;----------------------------------------------------------------------
; level4 - interrupcion de audio del canal 0. Paula acaba de enganchar el
; buffer que se le encolo la vez anterior y lo esta tocando; el otro quedo
; libre. Se lo llena y se lo encola: lo engancha cuando termine este.
;
; Llenarlo cuesta ~2 ms. Despues de acusar la interrupcion se baja la
; prioridad a 2 para que el VBL (nivel 3) pueda interrumpir: si no, el
; intercambio de copper list podria caer ya dentro de la pantalla.
;----------------------------------------------------------------------
level4:
        movem.l d0-d5/a0-a4,-(sp)
        lea     CUSTOM,a0
        move.w  #$0080,INTREQ(a0)             ; AUD0
        move.w  #$0080,INTREQ(a0)
        lea     vars(pc),a4

        ifd     BENCH
        lea     V_ALAST(a4),a1
        bsr     stamp_irq
        tst.l   V_ACOUNT(a4)
        bne.s   .notfirst
        move.l  V_ALAST(a4),V_AFIRST(a4)
        move.l  V_ALAST+4(a4),V_AFIRST+4(a4)
.notfirst:
        addq.l  #1,V_ACOUNT(a4)
        endc

        move.w  #$2200,sr                     ; dejar pasar al VBL
        move.w  V_ANEXT(a4),d0
        add.w   d0,d0
        add.w   d0,d0
        lea     V_ABUF(a4),a0                 ; d8(An,Xn) no llega a 144
        move.l  0(a0,d0.w),a0
        bsr     audio_fill
        lea     CUSTOM,a1
        move.l  a0,AUD0LC(a1)                 ; los dos canales, mismo buffer
        move.l  a0,AUD1LC(a1)
        eor.w   #1,V_ANEXT(a4)

        ifd     BENCH                         ; cuanto costo este llenado
        move.w  #$2400,sr                     ; sin VBL: stamp_irq corrige un
        lea     V_AFEND(a4),a1                ; VBL pendiente, no uno que la
        bsr     stamp_irq                     ; interrumpe a mitad de camino
        lea     V_ALAST(a4),a0
        bsr     stamp_diff
        add.l   d0,V_AFSUM(a4)
        cmp.l   V_AFMAX(a4),d0
        bls.s   .notmaxf
        move.l  d0,V_AFMAX(a4)
.notmaxf:
        endc

        movem.l (sp)+,d0-d5/a0-a4
        rte

;----------------------------------------------------------------------
; audio_fill - llena un buffer de Paula con las proximas AUD_SAMPLES
; muestras del flujo de audio. Cuando el flujo se termina, repite la
; ultima muestra: silencio.
;   a0 = buffer, a4 = variables. Preserva todo.
;----------------------------------------------------------------------
audio_fill:
        movem.l d0-d5/a0-a3,-(sp)
        move.w  #AUD_SAMPLES,d5               ; muestras que faltan
        move.w  V_AACC(a4),d4                 ; acumulador (byte bajo)
        lea     fibtab(pc),a3
        move.l  V_APTR(a4),a1
        move.l  V_ALEFT(a4),d3
.more:  tst.l   d3
        bne.s   .have
        bsr     audio_nextpkt                 ; a1, d3 del paquete siguiente
        beq.s   .silence                      ; no quedan
.have:  cmp.w   #AFMT_FIB4,V_AFMT(a4)
        bne.s   .pcm

.fib:   moveq   #0,d0                         ; fib4: dos muestras por byte,
        move.b  (a1)+,d0                      ; nibble alto primero
        move.w  d0,d1
        lsr.w   #4,d1
        add.b   0(a3,d1.w),d4
        move.b  d4,(a0)+
        and.w   #$000f,d0
        add.b   0(a3,d0.w),d4
        move.b  d4,(a0)+
        subq.l  #1,d3
        subq.w  #2,d5                         ; paquetes y buffer son pares
        beq.s   .done
        tst.l   d3
        bne.s   .fib
        bra.s   .more

.pcm:   move.b  (a1)+,d4                      ; pcm8: una muestra por byte
        move.b  d4,(a0)+
        subq.l  #1,d3
        subq.w  #1,d5
        beq.s   .done
        tst.l   d3
        bne.s   .pcm
        bra.s   .more

.silence:
        move.b  d4,(a0)+
        subq.w  #1,d5
        bne.s   .silence
.done:  move.l  a1,V_APTR(a4)
        move.l  d3,V_ALEFT(a4)
        move.w  d4,V_AACC(a4)
        movem.l (sp)+,d0-d5/a0-a3
        rts

;----------------------------------------------------------------------
; audio_nextpkt - pasa el lector de audio al paquete siguiente.
;   devuelve a1 = primer byte de audio, d3 = bytes de audio.
;   Z = 1 si no quedan paquetes.
;----------------------------------------------------------------------
audio_nextpkt:
        movem.l d0/a2,-(sp)
.again: tst.l   V_ANPKT(a4)
        beq.s   .end
        subq.l  #1,V_ANPKT(a4)
        move.l  V_APKT(a4),a2
        cmp.l   V_BLK1END(a4),a2              ; fin del bloque 1: seguir en el 2
        bne.s   .inblk
        move.l  V_BLK2(a4),a2
.inblk: moveq   #0,d3
        move.w  4(a2),d3                      ; bytes de audio
        lea     6(a2),a1
        btst    #0,3(a2)                      ; el audio va despues de la paleta
        beq.s   .nopal
        add.w   V_PALBYTES(a4),a1
.nopal: moveq   #0,d0
        move.w  (a2),d0
        add.l   d0,a2
        move.l  a2,V_APKT(a4)
        tst.l   d3
        beq.s   .again                        ; paquete sin audio
        movem.l (sp)+,d0/a2                   ; movem no toca los flags: Z = 0
        rts
.end:   movem.l (sp)+,d0/a2
        moveq   #0,d3                         ; Z = 1
        rts

;----------------------------------------------------------------------
; read_chunk - lee CHUNK_BYTES (o lo que quede de disco) al rebote.
;   d0 = offset en disco, multiplo de 512.  Z = 1 si salio bien.
;----------------------------------------------------------------------
read_chunk:
        movem.l d1/a0-a1,-(sp)
        move.l  #DISK_BYTES,d1
        sub.l   d0,d1
        cmp.l   #CHUNK_BYTES,d1
        bls.s   .len
        move.l  #CHUNK_BYTES,d1
.len:   move.l  a5,a1
        move.w  #CMD_READ,IO_COMMAND(a1)
        move.l  d1,IO_LENGTH(a1)
        move.l  V_BOUNCE(a4),IO_DATA(a1)
        move.l  d0,IO_OFFSET(a1)
        jsr     _LVODoIO(a6)
        tst.l   d0
        movem.l (sp)+,d1/a0-a1                ; movem no toca los flags
        rts

;----------------------------------------------------------------------
; progress - corre el fin de la barra de carga. d5 = bytes que faltan.
;----------------------------------------------------------------------
progress:
        movem.l d0-d1/a0,-(sp)
        move.l  hdr_data_len(pc),d0
        sub.l   d5,d0                         ; cargado
        move.w  V_STEP(a4),d1
        divu    d1,d0
        cmp.w   #BAR_LINES,d0
        bls.s   .ok
        move.w  #BAR_LINES,d0
.ok:    add.w   #BAR_TOP,d0
        lsl.w   #8,d0
        or.w    #$0007,d0
        lea     loadcop_end(pc),a0
        move.w  d0,(a0)
        movem.l (sp)+,d0-d1/a0
        rts

;----------------------------------------------------------------------
; read_tod - d2 = TOD del CIA-A (cuenta VSYNC, 24 bits). Solo lee: leer
; el byte alto congela el contador hasta leer el bajo. Con el sistema
; vivo, entre Disable/Enable para que nadie lo lea en el medio.
;----------------------------------------------------------------------
read_tod:
        movem.l d0-d1/a0-a1,-(sp)
        jsr     _LVODisable(a6)
        moveq   #0,d2
        move.b  CIAA_TODHI,d2
        lsl.l   #8,d2
        move.b  CIAA_TODMID,d2
        lsl.l   #8,d2
        move.b  CIAA_TODLO,d2
        jsr     _LVOEnable(a6)
        movem.l (sp)+,d0-d1/a0-a1
        rts

        ifd     BENCH
;----------------------------------------------------------------------
; stamp - guarda (VBL, linea, color clock) en (a1). Relee el contador de
; VBL para no mezclar un VBL con la posicion del haz del siguiente.
;----------------------------------------------------------------------
stamp:
        movem.l d0-d1/a0,-(sp)
        lea     CUSTOM,a0
.again: move.l  V_VBL(a4),d0
        move.l  VPOSR(a0),d1                  ; VPOSR:VHPOSR
        cmp.l   V_VBL(a4),d0
        bne.s   .again
        move.l  d0,(a1)
        move.l  d1,d0
        lsr.l   #8,d0
        and.w   #$01ff,d0                     ; linea (V8..V0)
        move.w  d0,4(a1)
        and.w   #$00ff,d1                     ; color clock (H8..H1)
        move.w  d1,6(a1)
        movem.l (sp)+,d0-d1/a0
        rts

;----------------------------------------------------------------------
; stamp_irq - como stamp, pero para usar dentro de la interrupcion de
; audio, donde la de VBL puede estar pendiente sin atender: si el haz ya
; volvio arriba y VERTB esta pedido, el VBL ya paso aunque el contador
; todavia no lo sepa.
;----------------------------------------------------------------------
stamp_irq:
        movem.l d0-d2/a0,-(sp)
        lea     CUSTOM,a0
        move.l  V_VBL(a4),d0
        move.l  VPOSR(a0),d1
        move.w  INTREQR(a0),d2
        btst    #5,d2
        beq.s   .ok
        move.l  d1,d2
        lsr.l   #8,d2
        and.w   #$01ff,d2
        cmp.w   #156,d2
        bhs.s   .ok
        addq.l  #1,d0
.ok:    move.l  d0,(a1)
        move.l  d1,d0
        lsr.l   #8,d0
        and.w   #$01ff,d0
        move.w  d0,4(a1)
        and.w   #$00ff,d1
        move.w  d1,6(a1)
        movem.l (sp)+,d0-d2/a0
        rts

;----------------------------------------------------------------------
; stamp_diff - d0 = color clocks entre la estampa (a0) y la (a1), que
; tienen que estar a pocos frames una de otra.
;----------------------------------------------------------------------
stamp_diff:
        move.l  d1,-(sp)
        move.l  (a1),d0
        sub.l   (a0),d0                       ; frames
        mulu    #313,d0
        move.w  4(a1),d1
        sub.w   4(a0),d1
        ext.l   d1
        add.l   d1,d0                         ; lineas
        muls    #227,d0
        move.w  6(a1),d1
        sub.w   6(a0),d1
        ext.l   d1
        add.l   d1,d0
        move.l  (sp)+,d1
        rts

;----------------------------------------------------------------------
; elapsed - d0 = color clocks entre V_STAMP0 y V_STAMP1.
; Frame PAL no entrelazado: 313 lineas de 227 color clocks.
;----------------------------------------------------------------------
elapsed:
        move.l  d1,-(sp)
        move.l  V_STAMP1(a4),d0
        sub.l   V_STAMP0(a4),d0               ; frames (pocos)
        mulu    #313,d0
        move.w  V_STAMP1+4(a4),d1
        sub.w   V_STAMP0+4(a4),d1
        ext.l   d1
        add.l   d1,d0                         ; lineas
        muls    #227,d0
        move.w  V_STAMP1+6(a4),d1
        sub.w   V_STAMP0+6(a4),d1
        ext.l   d1
        add.l   d1,d0
        move.l  (sp)+,d1
        rts

;----------------------------------------------------------------------
; bench_finish - devuelve la maquina al sistema y graba las mediciones:
; la tabla de tiempos y el sector de resumen (FORMAT.md).
;----------------------------------------------------------------------
bench_finish:
        lea     CUSTOM,a0
        move.w  #$7fff,INTENA(a0)
        move.w  #$7fff,INTREQ(a0)
        move.l  V_OLDINT(a4),$6c.w
        move.l  V_OLDINT4(a4),$70.w
        move.w  V_OLDDMA(a4),d0
        and.w   #$ffdf,d0                     ; sin sprites: no hay punteros
        or.w    #$8000,d0
        move.w  d0,DMACON(a0)
        move.w  V_OLDINTENA(a4),d0
        or.w    #$c000,d0
        move.w  d0,INTENA(a0)
        move.l  4.w,a6
        jsr     _LVOPermit(a6)

        lea     infobuf(pc),a0
        lea     header(pc),a1
        move.l  #$504c4159,(a0)+              ; "PLAY"
        move.l  20(a1),(a0)+                  ; paquetes
        move.l  V_TODLOAD(a4),(a0)+           ; carga, en VSYNC
        move.l  hdr_data_len(pc),(a0)+        ; bytes cargados
        move.l  V_BLK1(a4),(a0)+
        move.l  V_BLK1END(a4),d0
        sub.l   V_BLK1(a4),d0
        move.l  d0,(a0)+                      ; bytes en el bloque 1
        move.l  V_BLK2(a4),(a0)+
        move.l  V_BLK2END(a4),d0
        sub.l   V_BLK2(a4),d0
        move.l  d0,(a0)+                      ; bytes en el bloque 2
        move.l  V_LATE(a4),(a0)+
        move.l  V_MAXLATE(a4),(a0)+
        move.l  V_ENDVBL(a4),d0
        sub.l   V_START(a4),d0
        move.l  d0,(a0)+                      ; VBL de reproduccion
        move.l  20(a1),d0
        add.l   d0,d0
        move.l  d0,(a0)+                      ; VBL esperados
        move.l  V_FB(a4),(a0)+
        move.l  V_FB+4(a4),(a0)+
        move.l  V_MAXDEC(a4),(a0)+
        move.l  V_MAXDECF(a4),(a0)+
        move.l  V_NDELTA(a4),(a0)+
        move.l  #TIMING_MAX,(a0)+
        move.l  a0,-(sp)                      ; (72) error de la tabla

        move.l  V_IOREQ(a4),a1
        move.w  #CMD_WRITE,IO_COMMAND(a1)
        move.l  #TIMING_MAX*4,IO_LENGTH(a1)
        lea     timing(pc),a0
        move.l  a0,IO_DATA(a1)
        move.l  #TIMING_SECTOR*512,IO_OFFSET(a1)
        jsr     _LVODoIO(a6)
        move.l  (sp)+,a0
        move.l  V_IOREQ(a4),a1
        moveq   #0,d0
        move.b  IO_ERROR(a1),d0
        move.l  d0,(a0)

        lea     infobuf(pc),a0                ; audio (desde el offset 76)
        move.l  V_ACOUNT(a4),76(a0)
        move.l  V_AFIRST(a4),80(a0)
        move.l  V_AFIRST+4(a4),84(a0)
        move.l  V_ALAST(a4),88(a0)
        move.l  V_ALAST+4(a4),92(a0)
        move.l  V_START(a4),96(a0)
        moveq   #0,d0
        move.w  V_AFMT(a4),d0
        move.l  d0,100(a0)
        move.w  V_APER(a4),d0
        move.l  d0,104(a0)
        move.l  V_AFSUM(a4),108(a0)
        move.l  V_AFMAX(a4),112(a0)

        move.l  V_IOREQ(a4),a1
        move.w  #CMD_WRITE,IO_COMMAND(a1)
        move.l  #512,IO_LENGTH(a1)
        lea     infobuf(pc),a0
        move.l  a0,IO_DATA(a1)
        move.l  #INFO_SECTOR*512,IO_OFFSET(a1)
        jsr     _LVODoIO(a6)

        move.l  V_IOREQ(a4),a1                ; bajar la pista a disco
        move.w  #CMD_UPDATE,IO_COMMAND(a1)
        clr.l   IO_LENGTH(a1)
        jsr     _LVODoIO(a6)

        move.l  V_IOREQ(a4),a1                ; y apagar el motor
        move.w  #TD_MOTOR,IO_COMMAND(a1)
        clr.l   IO_LENGTH(a1)
        jsr     _LVODoIO(a6)
        rts
        endc

        include "video_code.i"

;----------------------------------------------------------------------
; Datos
;----------------------------------------------------------------------
        even
gfxname:    dc.b    "graphics.library",0
        even
header:     ds.b    HDR_SIZE
vars:       ds.b    VARS_SIZE
dbltab:     ds.w    256
fibtab:     dc.b    -34,-21,-13,-8,-5,-3,-2,-1,0,1,2,3,5,8,13,21
        even

;----------------------------------------------------------------------
; Copper list de la carga (y de la pantalla negra del final). Sin
; bitplanes: el color de fondo llena todo, borde incluido.
;----------------------------------------------------------------------
        cnop    0,4
loadcop:
        dc.w    $0100,$0200                   ; BPLCON0: sin planos
        dc.w    $0180,$0000
        dc.w    (BAR_TOP<<8)|$07,$fffe
        dc.w    $0180,COL_BAR
loadcop_end:
        dc.w    (BAR_TOP<<8)|$07,$fffe        ; hasta donde llega la barra
        dc.w    $0180,$0000
        dc.w    $ffff,$fffe

        ifd     BENCH
        cnop    0,4
infobuf:    ds.b    512
timing:     ds.l    TIMING_MAX
        endc
