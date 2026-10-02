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

            .extern _Zp, cstack_unused, _InitialStack
            .section cstack

CANARY:     .equ 0xa5
ROMMAP:     .equ 0xd030
ROM_BASIC:  .equ 0x10

            .section code, text
            .public __low_level_init, cstack_measure, basic_return

__low_level_init:
            ; The first thing, before anything has written over what BASIC
            ; owns: keep it in attic RAM, so that quitting can put it back and
            ; return through the SYS that started us. See basic_return.
            lda     ROMMAP
            sta     rom_d030
            lda     #.byte1 save_low
            ldx     #.byte0 save_low
            jsr     dma_run
            lda     #.byte1 save_bank1
            ldx     #.byte0 save_bank1
            jsr     dma_run
            ; BASIC out, and $A000-$BFFF is RAM for the rest of the run: the
            ; HIGH_BSS window in src/mega65-game.scm. Here rather than in main
            ; because nothing may read the window before it is RAM -- and
            ; after the startup has set the C stack, which is below it.
            lda     ROMMAP
            and     #~ROM_BASIC & 0xff
            sta     ROMMAP
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

; **Quitting to BASIC is a return, not a reset.** A reset reruns the disk's
; AUTOBOOT.C65 -- BASIC 65's cold start boots any disk that has one -- so the
; only way to READY is back through the SYS that started the game, which is
; what the MEGA65 startup is built for: it never moves the stack, and keeps
; the stack pointer main was called with in _InitialStack, for exit().
;
; What stands in the way is that the game has had BASIC's memory: zero page,
; the ROM's buffers at $1600-$1EFF (LOW_FREE), and bank 1, where BASIC keeps
; its variables and the colour RAM alias sits. __low_level_init copied all of
; it to attic RAM before any of that happened, and this copies it back --
; which puts the IRQ vector, the screen editor and the screen itself back as
; well. Called with interrupts off and the display, SIDs and CIA2 already
; restored from C (quit_to_basic in main.c); never returns to its caller.
; Zero page and the stack are BASIC's again after the first DMA, so nothing
; here may use either until the RTS.
basic_return:
            sei
            lda     #.byte1 load_bank1
            ldx     #.byte0 load_bank1
            jsr     dma_run
            ; Inline rather than through dma_run: this job rewrites the stack
            ; page, so no return address may be waiting on it.
            lda     #.byte1 load_low
            sta     0xd701
            lda     #0
            sta     0xd702
            sta     0xd704
            lda     #.byte0 load_low
            sta     0xd705
            ldx     _InitialStack   ; before BASIC is back over HIGH_BSS
            txs
            lda     rom_d030
            sta     ROMMAP
            cli
            rts                     ; into BASIC's SYS, which prints READY

; DMA job list at A:X (high:low), which must be in bank 0.
dma_run:    sta     0xd701
            lda     #0
            sta     0xd702
            sta     0xd704
            stx     0xd705          ; enhanced list; the job runs here
            rts

rom_d030:   .byte   0               ; in code, which nothing initialises over

; Where the snapshot lives: attic RAM above the per-map palettes.
SNAP_MB:    .equ    0x87
SNAP_BANK0: .equ    0x08            ; $8780000: bank 1's 64 KB
SNAP_BANK1: .equ    0x09            ; $8790000: $0002-$1FFF

; F018B jobs with the option bytes dma.c uses: megabytes, a destination step
; of one, then copy, count, source, destination.
job:        .macro  smb, dmb, count, src, sbank, dst, dbank
            .byte   0x0b, 0x80, \smb, 0x81, \dmb, 0x85, 1, 0
            .byte   0
            .word   \count, \src
            .byte   \sbank
            .word   \dst
            .byte   \dbank, 0
            .word   0
            .endm

; $0000 and $0001 are the CPU port, not RAM, and are left out. A count of 0
; is 64 KB.
save_low:   job     0, SNAP_MB, 0x1ffe, 0x0002, 0, 0x0002, SNAP_BANK1
save_bank1: job     0, SNAP_MB, 0, 0x0000, 1, 0x0000, SNAP_BANK0
load_low:   job     SNAP_MB, 0, 0x1ffe, 0x0002, SNAP_BANK1, 0x0002, 0
load_bank1: job     SNAP_MB, 0, 0, 0x0000, SNAP_BANK0, 0x0000, 1
