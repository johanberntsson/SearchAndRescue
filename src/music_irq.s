; The music interrupt, and the one write that installs it.
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

            .extern music_play, music_enabled, music_chain

CINV:       .equlab 0x0314          ; the ROM's IRQ vector, in RAM

            .section code,text
            .public music_hook, music_irq

; Once, from music_begin. Under SEI, because an interrupt taken between the
; two halves of the vector would jump into nothing.
music_hook: sei
            lda     CINV
            sta     music_chain
            lda     CINV+1
            sta     music_chain+1
            lda     #.byte0 music_irq
            sta     CINV
            lda     #.byte1 music_irq
            sta     CINV+1
            cli
            rts

; Fifty times a second.
music_irq:  lda     music_enabled
            beq     out$

            ; The player reads its pointers out of zero page, and zero page is
            ; wherever the base page register says it is. The ROM sets B for
            ; its own use and restores it on the way out, so what B holds here
            ; is the ROM's business: put it back to 0 for the player and
            ; return it afterwards.
            tba
            pha
            lda     #0
            tab
            jsr     music_play
            pla
            tab

out$:       jmp     (music_chain)
