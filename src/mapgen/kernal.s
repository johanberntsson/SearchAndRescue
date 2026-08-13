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
