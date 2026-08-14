; The engine note's interrupt half: walk the pitch towards the target and
; write it to both SIDs, fifty times a second.
;
; This is assembly because it runs in an interrupt. A C function called from
; one would use the same zero page scratch and software stack as whatever the
; main program happened to be in the middle of, and the renderer is in the
; middle of something for nearly the whole frame. src/engine.c keeps the
; decisions -- what the note should be -- and this does the walking.
;
; No zero page and no scratch of its own: the difference between where the
; note is and where it is going lives in Y:X for the eight instructions it is
; needed for.

            .extern engine_freq, engine_target

SID:        .equlab 0xd400
SID2:       .equlab 0xd420

; Frequency units per tick, and how far above voice 1 voice 2 sits. Both are
; described where they are chosen, in src/engine.c -- keep them in step.
RATE:       .equ 24
DETUNE:     .equ 60

            .section code,text
            .public engine_tick

engine_tick:
            ; Y:X = target - freq, and A the high half of it.
            lda     engine_target
            sec
            sbc     engine_freq
            tax
            lda     engine_target+1
            sbc     engine_freq+1
            tay
            bmi     below$

            ; The note is under the target, or on it. Within one step of it,
            ; land exactly rather than jittering either side for ever.
            cpy     #0
            bne     up$
            cpx     #RATE
            bcc     snap$
up$:        lda     engine_freq
            clc
            adc     #RATE
            sta     engine_freq
            lda     engine_freq+1
            adc     #0
            sta     engine_freq+1
            bra     write$

            ; Above it. The difference is negative, so a high half of $ff is
            ; the only case where the magnitude can be under a step, and there
            ; the low half counts back from 256.
below$:     cpy     #0xff
            bne     down$
            cpx     #(257-RATE)
            bcs     snap$
down$:      lda     engine_freq
            sec
            sbc     #RATE
            sta     engine_freq
            lda     engine_freq+1
            sbc     #0
            sta     engine_freq+1
            bra     write$

snap$:      lda     engine_target
            sta     engine_freq
            lda     engine_target+1
            sta     engine_freq+1

            ; Voice 1: the note itself.
write$:     lda     engine_freq
            sta     SID+0
            sta     SID2+0
            lda     engine_freq+1
            sta     SID+1
            sta     SID2+1

            ; Voice 2: DETUNE above it, which is what makes the throb.
            lda     engine_freq
            clc
            adc     #DETUNE
            sta     SID+7
            sta     SID2+7
            lda     engine_freq+1
            adc     #0
            sta     SID+8
            sta     SID2+8

            ; Voice 3: an octave down, for the body of it.
            lda     engine_freq+1
            lsr     a
            tax
            lda     engine_freq
            ror     a
            sta     SID+14
            sta     SID2+14
            stx     SID+15
            stx     SID2+15
            rts
