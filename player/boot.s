;----------------------------------------------------------------------
; boot.s - bootblock de A500VP (sectores 0-1 del ADF, 1024 bytes)
;
; Unica tarea: cargar el reproductor desde sectores fijos con CMD_READ
; y saltar a el. La longitud la escribe mkadf en la cabecera.
;
; Entrada (convencion del Kickstart):
;   A1 = IOStdReq de trackdisk.device, unidad 0, ya abierto
;   A6 = ExecBase   (igual lo recargamos de $4, es mas seguro)
;
; Salida hacia stage2:
;   A6 = ExecBase, A1 = el mismo IOStdReq, A0 = base de stage2
;
; Colores de borde (diagnostico visible sin depurador):
;   azul   = el bootblock arranco
;   verde  = stage2 cargado, saltando
;   rojo   = fallo (sin memoria, o error de lectura)
;----------------------------------------------------------------------

        include "exec.i"

COL_ALIVE   equ $000F           ; azul
COL_OK      equ $00F0           ; verde
COL_FAIL    equ $0F00           ; rojo

STAGE2_OFF  equ 1024            ; stage2 empieza en el sector 2

;----------------------------------------------------------------------
; Cabecera del bootblock. Ver docs/FORMAT.md.
;   0  "DOS",0
;   4  checksum        (lo calcula mkadf)
;   8  longitud stage2 (la escribe mkadf, multiplo de 512)
;  12  codigo
;----------------------------------------------------------------------
bootblock:
        dc.b    "DOS",0
bb_checksum:
        dc.l    0
bb_stage2len:
        dc.l    0

bb_code:
        move.l  4.w,a6                      ; ExecBase
        lea     CUSTOM,a4
        move.w  #COL_ALIVE,COLOR00(a4)

        move.l  a1,a5                       ; a5 = IOStdReq
        move.l  bb_stage2len(pc),d2         ; d2 = bytes a leer
        beq.s   .fail

        ; --- memoria para stage2 -------------------------------------
        ; Chip obligatorio: en KS 1.2/1.3 trackdisk solo lee a Chip RAM.
        move.l  d2,d0
        move.l  #MEMF_CHIP|MEMF_CLEAR,d1
        jsr     _LVOAllocMem(a6)
        tst.l   d0
        beq.s   .fail
        move.l  d0,a3                       ; a3 = base de stage2

        ; --- leer stage2 ---------------------------------------------
        move.l  a5,a1
        move.w  #CMD_READ,IO_COMMAND(a1)
        move.l  d2,IO_LENGTH(a1)
        move.l  a3,IO_DATA(a1)
        move.l  #STAGE2_OFF,IO_OFFSET(a1)
        jsr     _LVODoIO(a6)
        tst.l   d0
        bne.s   .fail

        ; --- apagar el motor -----------------------------------------
        move.l  a5,a1
        move.w  #TD_MOTOR,IO_COMMAND(a1)
        clr.l   IO_LENGTH(a1)
        jsr     _LVODoIO(a6)

        move.w  #COL_OK,COLOR00(a4)
        move.l  a5,a1                       ; a1 = IOStdReq
        move.l  a3,a0                       ; a0 = base de stage2
        move.l  4.w,a6
        jmp     (a3)

.fail:
        move.w  #COL_FAIL,COLOR00(a4)
.stop:
        bra.s   .stop
