; The generator's other per-pixel loop: normalise a row of the octave sum,
; write it to attic RAM, and checksum it.
;
;   for x in 0..511:
;       v = mulhi(acc[x], weight_recip)
;       out[x] = v
;       a += v ; b += a
;       acc[x] = 0
;
; **The zeroing is folded in on purpose.** acc is a Q8.16 accumulator that has
; to start each row empty, and it is read here for the last time -- so clearing
; it costs four stores in a loop that already has the pointer, against a whole
; second pass over the row. That pass was in C and is now gone.
;
; The attic write is a plain CPU store through a 32-bit pointer rather than a
; DMA out of a row buffer: a posted write into attic RAM costs +3 cycles and
; the DMA costs 9.54 a byte, so buffering would be three times the price of
; storing each value as it is computed. documentation/on-device-maps.md has
; the measurement.

            .rtmodel version, "1"
            .rtmodel core, "*"

            .extern nz_acc, nz_out, nz_recip, nz_sum_a, nz_sum_b, nz_chunks

MULTINA:    .equ 0xd770
MULTINB:    .equ 0xd774
MULTOUT:    .equ 0xd778

; The same 64 that keeps the blend loop's indices eight bits wide: 64 pixels is
; 256 bytes of acc for Y and 128 bytes of output for Z.
CHUNK:      .equ 64
CHUNKS:     .equ 512 / CHUNK

            .section code, text
            .public noise_store

noise_store:
            ; The divisor is one number for the whole field, so it goes in the
            ; multiplier's B input once and stays there -- the only operand in
            ; either loop that never has to be rewritten.
            lda     zp:nz_recip
            sta     MULTINB
            lda     zp:nz_recip+1
            sta     MULTINB+1
            lda     zp:nz_recip+2
            sta     MULTINB+2
            lda     zp:nz_recip+3
            sta     MULTINB+3
            lda     #CHUNKS
            sta     zp:nz_chunks

chunk$:     ldy     #0
            ldz     #0

pixel$:     ; --- MULTINA = acc[x], and clear it as it goes ------------------
            lda     (nz_acc),y
            sta     MULTINA
            lda     #0
            sta     (nz_acc),y
            iny
            lda     (nz_acc),y
            sta     MULTINA+1
            lda     #0
            sta     (nz_acc),y
            iny
            lda     (nz_acc),y
            sta     MULTINA+2
            lda     #0
            sta     (nz_acc),y
            iny
            lda     (nz_acc),y
            sta     MULTINA+3
            lda     #0
            sta     (nz_acc),y
            iny

            ; --- v = product >> 16, out to attic RAM -----------------------
            lda     MULTOUT+2
            sta     [nz_out],z
            tax                       ; keep the low byte for the checksum
            inz
            lda     MULTOUT+3
            sta     [nz_out],z
            inz

            ; --- Fletcher: a += v, b += a, both wrapping at 16 bits --------
            clc
            txa
            adc     zp:nz_sum_a
            sta     zp:nz_sum_a
            lda     MULTOUT+3
            adc     zp:nz_sum_a+1
            sta     zp:nz_sum_a+1
            clc
            lda     zp:nz_sum_a
            adc     zp:nz_sum_b
            sta     zp:nz_sum_b
            lda     zp:nz_sum_a+1
            adc     zp:nz_sum_b+1
            sta     zp:nz_sum_b+1

            cpz     #CHUNK*2
            lbcc    pixel$

            ; --- on to the next chunk of the row ---------------------------
            ; 64 pixels of acc is exactly 256 bytes, so only its high byte moves
            inc     zp:nz_acc+1
            clc
            lda     zp:nz_out
            adc     #CHUNK*2
            sta     zp:nz_out
            bcc     done$
            inc     zp:nz_out+1
            bne     done$
            inc     zp:nz_out+2
            bne     done$
            inc     zp:nz_out+3
done$:      dec     zp:nz_chunks
            lbne    chunk$
            ; **Z must go back to zero** -- see the note in noise_asm.s.
            ldz     #0
            rts
