;----------------------------------------------------------------------
; exec.i - offsets y constantes del sistema que usamos.
; Escrito a mano: no dependemos de los includes de NDK.
;----------------------------------------------------------------------

; --- exec.library LVOs ---
_LVODisable         equ -120
_LVOEnable          equ -126
_LVOForbid          equ -132
_LVOPermit          equ -138
_LVOAllocMem        equ -198
_LVOFreeMem         equ -210
_LVOAvailMem        equ -216
_LVOOpenLibrary     equ -552
_LVOCloseLibrary    equ -414
_LVODoIO            equ -456

; --- graphics.library LVOs ---
_LVOLoadView        equ -222
_LVOWaitTOF         equ -270

; --- requisitos de memoria (exec/memory.i) ---
MEMF_ANY            equ 0
MEMF_PUBLIC         equ 1
MEMF_CHIP           equ 2
MEMF_FAST           equ 4
MEMF_CLEAR          equ $00010000
MEMF_LARGEST        equ $00020000
; MEMF_TOTAL ($00080000) es V36+: NO usarlo, el objetivo es Kickstart 1.2/1.3.

; --- struct IOStdReq ---
IO_DEVICE           equ 20
IO_UNIT             equ 24
IO_COMMAND          equ 28
IO_FLAGS            equ 30
IO_ERROR            equ 31
IO_ACTUAL           equ 32
IO_LENGTH           equ 36
IO_DATA             equ 40
IO_OFFSET           equ 44

; --- comandos de trackdisk.device ---
CMD_READ            equ 2
CMD_WRITE           equ 3
CMD_UPDATE          equ 4       ; vacia el buffer de pista: sin esto no se graba
TD_MOTOR            equ 9

; --- custom chips ---
CUSTOM              equ $dff000
DMACON              equ $096
DMACONR             equ $002
VPOSR               equ $004            ; bit 0 = V8
VHPOSR              equ $006            ; V7..V0 | H8..H1 (en color clocks)
INTENAR             equ $01c
INTREQR             equ $01e

; --- CIA-A: el TOD cuenta VSYNC. Solo se lee. ---
CIAA_TODHI          equ $bfea01         ; leer el alto congela hasta leer el bajo
CIAA_TODMID         equ $bfe901
CIAA_TODLO          equ $bfe801
CIAA_PRA            equ $bfe001         ; bit 1 = LED y filtro (0 = encendido)

; --- Blitter ---
BLTCON0             equ $040
BLTCON1             equ $042
BLTAFWM             equ $044            ; y BLTALWM en $046, contiguos
BLTAPT              equ $050
BLTDPT              equ $054
BLTSIZE             equ $058            ; (filas << 6) | palabras por fila
BLTAMOD             equ $064
BLTDMOD             equ $066

; --- Paula ---
AUD0LC              equ $0a0
AUD0LEN             equ $0a4
AUD0PER             equ $0a6
AUD0VOL             equ $0a8
AUD1LC              equ $0b0
AUD1LEN             equ $0b4
AUD1PER             equ $0b6
AUD1VOL             equ $0b8
INTENA              equ $09a
INTREQ              equ $09c
COP1LC              equ $080
COPJMP1             equ $088
COLOR00             equ $180
