;----------------------------------------------------------------------
; player.s - reproductor de A500VP.
;
; Hito 3: carga el bitstream, decodifica el primer paquete (un DELTA desde
; negro con su paleta) y lo muestra fijo, con doblado horizontal por tabla
; y doblado vertical por Copper. Antes de tomar el hardware vuelca el
; framebuffer al disquete, para que el PC lo compare byte a byte con el
; decoder de referencia.
;
; Entrada (desde boot.s): A6 = ExecBase, A1 = IOStdReq de trackdisk,
; A0 = base propia. Codigo independiente de posicion.
;
; Registros que viven todo el programa:
;   a2 = framebuffer   a5 = IOStdReq   a6 = ExecBase
;   d2 = bytes de framebuffer          d6 = bitplanes
;----------------------------------------------------------------------

        include "exec.i"

FB_ROWBYTES   equ 40                  ; bytes de pantalla por fila y plano
FB_ROWS       equ 128                 ; filas logicas (cada una se ve 2 veces)
PLANE_BYTES   equ FB_ROWBYTES*FB_ROWS ; 5120
COPPER_SIZE   equ 4096
DUMP_SECTOR   equ 1719                ; volcado del framebuffer (hasta 40 sect.)
INFO_SECTOR   equ 1759

DIW_FIRST     equ $2c                 ; primera linea de pantalla
DIW_END       equ DIW_FIRST+256       ; primera linea despues de la pantalla

COL_LOAD      equ $0f80               ; naranja: cargando
COL_NOMEM     equ $0f00               ; rojo: sin memoria
COL_DISK      equ $0f0f               ; magenta: fallo de trackdisk
COL_BADHDR    equ $0ff0               ; amarillo: bitstream invalido

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
        move.l  a1,a5
        lea     CUSTOM,a4
        move.w  #COL_LOAD,COLOR00(a4)

        ;--- cargar los datos (a Chip: trackdisk de KS 1.x) ---------
        move.l  hdr_data_len(pc),d2
        beq     badhdr
        add.l   #511,d2
        and.l   #$fffffe00,d2
        move.l  d2,d0
        move.l  #MEMF_CHIP,d1
        jsr     _LVOAllocMem(a6)
        tst.l   d0
        beq     nomem
        move.l  d0,a3                         ; a3 = datos

        move.l  a5,a1
        move.w  #CMD_READ,IO_COMMAND(a1)
        move.l  d2,IO_LENGTH(a1)
        move.l  a3,IO_DATA(a1)
        move.l  hdr_data_off(pc),IO_OFFSET(a1)
        jsr     _LVODoIO(a6)
        tst.l   d0
        bne     diskerr

        ;--- cabecera del bitstream ---------------------------------
        cmp.l   #$41355650,(a3)               ; "A5VP"
        bne     badhdr
        cmp.w   #2,4(a3)                      ; version de formato
        bne     badhdr
        moveq   #0,d6
        move.b  12(a3),d6                     ; bitplanes
        beq     badhdr
        cmp.w   #4,d6
        bhi     badhdr
        tst.l   20(a3)                        ; al menos un paquete
        beq     badhdr

        ;--- primer paquete -----------------------------------------
        lea     32(a3),a0
        move.b  2(a0),d4                      ; op
        move.b  3(a0),d5                      ; flags
        move.w  4(a0),d3                      ; bytes de audio
        addq.l  #6,a0
        btst    #0,d5
        beq.s   .nopal
        lea     palbuf(pc),a1
        moveq   #1,d0
        lsl.w   d6,d0
        subq.w  #1,d0
.pal:   move.w  (a0)+,(a1)+
        dbf     d0,.pal
