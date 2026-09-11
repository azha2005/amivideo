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
V_NCOLM1    equ 108     ; w  colores - 1
V_STAMP0    equ 110     ; 8  VBL.l, linea.w, color clock.w
V_STAMP1    equ 118     ; 8
V_MAXDEC    equ 126     ; l  peor decodificacion, en color clocks
V_MAXDECF   equ 130     ; l  y en que frame
V_NDELTA    equ 134     ; l  deltas medidos
VARS_SIZE   equ 138

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
        cmp.w   #2,4(a0)                      ; version de formato
        bne     badhdr
        moveq   #0,d6
        move.b  12(a0),d6                     ; d6 = bitplanes
        beq     badhdr
        cmp.w   #4,d6
        bhi     badhdr
        tst.l   20(a0)                        ; al menos un paquete
        beq     badhdr
        moveq   #1,d0
        lsl.w   d6,d0
        subq.w  #1,d0
        move.w  d0,V_NCOLM1(a4)

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
        lea     palbuf(pc),a1                 ; ceros hasta el primer delta
        move.l  V_COP(a4),a0
        move.l  V_FB(a4),a2
        bsr     build_copper
        move.l  V_COP+4(a4),a0
        move.l  V_FB+4(a4),a2
        bsr     build_copper

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
        move.l  V_COP(a4),COP1LC(a0)
        move.w  d0,COPJMP1(a0)
        move.w  #$8380,DMACON(a0)             ; MASTER|RASTER|COPPER
        move.w  #$c020,INTENA(a0)             ; INTEN|VERTB

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
        btst    #0,3(a5)                      ; paleta nueva
        beq.s   .nopal
        lea     palbuf(pc),a1
        move.w  V_NCOLM1(a4),d0
.cpal:  move.w  (a0)+,(a1)+
        dbf     d0,.cpal
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
        lea     palbuf(pc),a1
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
        tst.w   V_PENDING(a1)
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
palbuf:     ds.w    16
dbltab:     ds.w    256

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
