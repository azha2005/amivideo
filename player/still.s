;----------------------------------------------------------------------
; still.s - disco de prueba del Hito 3: un frame fijo.
;
; Carga el bitstream, decodifica el primer paquete (un DELTA desde negro
; con su paleta) y lo muestra fijo, con doblado horizontal por tabla y
; doblado vertical por Copper. Antes de tomar el hardware vuelca el
; framebuffer al disquete, para que el PC lo compare byte a byte con el
; decoder de referencia (build.ps1 still). Queda como prueba de regresion
; del decodificador en ensamblador.
;
; Entrada (desde boot.s): A6 = ExecBase, A1 = IOStdReq de trackdisk,
; A0 = base propia. Codigo independiente de posicion.
;
; Registros que viven todo el programa:
;   a2 = framebuffer   a5 = IOStdReq   a6 = ExecBase
;   d2 = bytes de framebuffer          d6 = bitplanes
;----------------------------------------------------------------------

        include "exec.i"
        include "video.i"

DUMP_SECTOR   equ 1680                ; volcado del framebuffer (hasta 40 sect.)
COPDUMP_SECTOR equ 1720               ; volcado del copper list (16 sectores)
INFO_SECTOR   equ 1759

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
        cmp.w   #4,4(a3)                      ; version de formato
        bne     badhdr
        moveq   #0,d6
        move.b  12(a3),d6                     ; bitplanes
        beq     badhdr
        cmp.w   #4,d6
        bhi     badhdr
        tst.l   20(a3)                        ; al menos un paquete
        beq     badhdr
        move.l  a3,a0                         ; franjas de paleta
        bsr     set_bands
        tst.w   d0
        beq     badhdr
        add.w   d0,d0
        lea     v_palbytes(pc),a1
        move.w  d0,(a1)

        ;--- primer paquete -----------------------------------------
        lea     32(a3),a0
        move.b  2(a0),d4                      ; op
        move.b  3(a0),d5                      ; flags
        move.w  4(a0),d3                      ; bytes de audio
        addq.l  #6,a0
        btst    #0,d5
        beq.s   .nopal
        lea     v_palptr(pc),a1               ; la paleta se usa donde esta
        move.l  a0,(a1)
        add.w   v_palbytes(pc),a0
.nopal:
        add.w   d3,a0                         ; el audio no se usa
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
        bsr     apply_delta                   ; a0 = fin del delta
        sub.l   v_delta(pc),a0
        lea     v_consumed(pc),a1
        move.l  a0,(a1)
.nodelta:

        move.l  v_copper(pc),a0
        bsr     build_copper
        move.l  v_palptr(pc),d0               ; el primer paquete siempre trae
        beq.s   .nopw                         ; paleta, pero por las dudas
        move.l  d0,a1
        bsr     write_palette
.nopw:

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
        move.b  IO_ERROR(a5),d1               ; a1 no sobrevive a DoIO,
        move.l  d1,-(sp)                      ; ni d1: a la pila

        move.l  a5,a1                         ; el copper list, entero
        move.w  #CMD_WRITE,IO_COMMAND(a1)
        move.l  #COPPER_SIZE,IO_LENGTH(a1)
        move.l  v_copper(pc),IO_DATA(a1)
        move.l  #COPDUMP_SECTOR*512,IO_OFFSET(a1)
        jsr     _LVODoIO(a6)
        moveq   #0,d0
        move.b  IO_ERROR(a5),d0
        move.l  d0,-(sp)

        lea     infobuf(pc),a0
        move.l  #$4642444d,(a0)+              ; "FBDM"
        moveq   #0,d0
        move.w  d6,d0
        move.l  d0,(a0)+                      ; bitplanes
        move.l  a2,(a0)+                      ; direccion del framebuffer
        move.l  v_copper(pc),(a0)+            ; direccion del copper list
        move.l  v_consumed(pc),(a0)+          ; bytes de delta consumidos
        move.l  4(sp),(a0)+                   ; error al volcar el framebuffer
        move.l  (sp)+,(a0)+                   ; error al volcar el copper list
        addq.l  #4,sp

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

        include "video_code.i"

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
v_palptr:   dc.l    0                   ; paletas del primer paquete
v_palbytes: dc.w    0                   ; y su tamano
dbltab:     ds.w    256
        cnop    0,4
infobuf:    ds.b    512