.nopal:
        add.w   d3,a0                         ; el audio no se usa todavia
        lea     v_delta(pc),a1
        move.l  a0,(a1)
        lea     v_op(pc),a1
        move.b  d4,(a1)

        ;--- framebuffer y copper list, en Chip --------------------
        move.w  d6,d2
        mulu    #PLANE_BYTES,d2
        move.l  d2,d0
        move.l  #MEMF_CHIP|MEMF_CLEAR,d1
        jsr     _LVOAllocMem(a6)
        tst.l   d0
        beq     nomem
        move.l  d0,a2

        move.l  #COPPER_SIZE,d0
        move.l  #MEMF_CHIP,d1
        jsr     _LVOAllocMem(a6)
        tst.l   d0
        beq     nomem
        lea     v_copper(pc),a1
        move.l  d0,(a1)

        ;--- tabla de doblado y decodificacion ----------------------
        lea     dbltab(pc),a3
        bsr     build_table

        move.b  v_op(pc),d0
        bne.s   .nodelta                      ; solo DELTA dibuja
        move.l  v_delta(pc),a0
        bsr     apply_delta                   ; a4 = fin del delta
        sub.l   v_delta(pc),a4
        lea     v_consumed(pc),a1
        move.l  a4,(a1)
.nodelta:

        move.l  v_copper(pc),a0
        lea     palbuf(pc),a1
        bsr     build_copper

        ;--- volcado para verificar desde el PC --------------------
        bsr     dump

        ;--- tomar el hardware -------------------------------------
        ; Sin LoadView(NULL) el servidor de VBL de graphics reinstala su
        ; copper list y nos pisa la pantalla.
        lea     gfxname(pc),a1
        moveq   #0,d0
        jsr     _LVOOpenLibrary(a6)
        move.l  d0,a3
        tst.l   d0
        beq.s   .nogfx
        jsr     _LVOForbid(a6)
        exg     a3,a6                         ; a6 = GfxBase
        sub.l   a1,a1
        jsr     _LVOLoadView(a6)
        jsr     _LVOWaitTOF(a6)
        jsr     _LVOWaitTOF(a6)
        exg     a3,a6                         ; a6 = ExecBase otra vez
.nogfx:
        lea     CUSTOM,a4
        move.w  #$7fff,INTENA(a4)
        move.w  #$7fff,INTREQ(a4)
        move.w  #$7fff,DMACON(a4)
        move.l  v_copper(pc),COP1LC(a4)
        move.w  d0,COPJMP1(a4)                ; strobe
        move.w  #$8380,DMACON(a4)             ; MASTER|RASTER|COPPER
.forever:
        bra.s   .forever

;----------------------------------------------------------------------
; Errores: el color se reescribe en un lazo para que el copper list del
; sistema, que pone COLOR00 una vez por frame, no lo tape.
;----------------------------------------------------------------------
nomem:  move.w  #COL_NOMEM,d0
        bra.s   stop
diskerr:
        move.w  #COL_DISK,d0
        bra.s   stop
badhdr: move.w  #COL_BADHDR,d0
stop:   lea     CUSTOM,a4
.loop:  move.w  d0,COLOR00(a4)
        bra.s   .loop

;----------------------------------------------------------------------
; build_table - tabla de doblado horizontal: 256 palabras, cada bit del
; byte logico repetido dos veces (bit 7 -> bits 15-14).
;   a3 = tabla
;----------------------------------------------------------------------
build_table:
        movem.l d0-d3/a3,-(sp)
        moveq   #0,d0
.byte:  moveq   #0,d2
        moveq   #7,d3
.bit:   lsl.w   #2,d2
        btst    d3,d0
        beq.s   .zero
        addq.w  #3,d2
.zero:  dbf     d3,.bit
        move.w  d2,(a3)+
        addq.w  #1,d0
        cmp.w   #256,d0
        bne.s   .byte
        movem.l (sp)+,d0-d3/a3
        rts

