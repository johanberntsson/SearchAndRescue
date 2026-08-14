; The audio interrupt, and the one write that installs it.
;
; Both are here rather than in C because one is an interrupt handler and the
; other rewrites a vector the interrupt reads.
;
; **This chains rather than taking the vector outright.** The C65 ROM's IRQ
; entry at $FA23 pushes A, X, Y, Z and the base page register and then jumps
; through $0314, so a handler reached from there may use every register freely
; and leaves by jumping to whatever was in $0314 before. Everything the ROM
; does with the interrupt -- its raster compare, the keyboard scan, the jiffy
; clock -- goes on happening, and nothing here has to know which raster line
; the ROM asked for. Taking the vector at $FFFE instead means owning all of
; that, and it means knowing that the ROM's exit pulls FIVE bytes and not the
; C64's three.
;
; Two things hang off it and never at the same time: the title music, which is
; the menus', and the engine note, which is the flight's.

            .extern music_play, music_enabled
            .extern engine_tick, engine_on
            .extern audio_chain

CINV:       .equlab 0x0314          ; the ROM's IRQ vector, in RAM

            .section code,text
            .public audio_hook, audio_irq

; Once, from audio_begin. Under SEI, because an interrupt taken between the
; two halves of the vector would jump into nothing.
audio_hook: sei
            lda     CINV
            sta     audio_chain
            lda     CINV+1
            sta     audio_chain+1
            lda     #.byte0 audio_irq
            sta     CINV
            lda     #.byte1 audio_irq
            sta     CINV+1
            cli
            rts

; Fifty times a second. The common case in a flight is one byte compare and a
; jump, and in the menus it is the player and nothing else.
audio_irq:  lda     music_enabled
            ora     engine_on
            beq     out$

            ; Zero page is wherever the base page register says it is, and the
            ; player keeps its pointers there. The ROM sets B for its own use
            ; and restores it on the way out, so what B holds here is the
            ; ROM's business: put it back to 0 for the duration and return it.
            tba
            pha
            lda     #0
            tab

            lda     music_enabled
            beq     engine$
            jsr     music_play
engine$:    lda     engine_on
            beq     done$
            jsr     engine_tick

done$:      pla
            tab

out$:       jmp     (audio_chain)
