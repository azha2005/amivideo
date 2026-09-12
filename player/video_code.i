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
; DELTA_COLS \1 - un grupo de d1+1 columnas, con los \1 planos
; desenrollados. Desenrollar saca el dbf y el lea de cada plano (~22 ciclos
; por plano y por columna marcada), que son el grueso de los 177,3 ciclos
; por columna que midio el Hito 4. El desplazamiento del plano entra en los
; 16 bits de d16(An), asi que la escritura va directo sin mover a1.
DELTA_COLS  macro
.col\@: add.l   d2,d2
        bcc.s   .skip\@
        move.l  a6,a1
adp_n   set     0
        rept    \1
        moveq   #0,d7
        move.b  (a4)+,d7
        add.w   d7,d7
        move.w  0(a3,d7.w),adp_n*PLANE_BYTES(a1)
adp_n   set     adp_n+1
        endr
.skip\@:
        addq.l  #2,a6
        dbf     d1,.col\@
        endm

; DELTA_ROWS \1 - el lazo de filas para una cantidad fija de planos.
; Las 20 columnas se recorren en dos grupos de 8 y uno de 4: si los 8 bits
; de arriba de la mascara estan en cero se saltan las 8 columnas de una,
; en vez de pagar ~36 ciclos por columna caminando bits (720 por fila).
DELTA_ROWS  macro
        moveq   #15,d5                        ; 16 bytes de mapa de filas
.mbyte\@:
        move.b  (a0)+,d4
        moveq   #7,d3
.mbit\@:
        add.b   d4,d4                         ; bit 7 = fila de mas arriba
        bcc     .nextrow\@

        moveq   #0,d2                         ; d2 = mascara: b0 b1 b2 00
        move.b  (a4)+,d2
        lsl.w   #8,d2
        move.b  (a4)+,d2
        swap    d2
        move.b  (a4)+,d2
        lsl.w   #8,d2

        move.l  a5,a6                         ; a6 = columna 0 de la fila
        cmp.l   #$00ffffff,d2                 ; primeras 8 sin marcar?
        bhi.s   .g1\@
        lsl.l   #8,d2
        lea     16(a6),a6
        bra.s   .g1end\@
.g1\@:  moveq   #7,d1
        DELTA_COLS  \1
.g1end\@:
        cmp.l   #$00ffffff,d2                 ; segundas 8 sin marcar?
        bhi.s   .g2\@
        lsl.l   #8,d2
        lea     16(a6),a6
        bra.s   .g2end\@
.g2\@:  moveq   #7,d1
        DELTA_COLS  \1
.g2end\@:
        moveq   #3,d1                         ; las ultimas 4
        DELTA_COLS  \1

.nextrow\@:
        lea     FB_ROWBYTES(a5),a5
        dbf     d3,.mbit\@
        dbf     d5,.mbyte\@
        endm

apply_delta:
        movem.l d0-d7/a1/a4-a6,-(sp)
        lea     16(a0),a4                     ; a4 = datos de las filas
        move.l  a2,a5                         ; a5 = fila actual, plano 0
        cmp.w   #3,d6                         ; los dos casos que se usan
        beq     .three
        cmp.w   #4,d6
        beq     .four

        moveq   #15,d5                        ; generico: 1 o 2 planos
.mbyte: move.b  (a0)+,d4
        moveq   #7,d3
.mbit:  add.b   d4,d4
        bcc.s   .nextrow

        moveq   #0,d2
        move.b  (a4)+,d2
        lsl.w   #8,d2
        move.b  (a4)+,d2
        swap    d2
        move.b  (a4)+,d2
        lsl.w   #8,d2

        move.l  a5,a6
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
        bra     .done                         ; las macros no entran en bra.s

.three: DELTA_ROWS  3
        bra     .done
.four:  DELTA_ROWS  4
.done:  move.l  a4,a0                         ; fin del delta
        movem.l (sp)+,d0-d7/a1/a4-a6
        rts

;----------------------------------------------------------------------
; build_copper - arma un copper list de reproduccion.
;   a0 = copper list, a2 = framebuffer, d6 = bitplanes
; Los colores quedan en negro: los pone write_palette. Usa las franjas
; que fijo set_bands, y anota en vc_bandoff donde quedo cada una.
;
; Franjas de paleta: en la primera linea de pantalla de cada franja, el
; WAIT de hpos $06 sigue con los MOVE de COLOR01..n-1 y despues con los de
; modulo. Con 8 colores son 9 MOVE (36 color clocks): terminan hacia el
; color clock 44, y la imagen empieza en el 64. El color 0 no se toca: es
; tambien el del borde.
;
; Doblado vertical por modulo: en cada linea de pantalla se alterna
; BPLxMOD entre -40 y 0. El modulo se suma al terminar el fetch de la linea
; (cerca de hpos $D8), asi que con -40 la linea siguiente vuelve a leer la
; misma fila, y con 0 avanza a la proxima. Escribirlo al principio de la
; linea (hpos $06) deja casi una linea entera de margen. Ver DECISIONS.md.
;----------------------------------------------------------------------
build_copper:
        movem.l d0-d5/a0-a3,-(sp)
        move.l  a0,a3                         ; a3 = principio del copper list
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

        lea     vc_bandoff(pc),a1             ; la franja 0 va en la cabecera
        move.l  a0,d0                         ; (COP_PAL_BASE + 8*planos)
        sub.l   a3,d0
        addq.w  #2,d0
        move.w  d0,(a1)
        moveq   #1,d2                         ; COLOR00..n-1, en negro
        lsl.w   d6,d2
        subq.w  #1,d2
        move.w  #$0180,d1
