; Two Kernal calls stage one needs and C cannot make.
;
; IOINIT is the one that matters. src/profile.c takes CIA2's two timers over
; for its clock, and the Kernal needs them to talk to a disk -- which is
; exactly what the handover does, since BASIC has to LOAD the game. So the
; timers have to go back before stage one returns, and $FF84 is the documented
; way to put them back: ozmoo calls it for the same reason before its restart
; (asm/disk.asm, just above the keyboard-queue code this borrows).

            .rtmodel version, "1"
            .rtmodel core, "*"

            .section code, text
            .public kernal_ioinit

; Initialize the CIAs, the SID, the memory configuration and the interrupt
; timer -- undoing profile_init's claim on CIA2.
kernal_ioinit:
            jsr 0xff84
            rts

; --- and BASIC's zero page ------------------------------------------------
;
; **The linker is allowed $02-$7F and BASIC lives there too.** Calypsi's
; pseudo registers take the bottom of it and save themselves; the variables a
; program declares `__zpage` are placed above them and are not saved by
; anybody. The generator's went to $3A-$5A, and with them held BASIC could not
; get back to READY at all -- main returned, the keyboard queue was filled, and
; the machine stopped there.
;
; NEW does not cover this. NEW resets the pointers a *program* moves -- which
; is why it fixes the string stack -- and leaves everything else as it found
; it. So the whole span the linker gave us is copied out at the start of the
; run and put back before the handover.
;
; The bounds come from the linker rather than from a comment, so the buffer
; cannot fall behind the variables it is shadowing.

            .section zzpage

            .section zpsave_copy, bss
zpcopy:     .space  128

            .section code, text
            .public zp_preserve, zp_restore

zp_preserve:
            ldx     #.sectionSize zzpage - 1
-           lda     .sectionStart zzpage,x
            sta     zpcopy,x
            dex
            bpl     -
            rts

zp_restore:
            ldx     #.sectionSize zzpage - 1
-           lda     zpcopy,x
            sta     .sectionStart zzpage,x
            dex
            bpl     -
            rts
