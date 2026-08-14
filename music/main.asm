RASTER_LINE = $f8           ; below the visible text, in the lower border

; ---------------------------------------------------------------------------
;  BASIC line:  10 SYS 2064
; ---------------------------------------------------------------------------
    * = $0801
    !byte $0c,$08,$0a,$00,$9e,$32,$30,$36,$34,$00,$00,$00

    * = $0810
start:
    sei
    lda #$7f                ; no CIA interrupts
    sta $dc0d
    sta $dd0d
    lda $dc0d
    lda $dd0d

    lda $d011               ; raster IRQ, high bit of the line = 0
    and #$7f
    sta $d011
    lda #RASTER_LINE
    sta $d012
    lda #$01
    sta $d01a
    sta $d019

    lda #$35                ; kernal + basic out, I/O in
    sta $01
    lda #<irq
    sta $fffe
    lda #>irq
    sta $ffff

    jsr music_init
    cli
.forever jmp .forever


; ---------------------------------------------------------------------------
;  IRQ, once per frame.  The border shows how much time the player takes.
; ---------------------------------------------------------------------------
irq:
    pha
    txa
    pha
    tya
    pha

    lda #$01
    sta $d019               ; acknowledge

    lda #$0b
    sta $d020
    jsr music_play
    lda #$00
    sta $d020

    pla
    tay
    pla
    tax
    pla
    rti

!source "player.asm"