;----------------------------------------------------------------------
; apply_delta - aplica un delta del formato v2 (docs/FORMAT.md).
;   a0 = delta, a2 = framebuffer, a3 = tabla de doblado, d6 = bitplanes
;   devuelve a4 = primer byte despues del delta
;
; Mapa de 16 bytes de filas; por fila marcada, 3 bytes de mascara de
; columnas y, por columna marcada, un byte por plano. Cada byte logico se
; escribe como una palabra en pantalla, via la tabla.
;----------------------------------------------------------------------
apply_delta:
        movem.l d0-d7/a1/a5-a6,-(sp)
        lea     16(a0),a4                     ; a4 = datos de las filas
        move.l  a2,a5                         ; a5 = fila actual, plano 0
        moveq   #15,d5                        ; 16 bytes de mapa de filas
.mbyte: move.b  (a0)+,d4
        moveq   #7,d3
.mbit:  add.b   d4,d4                         ; bit 7 = fila de mas arriba
        bcc.s   .nextrow

        moveq   #0,d2                         ; d2 = mascara: b0 b1 b2 00
        move.b  (a4)+,d2
        lsl.w   #8,d2
        move.b  (a4)+,d2
        swap    d2
        move.b  (a4)+,d2
        lsl.w   #8,d2

        move.l  a5,a6                         ; a6 = columna 0 de la fila
        moveq   #19,d1
.col:   add.l   d2,d2
        bcc.s   .skip
        move.l  a6,a1
        move.w  d6,d0
        subq.w  #1,d0
.plane: moveq   #0,d7
        move.b  (a4)+,d7
        add.w   d7,d7
        move.w  0(a3,d7.w),(a1)
        lea     PLANE_BYTES(a1),a1
        dbf     d0,.plane
.skip:  addq.l  #2,a6
        dbf     d1,.col

.nextrow:
        lea     FB_ROWBYTES(a5),a5
        dbf     d3,.mbit
        dbf     d5,.mbyte
        movem.l (sp)+,d0-d7/a1/a5-a6
        rts

;----------------------------------------------------------------------
; build_copper - arma el copper list.
;   a0 = copper list, a1 = paleta, a2 = framebuffer, d6 = bitplanes
;
; Doblado vertical por modulo: en cada linea de pantalla se alterna
; BPLxMOD entre -40 y 0. El modulo se suma al terminar el fetch de la linea
; (cerca de hpos $D8), asi que con -40 la linea siguiente vuelve a leer la
; misma fila, y con 0 avanza a la proxima. Escribirlo al principio de la
; linea (hpos $07) deja casi una linea entera de margen. Ver DECISIONS.md.
;----------------------------------------------------------------------
build_copper:
        movem.l d0-d3/a0-a1,-(sp)
        move.l  #$008e2c81,(a0)+              ; DIWSTRT
        move.l  #$00902cc1,(a0)+              ; DIWSTOP (256 lineas)
        move.l  #$00920038,(a0)+              ; DDFSTRT
        move.l  #$009400d0,(a0)+              ; DDFSTOP
        move.l  #$01020000,(a0)+              ; BPLCON1
        move.l  #$01040000,(a0)+              ; BPLCON2
        move.l  #$0108ffd8,(a0)+              ; BPL1MOD = -40 (primera linea)
        move.l  #$010affd8,(a0)+              ; BPL2MOD = -40

        move.l  a2,d0                         ; punteros de bitplane
        move.w  #$00e0,d1
        move.w  d6,d2
        subq.w  #1,d2
.bp:    swap    d0
        move.w  d1,(a0)+
        move.w  d0,(a0)+
        addq.w  #2,d1
        swap    d0
        move.w  d1,(a0)+
        move.w  d0,(a0)+
        addq.w  #2,d1
        add.l   #PLANE_BYTES,d0
        dbf     d2,.bp

        moveq   #1,d2                         ; paleta
        lsl.w   d6,d2
        subq.w  #1,d2
        move.w  #$0180,d1
