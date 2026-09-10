;----------------------------------------------------------------------
; memcheck.s - stage2 del Hito 0.
;
; Lo carga el bootblock desde el sector 2. Mide la memoria libre con
; AvailMem *antes* de tocar nada mas, y la muestra en pantalla con un
; display propio (1 bitplane, copper propio, sin sistema operativo).
;
; Entrada: A6 = ExecBase, A1 = IOStdReq de trackdisk, A0 = base propia.
;
; Codigo independiente de posicion: todas las referencias a datos son
; PC-relativas. Se ejecuta donde AllocMem lo haya puesto (Chip RAM).
;
; NOTA: no se usa MEMF_TOTAL (V36+). Con Kickstart 1.2/1.3 solo hay
; "libre" (suma) y MEMF_LARGEST (bloque contiguo mas grande).
;----------------------------------------------------------------------

        include "exec.i"

BPR         equ 40                  ; bytes por fila (320 px lowres)
SCRH        equ 256                 ; filas PAL
PLANESIZE   equ BPR*SCRH

COL_BG      equ $0002               ; azul muy oscuro
COL_FG      equ $0ff0               ; amarillo
COL_FAIL    equ $0f00

; La medicion se graba ademas en el ultimo sector del disquete (1759), para
; poder leerla desde el PC sin depender de una foto de la pantalla.
RESULT_OFF  equ 1759*512

;----------------------------------------------------------------------
stage2:
        move.l  4.w,a6
        move.l  a1,a5                       ; IOStdReq por si hiciera falta

        ;--- medicion, lo primero de todo -----------------------------
        lea     results(pc),a2
        move.l  #MEMF_CHIP,d1
        jsr     _LVOAvailMem(a6)
        move.l  d0,(a2)+
        move.l  #MEMF_CHIP|MEMF_LARGEST,d1
        jsr     _LVOAvailMem(a6)
        move.l  d0,(a2)+
        move.l  #MEMF_FAST,d1
        jsr     _LVOAvailMem(a6)
        move.l  d0,(a2)+
        move.l  #MEMF_FAST|MEMF_LARGEST,d1
        jsr     _LVOAvailMem(a6)
        move.l  d0,(a2)+

        lea     results(pc),a2
        move.l  (a2),d0                     ; chip libre
        add.l   8(a2),d0                    ; + fast libre
        move.l  d0,16(a2)                   ; = total libre

        ;--- bitplane ------------------------------------------------
        move.l  #PLANESIZE,d0
        move.l  #MEMF_CHIP|MEMF_CLEAR,d1
        jsr     _LVOAllocMem(a6)
        tst.l   d0
        beq     nomem
        move.l  d0,a3                       ; a3 = plano

        ;--- grabar la medicion en el ultimo sector -------------------
        lea     results(pc),a0
        lea     secbuf(pc),a1
        addq.l  #4,a1                       ; despues del magic "MEMR"
        moveq   #4,d0
.fill:  move.l  (a0)+,(a1)+
        dbf     d0,.fill
        lea     stage2(pc),a0
        move.l  a0,(a1)+                    ; +24: donde quedo stage2
        move.l  a3,(a1)+                    ; +28: donde quedo el bitplane

        move.l  a5,a1
        move.w  #CMD_WRITE,IO_COMMAND(a1)
        move.l  #512,IO_LENGTH(a1)
        lea     secbuf(pc),a0
        move.l  a0,IO_DATA(a1)
        move.l  #RESULT_OFF,IO_OFFSET(a1)
        jsr     _LVODoIO(a6)
        moveq   #0,d3
        move.b  IO_ERROR(a1),d3             ; 0 = grabado; $1C = protegido

        move.l  a5,a1                       ; CMD_UPDATE: bajar la pista a disco
        move.w  #CMD_UPDATE,IO_COMMAND(a1)
        clr.l   IO_LENGTH(a1)
        jsr     _LVODoIO(a6)

        move.l  a5,a1                       ; y apagar el motor
        move.w  #TD_MOTOR,IO_COMMAND(a1)
        clr.l   IO_LENGTH(a1)
        jsr     _LVODoIO(a6)

        ;--- dibujar -------------------------------------------------
        lea     title(pc),a0
        moveq   #3,d0
        moveq   #2,d1
        bsr     puts

        lea     rowlabels(pc),a0
        lea     results(pc),a2
        moveq   #4,d7                       ; 5 filas
        moveq   #6,d6                       ; fila de texto inicial