.pal:   move.w  d1,(a0)+
        clr.w   (a0)+
        addq.w  #2,d1
        dbf     d2,.pal

        move.w  d6,d0                         ; BPLCON0 = planos<<12 | COLOR
        ror.w   #4,d0
        or.w    #$0200,d0
        move.w  #$0100,(a0)+
        move.w  d0,(a0)+

        moveq   #1,d5                         ; d5 = proxima franja
        move.w  #$7fff,d4                     ; d4 = su primera linea
        cmp.w   vc_nbands(pc),d5
        bhs.s   .nob
        move.w  vc_y0(pc),d4
        add.w   vc_brows(pc),d4
        add.w   d4,d4
        add.w   #DIW_FIRST,d4
.nob:
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
        cmp.w   d4,d3                         ; empieza una franja
        bne.s   .nocol
        move.l  a0,d0
        sub.l   a3,d0
        addq.w  #2,d0
        move.w  d5,d1
        add.w   d1,d1
        move.w  d0,0(a1,d1.w)                 ; offset del valor de COLOR01
        moveq   #1,d2
        lsl.w   d6,d2
        subq.w  #2,d2                         ; n-1 colores
        move.w  #$0182,d1
.bc:    move.w  d1,(a0)+
        clr.w   (a0)+
        addq.w  #2,d1
        dbf     d2,.bc
        addq.w  #1,d5                         ; y la franja siguiente
        move.w  #$7fff,d0
        cmp.w   vc_nbands(pc),d5
        bhs.s   .setnx
        move.w  vc_brows(pc),d0
        add.w   d0,d0
        add.w   d4,d0
.setnx: move.w  d0,d4
.nocol:
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
        movem.l (sp)+,d0-d5/a0-a3
        rts

;----------------------------------------------------------------------
; write_palette - copia las paletas de todas las franjas a un copper list
; ya armado por build_copper.
;   a0 = copper list, a1 = paletas (franjas x colores palabras, como vienen
;   en el paquete). El color 0 se escribe solo en la cabecera: es el mismo
;   en todas las franjas (FORMAT.md).
;----------------------------------------------------------------------
write_palette:
        movem.l d0-d3/a1-a3,-(sp)
        lea     vc_bandoff(pc),a2
        move.w  vc_ncolors(pc),d1
        move.w  (a2)+,d0                      ; franja 0: en la cabecera
        lea     0(a0,d0.w),a3
        move.w  d1,d2
        subq.w  #1,d2
.c0:    move.w  (a1)+,(a3)
        addq.l  #4,a3
        dbf     d2,.c0
        move.w  vc_nbands(pc),d3
        subq.w  #2,d3                         ; franjas 1..
        bmi.s   .done
.band:  move.w  (a2)+,d0
        lea     0(a0,d0.w),a3
        addq.l  #2,a1                         ; sin el color 0
        move.w  d1,d2
        subq.w  #2,d2
.cb:    move.w  (a1)+,(a3)
        addq.l  #4,a3
        dbf     d2,.cb
        dbf     d3,.band
.done:  movem.l (sp)+,d0-d3/a1-a3
        rts

;----------------------------------------------------------------------
; set_bands - lee las franjas de paleta de la cabecera del bitstream.
;   a0 = cabecera. Devuelve d0 = palabras de paleta de un paquete
;   (franjas x colores), o 0 si el Copper no puede con lo que pide.
;----------------------------------------------------------------------
set_bands:
        movem.l d1-d3/a1,-(sp)
        lea     vc_brows(pc),a1
        moveq   #0,d1
        move.b  29(a0),d1                     ; filas por franja
        move.w  d1,(a1)
        move.w  16(a0),d2                     ; y0
        move.w  d2,vc_y0-vc_brows(a1)
        moveq   #1,d0                         ; franjas
        tst.w   d1
        beq.s   .one
        move.w  18(a0),d3                     ; y1
        sub.w   d2,d3                         ; filas activas
        ble.s   .one
        add.w   d1,d3
        subq.w  #1,d3
        ext.l   d3
        divu    d1,d3                         ; hacia arriba
        move.w  d3,d0
.one:   move.w  d0,vc_nbands-vc_brows(a1)
        moveq   #0,d2
        move.b  13(a0),d2                     ; colores
        move.w  d2,vc_ncolors-vc_brows(a1)
        cmp.w   #1,d0
        beq.s   .ok
        cmp.w   #MAX_BANDS,d0
        bhi.s   .bad
        cmp.w   #8,d2                         ; 16 colores: no llega
        bhi.s   .bad
.ok:    mulu    d2,d0
        bra.s   .out
.bad:   moveq   #0,d0
.out:   movem.l (sp)+,d1-d3/a1
        rts

;----------------------------------------------------------------------
; Estado de las franjas de paleta (lo fija set_bands, lo usa todo lo
; demas). Los dos copper lists tienen la misma forma: una sola tabla.
;----------------------------------------------------------------------
        even
vc_brows:   dc.w    0                         ; filas logicas por franja
vc_y0:      dc.w    0                         ; primera fila activa
vc_nbands:  dc.w    1                         ; franjas
vc_ncolors: dc.w    8                         ; colores por franja
vc_bandoff: ds.w    MAX_BANDS                 ; offset del primer valor de
                                              ; color de cada franja
