;----------------------------------------------------------------------
; video.i - constantes de video compartidas por los reproductores.
; Las rutinas estan en video_code.i, que va al final de cada programa.
;----------------------------------------------------------------------

FB_ROWBYTES   equ 40                  ; bytes de pantalla por fila y plano
FB_ROWS       equ 128                 ; filas logicas (cada una se ve 2 veces)
PLANE_BYTES   equ FB_ROWBYTES*FB_ROWS ; 5120
COPPER_SIZE   equ 4096

DIW_FIRST     equ $2c                 ; primera linea de pantalla
DIW_END       equ DIW_FIRST+256       ; primera linea despues de la pantalla

COP_PAL_BASE  equ 32                  ; bytes del copper list antes de los
                                      ; punteros de bitplane
