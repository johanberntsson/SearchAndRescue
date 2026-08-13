; The game's C stack, measured -- and the groundwork for the RAM under the
; BASIC ROM, which is not claimed yet.
;
; **Why the banking is not here.** Stage one gets 12 KB that way and the game
; wants it more, having had 44 bytes free. But a PRG cannot have two content
; areas -- "multiple program areas not allowed in prg output" -- and the only
; memory a second area can hold without becoming one is a section the linker
; rules do not already know. Stage one's is `highbss`, and it works because
; something is *in* it; declaring an empty one for the game is still refused.
; So the game needs its big BSS marked before the memory can be declared, and
; that is a separate change from this one.
;
; **The measurement pays first and costs nothing.** todo.md has wanted the C
; stack's real high-water mark since the 32 KB got tight: fill the whole span
; with a canary before anything uses it, and count how much of the pattern
; survives. Stage one uses 127 bytes of the toolchain's 4096, so there is very
; likely three and a half kilobytes here for the taking, with no ROM banking at
; all.

; It also measures the C stack, which todo.md has wanted a number for since
; the 32 KB got tight: fill the whole span with a canary here, and count how
; much of the pattern survives. Stage one does the same and uses 127 bytes of
; the toolchain's 4096.

            .rtmodel version, "1"
            .rtmodel core, "*"

            .extern _Zp, cstack_unused
            .section cstack

CANARY:     .equ 0xa5
ROMMAP:     .equ 0xd030
ROM_BASIC:  .equ 0x10

            .section code, text
            .public __low_level_init, cstack_measure

__low_level_init:
            ; The ROM stays mapped for now -- see the note at the top of the
            ; file. Banking is groundwork; the measurement below is the thing
            ; that pays immediately.
            lda     #.byte0 (.sectionStart cstack)
            sta     zp:_Zp
            lda     #.byte1 (.sectionStart cstack)
            sta     zp:_Zp+1
            ldx     #.byte1 (.sectionSize cstack)
            beq     nofill$
            lda     #CANARY
            ldy     #0
fill$:      sta     (_Zp),y
            iny
            bne     fill$
            inc     zp:_Zp+1
            dex
            bne     fill$
nofill$:    rts

; Bytes at the bottom of the stack still holding the pattern: the space that
; was never needed. The stack grows down, so the untouched span is at the
; start.
cstack_measure:
            lda     #.byte0 (.sectionStart cstack)
            sta     zp:_Zp
            lda     #.byte1 (.sectionStart cstack)
            sta     zp:_Zp+1
            ldx     #0
            ldy     #0
free$:      lda     (_Zp),y
            cmp     #CANARY
            bne     done$
            iny
            bne     free$
            inc     zp:_Zp+1
            inx
            bra     free$
done$:      sty     cstack_unused
            stx     cstack_unused+1
            rts