.rowloop:
        move.l  d6,d1
        moveq   #3,d0
        bsr     puts                        ; a0 -> siguiente etiqueta

        move.l  a0,-(sp)
        move.l  (a2)+,d5                    ; valor medido

        move.l  d5,d0
        lea     hexbuf(pc),a1
        bsr     hex8
        lea     hexbuf(pc),a0
        move.l  d6,d1
        moveq   #15,d0
        bsr     puts

        move.l  d5,d0
        lsr.l   #8,d0                       ; bytes -> KB
        lsr.l   #2,d0
        lea     decbuf(pc),a1
        bsr     dec4
        lea     decbuf(pc),a0
        move.l  d6,d1
        moveq   #26,d0
        bsr     puts

        move.l  (sp)+,a0
        addq.l  #2,d6
        dbf     d7,.rowloop

        ; resultado de la grabacion en disco (0 = ok, $1C = protegido)
        lea     savelabel(pc),a0
        moveq   #3,d0
        moveq   #17,d1
        bsr     puts
        move.l  d3,d0
        lea     hexbuf(pc),a1
        bsr     hex8
        lea     hexbuf(pc),a0
        moveq   #15,d0
        moveq   #17,d1
        bsr     puts

        ;--- tomar el hardware ---------------------------------------
        ; Sin LoadView(NULL) el servidor de VBL de graphics reinstala su
        ; propio copper list y nos pisa la pantalla.
        lea     gfxname(pc),a1
        moveq   #0,d0
        jsr     _LVOOpenLibrary(a6)
        move.l  d0,a4
        tst.l   d0
        beq.s   .nogfx

        jsr     _LVOForbid(a6)
        exg     a4,a6                       ; a6 = GfxBase
        sub.l   a1,a1
        jsr     _LVOLoadView(a6)
        jsr     _LVOWaitTOF(a6)
        jsr     _LVOWaitTOF(a6)
        exg     a4,a6                       ; a6 = ExecBase otra vez
.nogfx:

        ; puntero de bitplane dentro del copper list
        lea     cop_bplpt(pc),a1
        move.l  a3,d0
        swap    d0
        move.w  d0,2(a1)
        swap    d0
        move.w  d0,6(a1)

        lea     CUSTOM,a4
        move.w  #$7fff,INTENA(a4)
        move.w  #$7fff,INTREQ(a4)
        move.w  #$7fff,DMACON(a4)
        lea     copper(pc),a0
        move.l  a0,COP1LC(a4)
        move.w  d0,COPJMP1(a4)              ; strobe
        move.w  #$8380,DMACON(a4)           ; MASTER|RASTER|COPPER
.forever:
        bra.s   .forever

nomem:
        lea     CUSTOM,a4
        move.w  #COL_FAIL,COLOR00(a4)
.stop:  bra.s   .stop

;----------------------------------------------------------------------
; puts - escribe una cadena ASCII en el plano.
;   a0 = cadena terminada en 0   (a la vuelta apunta tras el 0)
;   d0 = columna de caracter, d1 = fila de caracter, a3 = plano
;----------------------------------------------------------------------
puts:
        movem.l d0-d5/a1-a2,-(sp)
        move.l  d1,d2
        mulu    #BPR*8,d2
        add.l   d0,d2
        move.l  a3,a1
        add.l   d2,a1
