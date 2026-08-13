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

; --- how much of the C stack is actually used -----------------------------
;
; **4096 bytes is the toolchain's default and nothing here needs it.** todo.md
; has wanted this measured since the game's 32 KB got tight, and stage one is
; the right place to do it: no recursion, and a call chain three deep. The
; measurement is a canary rather than an estimate -- fill the stack before it
; is used and see how far the pattern survives.
;
; __low_level_init is the hook the startup calls after setting the stack
; pointer and before anything uses it, which is the one moment the whole span
; can be written.

CANARY:     .equ 0xa5

            .extern _Zp, cstack_unused
            .section cstack

            .section code, text
            .public __low_level_init, cstack_measure

; --- the RAM under the BASIC ROM ------------------------------------------
;
; **$D030 is the C65's ROM mapping, and bit 4 is BASIC at $A000.** Clearing it
; puts 8 KB of ordinary RAM there, which is where mega65-sar.scm sends this
; program's BSS and its C stack. Everything else in the map stays: the Kernal
; is untouched, so printf still works, and the interrupt vectors are still
; where the ROM left them -- which is why this needs no SEI and no handler of
; our own.
;
; It happens here because __low_level_init is called after the startup has set
; the C stack pointer and *before* anything uses it. The stack is one of the
; things that moved up there, so banking has to come first -- a write would
; fall through to the RAM underneath but the read back would come from ROM.
;
; basic_in below puts it back before the handover, since what runs next is
; BASIC itself.
ROMMAP:     .equ 0xd030
ROM_BASIC:  .equ 0x10
ROM_C000:   .equ 0x20

            .public basic_in

basic_in:   lda     ROMMAP
            ora     #ROM_BASIC | ROM_C000
            sta     ROMMAP
            rts

__low_level_init:
            lda     ROMMAP
            and     #~(ROM_BASIC | ROM_C000) & 0xff
            sta     ROMMAP

            lda     #.byte0 (.sectionStart cstack)
            sta     zp:_Zp
            lda     #.byte1 (.sectionStart cstack)
            sta     zp:_Zp+1
            ldx     #.byte1 (.sectionSize cstack)   ; whole pages of it
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

; Leaves the count in cstack_unused rather than returning it, so that nothing
; here has to guess at the calling convention for a sixteen-bit result.
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
