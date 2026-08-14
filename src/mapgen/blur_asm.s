; The box blur's two inner loops, over one row of 512.
;
;   blur_out    out[x] = (acc[x] * recip) >> 32
;   blur_roll   acc[x] += add[x] - sub[x]
;   blur_x      the same moving average along a row, in one pass
;
; The rows themselves are moved by DMA (see row_in/row_out in noise.c); what is
; left is the arithmetic, and in C that was 13 of stage one's 53 seconds. Each
; pixel is a 32-bit accumulator update and one multiply, which is exactly the
; shape the compiler is worst at: it will not inline the multiply and it keeps
; the accumulator index on the software stack.
;
; **The reciprocal is Q0.32, so the answer is the top half of the product.**
; The window is 17 wide and never a power of two; 1/17 as Q0.32 is 0x0F0F0F10
; and the divide disappears into one $D770 multiply, whose bytes 4 and 5 are
; the result.

            .rtmodel version, "1"
            .rtmodel core, "*"

            .extern nz_acc, nz_out, nz_top, nz_bot, nz_recip, nz_chunks
            .extern nz_t, nz_d, nz_n

MULTINA:    .equ 0xd770
MULTINB:    .equ 0xd774
MULTOUT:    .equ 0xd778

; 64 pixels a chunk keeps both indices eight bits wide, as everywhere else
; here: the accumulator is 32-bit so Y walks by fours, the 16-bit rows so Z
; walks by twos.
CHUNK:      .equ 64
CHUNKS:     .equ 512 / CHUNK

            .section code, text
            .public blur_out, blur_roll
            .extern nz_sum, nz_lead, nz_trail

; --- out[x] = (acc[x] * recip) >> 32 --------------------------------------
;
; nz_acc points at the 512 running sums, nz_top at the output row, and
; nz_recip holds the Q0.32 reciprocal, which never changes -- so it goes into
; the multiplier's B input once for the whole row.
blur_out:
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

ochunk$:    ldy     #0
            ldz     #0

opixel$:    lda     (nz_acc),y
            sta     MULTINA
            iny
            lda     (nz_acc),y
            sta     MULTINA+1
            iny
            lda     (nz_acc),y
            sta     MULTINA+2
            iny
            lda     (nz_acc),y
            sta     MULTINA+3
            iny

            lda     MULTOUT+4
            sta     (nz_top),z
            inz
            lda     MULTOUT+5
            sta     (nz_top),z
            inz

            cpz     #CHUNK*2
            lbcc    opixel$

            inc     zp:nz_acc+1       ; 64 sums is exactly 256 bytes
            clc
            lda     zp:nz_top
            adc     #CHUNK*2
            sta     zp:nz_top
            bcc     odone$
            inc     zp:nz_top+1
odone$:     dec     zp:nz_chunks
            lbne    ochunk$
            ldz     #0
            rts

; --- acc[x] += add[x] - sub[x] ---------------------------------------------
;
; The two rows entering and leaving the window, as 16-bit values added into a
; 32-bit sum. The difference is signed, so the subtraction is done first into
; a word and then sign-extended across the top two bytes -- the accumulator
; can never actually go negative, since it is a sum of seventeen of the same
; values, but the *step* can and the carry has to be right either way.
blur_roll:
            lda     #CHUNKS
            sta     zp:nz_chunks

rchunk$:    ldy     #0
            ldz     #0

rpixel$:    sec
            lda     (nz_top),z
            sbc     (nz_bot),z
            sta     zp:nz_d
            inz
            lda     (nz_top),z
            sbc     (nz_bot),z
            sta     zp:nz_d+1
            inz
            lda     #0
            bcs     rpos$
            lda     #0xff             ; borrowed: the step is negative
rpos$:      sta     zp:nz_t

            clc
            lda     (nz_acc),y
            adc     zp:nz_d
            sta     (nz_acc),y
            iny
            lda     (nz_acc),y
            adc     zp:nz_d+1
            sta     (nz_acc),y
            iny
            lda     (nz_acc),y
            adc     zp:nz_t
            sta     (nz_acc),y
            iny
            lda     (nz_acc),y
            adc     zp:nz_t
            sta     (nz_acc),y
            iny

            cpz     #CHUNK*2
            lbcc    rpixel$

            inc     zp:nz_acc+1
            clc
            lda     zp:nz_top
            adc     #CHUNK*2
            sta     zp:nz_top
            bcc     rbot$
            inc     zp:nz_top+1