.pchar:
        moveq   #0,d0
        move.b  (a0)+,d0
        beq.s   .pdone
        bsr.s   findglyph
        move.l  a1,-(sp)
        moveq   #7,d4
.prow:  move.b  (a2)+,(a1)
        lea     BPR(a1),a1
        dbf     d4,.prow
        move.l  (sp)+,a1
        addq.l  #1,a1
        bra.s   .pchar
.pdone:
        movem.l (sp)+,d0-d5/a1-a2
        rts

; d0 = caracter -> a2 = 8 bytes de glifo. Usa d5 (lo salva puts).
findglyph:
        lea     font(pc),a2
.fg:    move.b  (a2),d5
        beq.s   .fgend
        cmp.b   d0,d5
        beq.s   .fgend
        lea     9(a2),a2
        bra.s   .fg
.fgend: addq.l  #1,a2                       ; tras el 0 final va el glifo vacio
        rts

;----------------------------------------------------------------------
; hex8 - d0 = valor, a1 = destino de 8 caracteres. Preserva todo.
;----------------------------------------------------------------------
hex8:
        movem.l d0-d2/a1,-(sp)
        moveq   #7,d2
.hloop: move.l  d0,d1
        and.l   #15,d1
        cmp.b   #10,d1
        blt.s   .hdig
        add.b   #55,d1                      ; 'A'-10
        bra.s   .hst
.hdig:  add.b   #48,d1                      ; '0'
.hst:   move.b  d1,0(a1,d2.w)
        lsr.l   #4,d0
        dbf     d2,.hloop
        movem.l (sp)+,d0-d2/a1
        rts

;----------------------------------------------------------------------
; dec4 - d0 = valor (0..9999), a1 = destino de 4 digitos. Preserva todo.
;----------------------------------------------------------------------
dec4:
        movem.l d0-d2/a1,-(sp)
        and.l   #$0000ffff,d0
        moveq   #3,d2
.dloop: divu    #10,d0
        move.l  d0,d1
        clr.w   d1
        swap    d1                          ; d1 = resto
        add.b   #48,d1                      ; '0'
        move.b  d1,0(a1,d2.w)
        and.l   #$0000ffff,d0               ; quedarse con el cociente
        dbf     d2,.dloop
        movem.l (sp)+,d0-d2/a1
        rts

;----------------------------------------------------------------------
; Datos
;----------------------------------------------------------------------
        even
gfxname:    dc.b    "graphics.library",0

title:      dc.b    "A500VP HITO 0",0

rowlabels:  dc.b    "CHIP FREE",0
            dc.b    "CHIP LARG",0
            dc.b    "FAST FREE",0
            dc.b    "FAST LARG",0
            dc.b    "TOTAL FREE",0

savelabel:  dc.b    "SAVE ERR",0

hexbuf:     dc.b    "00000000",0
decbuf:     dc.b    "0000 KB",0

        even
results:    ds.l    5           ; chip libre, chip mayor, fast libre,
                                ; fast mayor, total libre

; Sector de resultado, tal como queda en el disco. Ver docs/FORMAT.md.
        cnop    0,4
secbuf:     dc.b    "MEMR"
            ds.b    508

;----------------------------------------------------------------------
; Copper list. Vive dentro de la imagen de stage2, que el bootblock
; cargo en Chip RAM, asi que el Copper la puede leer.
;----------------------------------------------------------------------
        cnop    0,4
copper:
        dc.w    $008e,$2c81                 ; DIWSTRT
        dc.w    $0090,$2cc1                 ; DIWSTOP  (256 lineas PAL)
        dc.w    $0092,$0038                 ; DDFSTRT
        dc.w    $0094,$00d0                 ; DDFSTOP
        dc.w    $0102,$0000                 ; BPLCON1
        dc.w    $0104,$0000                 ; BPLCON2
        dc.w    $0108,$0000                 ; BPL1MOD
        dc.w    $010a,$0000                 ; BPL2MOD
