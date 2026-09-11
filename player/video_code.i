;----------------------------------------------------------------------
; video_code.i - rutinas de video compartidas por los reproductores.
; Se incluye al final del codigo (no al principio: seria lo primero que
; se ejecuta). Necesita video.i.
;----------------------------------------------------------------------

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
;   devuelve a0 = primer byte despues del delta
;
; Mapa de 16 bytes de filas; por fila marcada, 3 bytes de mascara de
; columnas y, por columna marcada, un byte por plano. Cada byte logico se
; escribe como una palabra en pantalla, via la tabla.
;----------------------------------------------------------------------
apply_delta:
        movem.l d0-d7/a1/a4-a6,-(sp)
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
        move.l  a4,a0                         ; fin del delta
        movem.l (sp)+,d0-d7/a1/a4-a6
        rts

;----------------------------------------------------------------------
; build_copper - arma un copper list de reproduccion.
;   a0 = copper list, a1 = paleta, a2 = framebuffer, d6 = bitplanes
;
; Doblado vertical por modulo: en cada linea de pantalla se alterna
; BPLxMOD entre -40 y 0. El modulo se suma al terminar el fetch de la linea
; (cerca de hpos $D8), asi que con -40 la linea siguiente vuelve a leer la
; misma fila, y con 0 avanza a la proxima. Escribirlo al principio de la
; linea (hpos $06) deja casi una linea entera de margen. Ver DECISIONS.md.
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

        moveq   #1,d2                         ; paleta (COP_PAL_BASE + 8*planos)
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
; write_palette - copia una paleta a un copper list ya armado.
;   a0 = copper list, a1 = paleta (hasta 16 palabras), d6 = bitplanes
;----------------------------------------------------------------------
write_palette:
        movem.l d0/a0-a1,-(sp)
        move.w  d6,d0
        lsl.w   #3,d0                         ; 8 bytes de punteros por plano
        lea     COP_PAL_BASE+2(a0,d0.w),a0    ; valor del primer MOVE COLOR
        moveq   #1,d0
        lsl.w   d6,d0
        subq.w  #1,d0
.c:     move.w  (a1)+,(a0)
        addq.l  #4,a0
        dbf     d0,.c
        movem.l (sp)+,d0/a0-a1
        rts