.pal:   move.w  d1,(a0)+
        move.w  (a1)+,(a0)+
        addq.w  #2,d1
        dbf     d2,.pal

        move.w  d6,d0                         ; BPLCON0 = planos<<12 | COLOR
        ror.w   #4,d0
        or.w    #$0200,d0
        move.w  #$0100,(a0)+
        move.w  d0,(a0)+

        move.w  #DIW_FIRST,d3
.line:  cmp.w   #$100,d3                      ; el Copper cuenta 8 bits de linea
        bne.s   .nowrap
        move.l  #$ffdffffe,(a0)+
.nowrap:
        move.w  d3,d0
        lsl.w   #8,d0
        or.w    #$0007,d0
        move.w  d0,(a0)+                      ; WAIT linea, hpos $06
        move.w  #$fffe,(a0)+
        moveq   #-40,d1                       ; primera de cada par: repetir
        btst    #0,d3                         ; DIW_FIRST es par
        beq.s   .rep
        moveq   #0,d1                         ; segunda: avanzar
.rep:   move.w  #$0108,(a0)+
        move.w  d1,(a0)+
        move.w  #$010a,(a0)+
        move.w  d1,(a0)+
        addq.w  #1,d3
        cmp.w   #DIW_END,d3
        bne.s   .line

        move.l  #$fffffffe,(a0)+              ; fin
        movem.l (sp)+,d0-d3/a0-a1
        rts

;----------------------------------------------------------------------
; dump - vuelca el framebuffer y un sector de informacion al disquete.
; Formato en docs/FORMAT.md. Usa a2, d2, d6, a5, a6.
;----------------------------------------------------------------------
dump:
        movem.l d0-d1/a0-a1,-(sp)
        move.l  a5,a1
        move.w  #CMD_WRITE,IO_COMMAND(a1)
        move.l  d2,IO_LENGTH(a1)
        move.l  a2,IO_DATA(a1)
        move.l  #DUMP_SECTOR*512,IO_OFFSET(a1)
        jsr     _LVODoIO(a6)
        moveq   #0,d1
        move.b  IO_ERROR(a5),d1               ; a1 no sobrevive a DoIO

        lea     infobuf(pc),a0
        move.l  #$4642444d,(a0)+              ; "FBDM"
        moveq   #0,d0
        move.w  d6,d0
        move.l  d0,(a0)+                      ; bitplanes
        move.l  a2,(a0)+                      ; direccion del framebuffer
        move.l  v_copper(pc),(a0)+            ; direccion del copper list
        move.l  v_consumed(pc),(a0)+          ; bytes de delta consumidos
        move.l  d1,(a0)+                      ; error al volcar el framebuffer
        lea     palbuf(pc),a1
        moveq   #15,d0
.p:     move.w  (a1)+,(a0)+
        dbf     d0,.p

        move.l  a5,a1
        move.w  #CMD_WRITE,IO_COMMAND(a1)
        move.l  #512,IO_LENGTH(a1)
        lea     infobuf(pc),a0
        move.l  a0,IO_DATA(a1)
        move.l  #INFO_SECTOR*512,IO_OFFSET(a1)
        jsr     _LVODoIO(a6)

        move.l  a5,a1                         ; bajar la pista a disco
        move.w  #CMD_UPDATE,IO_COMMAND(a1)
        clr.l   IO_LENGTH(a1)
        jsr     _LVODoIO(a6)

        move.l  a5,a1                         ; y apagar el motor
        move.w  #TD_MOTOR,IO_COMMAND(a1)
        clr.l   IO_LENGTH(a1)
        jsr     _LVODoIO(a6)
        movem.l (sp)+,d0-d1/a0-a1
        rts

;----------------------------------------------------------------------
; Datos
;----------------------------------------------------------------------
        even
gfxname:    dc.b    "graphics.library",0
        even
v_delta:    dc.l    0
v_copper:   dc.l    0
v_consumed: dc.l    0
v_op:       dc.b    0
        even
palbuf:     ds.w    16
dbltab:     ds.w    256
        cnop    0,4
infobuf:    ds.b    512