cop_bplpt:
        dc.w    $00e0,$0000                 ; BPL1PTH  (parcheado en runtime)
        dc.w    $00e2,$0000                 ; BPL1PTL  (parcheado en runtime)
        dc.w    $0180,COL_BG
        dc.w    $0182,COL_FG
        dc.w    $0100,$1200                 ; BPLCON0: 1 plano, COLOR on
        dc.w    $ffff,$fffe

;----------------------------------------------------------------------
; Fuente 5x7 en celdas de 8x8. Cada entrada: caracter + 8 filas.
; La lista termina con 0 seguido del glifo vacio, que se usa para el
; espacio y para cualquier caracter que no este en la tabla.
;----------------------------------------------------------------------
font:
        dc.b '0',%01110000,%10001000,%10011000,%10101000,%11001000,%10001000,%01110000,0
        dc.b '1',%00100000,%01100000,%00100000,%00100000,%00100000,%00100000,%01110000,0
        dc.b '2',%01110000,%10001000,%00001000,%00010000,%00100000,%01000000,%11111000,0
        dc.b '3',%11111000,%00010000,%00100000,%00010000,%00001000,%10001000,%01110000,0
        dc.b '4',%00010000,%00110000,%01010000,%10010000,%11111000,%00010000,%00010000,0
        dc.b '5',%11111000,%10000000,%11110000,%00001000,%00001000,%10001000,%01110000,0
        dc.b '6',%00110000,%01000000,%10000000,%11110000,%10001000,%10001000,%01110000,0
        dc.b '7',%11111000,%00001000,%00010000,%00100000,%01000000,%01000000,%01000000,0
        dc.b '8',%01110000,%10001000,%10001000,%01110000,%10001000,%10001000,%01110000,0
        dc.b '9',%01110000,%10001000,%10001000,%01111000,%00001000,%00010000,%01100000,0
        dc.b 'A',%01110000,%10001000,%10001000,%11111000,%10001000,%10001000,%10001000,0
        dc.b 'B',%11110000,%10001000,%10001000,%11110000,%10001000,%10001000,%11110000,0
        dc.b 'C',%01110000,%10001000,%10000000,%10000000,%10000000,%10001000,%01110000,0
        dc.b 'D',%11100000,%10010000,%10001000,%10001000,%10001000,%10010000,%11100000,0
        dc.b 'E',%11111000,%10000000,%10000000,%11110000,%10000000,%10000000,%11111000,0
        dc.b 'F',%11111000,%10000000,%10000000,%11110000,%10000000,%10000000,%10000000,0
        dc.b 'G',%01110000,%10001000,%10000000,%10111000,%10001000,%10001000,%01111000,0
        dc.b 'H',%10001000,%10001000,%10001000,%11111000,%10001000,%10001000,%10001000,0
        dc.b 'I',%01110000,%00100000,%00100000,%00100000,%00100000,%00100000,%01110000,0
        dc.b 'K',%10001000,%10010000,%10100000,%11000000,%10100000,%10010000,%10001000,0
        dc.b 'L',%10000000,%10000000,%10000000,%10000000,%10000000,%10000000,%11111000,0
        dc.b 'O',%01110000,%10001000,%10001000,%10001000,%10001000,%10001000,%01110000,0
        dc.b 'P',%11110000,%10001000,%10001000,%11110000,%10000000,%10000000,%10000000,0
        dc.b 'R',%11110000,%10001000,%10001000,%11110000,%10100000,%10010000,%10001000,0
        dc.b 'S',%01111000,%10000000,%10000000,%01110000,%00001000,%00001000,%11110000,0
        dc.b 'T',%11111000,%00100000,%00100000,%00100000,%00100000,%00100000,%00100000,0
        dc.b 'V',%10001000,%10001000,%10001000,%10001000,%10001000,%01010000,%00100000,0
        dc.b 0
blankglyph:
        dc.b 0,0,0,0,0,0,0,0
        even