rbot$:      clc
            lda     zp:nz_bot
            adc     #CHUNK*2
            sta     zp:nz_bot
            bcc     rdone$
            inc     zp:nz_bot+1
rdone$:     dec     zp:nz_chunks
            lbne    rchunk$
            ldz     #0
            rts

; --- one row across --------------------------------------------------------
;
; `blur_x` from noise.c: a moving average along a row. The vertical half of the
; blur has been assembly since it was written -- this was the half left in C,
; and it is the same 262144 iterations, each with a `jsr` to the multiplier and
; two masked array reads.
;
;   for x: out[x] = (sum * recip) >> 32
;          sum += pad[x + 2R + 1] - pad[x]
;
; **The caller pads the row instead of wrapping the index.** The window runs
; from x - R to x + R and the row is a torus, so the C masked both edges on
; every pixel. Copying the last R entries in front of the row and the first R
; after it -- seventeen words a row, against a thousand masks -- makes all
; three walks linear, which is what lets them be pointers stepped by two with
; Z never leaving 0 and 1.
;
; nz_top is the padded row, nz_bot the output, nz_recip the Q0.32 reciprocal,
; which never changes and so is written to the multiplier's B input once.

BLUR_R2:    .equ 2 * 8                ; BLUR_R, in bytes

            .section code, text
            .public blur_x_row

blur_x_row:
            ; the first window: pad[0 .. 2R], which is BLUR_N entries
            lda     #0
            sta     zp:nz_sum
            sta     zp:nz_sum+1
            sta     zp:nz_sum+2
            sta     zp:nz_sum+3
            lda     zp:nz_top
            sta     zp:nz_lead
            lda     zp:nz_top+1
            sta     zp:nz_lead+1
            ldx     #17               ; BLUR_N
prime$:
            jsr     takelead$
            dex
            bne     prime$

            ; the trailing edge starts at the front of the pad; the leading one
            ; is where priming left it, at pad[2R + 1].
            lda     zp:nz_top
            sta     zp:nz_trail
            lda     zp:nz_top+1
            sta     zp:nz_trail+1

            lda     zp:nz_recip
            sta     MULTINB
            lda     zp:nz_recip+1
            sta     MULTINB+1
            lda     zp:nz_recip+2
            sta     MULTINB+2
            lda     zp:nz_recip+3
            sta     MULTINB+3

            ldx     #2                ; two runs of 256 is the row
outer$:
            ldy     #0
pixel$:
            lda     zp:nz_sum
            sta     MULTINA
            lda     zp:nz_sum+1
            sta     MULTINA+1
            lda     zp:nz_sum+2
            sta     MULTINA+2
            lda     zp:nz_sum+3
            sta     MULTINA+3
            ldz     #0
            lda     MULTOUT+4
            sta     (zp:nz_bot),z
            inz
            lda     MULTOUT+5
            sta     (zp:nz_bot),z
            clc
            lda     zp:nz_bot
            adc     #2
            sta     zp:nz_bot
            bcc     out$
            inc     zp:nz_bot+1
out$:
            jsr     takelead$
            jsr     droptrail$
            iny
            bne     pixel$
            dex
            bne     outer$
            ldz     #0                ; Calypsi keeps its pointer index in Z
            rts

; sum += the entry at the leading edge, then step it on.
takelead$:
            ldz     #0
            clc
            lda     (zp:nz_lead),z
            adc     zp:nz_sum
            sta     zp:nz_sum
            inz
            lda     (zp:nz_lead),z
            adc     zp:nz_sum+1
            sta     zp:nz_sum+1
            lda     #0
            adc     zp:nz_sum+2
            sta     zp:nz_sum+2
            lda     #0
            adc     zp:nz_sum+3
            sta     zp:nz_sum+3
            clc
            lda     zp:nz_lead
            adc     #2
            sta     zp:nz_lead
            bcc     ledone$
            inc     zp:nz_lead+1
ledone$:
            rts

; sum -= the entry at the trailing edge, then step it on.
droptrail$:
            ldz     #0
            sec
            lda     zp:nz_sum
            sbc     (zp:nz_trail),z
            sta     zp:nz_sum
            inz
            lda     zp:nz_sum+1
            sbc     (zp:nz_trail),z
            sta     zp:nz_sum+1
            lda     zp:nz_sum+2
            sbc     #0
            sta     zp:nz_sum+2
            lda     zp:nz_sum+3
            sbc     #0
            sta     zp:nz_sum+3
            clc
            lda     zp:nz_trail
            adc     #2
            sta     zp:nz_trail
            bcc     trdone$
            inc     zp:nz_trail+1
trdone$:
            rts
