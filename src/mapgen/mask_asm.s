; The island mask, one row: a radial falloff with a noisy coastline, multiplied
; into the terrain.
;
;   r2 = dx^2 + dy^2                  the radius squared, in Q0.16
;   r  = sqrt(r2) + wobble            wobble is one octave of value noise
;   t  = clip((EDGE - r) * 1/FADE)
;   h  = h * smoothstep(t)
;
; **This is the only per-pixel square root in the generator**, and the reason
; fixed.py's sqrt is normalised before it reads its table: a root's slope is
; infinite at zero, so reading the table straight at small x is wrong by two
; height units and the error is a visible ring around the middle of the map.
; Shifting x up in pairs of bits until its top is set puts every input on the
; flat part of the curve, and shifting the result back down by half as many is
; exact, because sqrt(4^k) is 2^k.
;
; Three cheap exits carry most of the pixels:
;
;   - r2 >= 1.0 is outside the unit circle, where the mask is zero however the
;     coastline wobbles: EDGE is 0.86 and the wobble reaches 0.13, so the sum
;     can never come back under. Half a map outside the coast, written as zero
;     without a root, a noise sample or a multiply.
;   - the clip at 1.0 means the mask is 1.0, so the terrain passes through
;     untouched and there is no multiply at all.
;   - the mask coming out zero writes zero.

            .rtmodel version, "1"
            .rtmodel core, "*"

            .extern nz_out, nz_top, nz_bot, nz_wy, nz_recip, nz_chunks
            .extern nz_t, nz_b, nz_d, nz_n, nz_sum_a, nz_sum_b
            .extern nz_sqrt, nz_delta, nz_sq, nz_dy2, nz_edge, nz_wobble
            .extern nz_ptr, nz_lo, nz_neg

MULTINA:    .equ 0xd770
MULTINB:    .equ 0xd774
MULTOUT:    .equ 0xd778

CHUNK:      .equ 64
CHUNKS:     .equ 512 / CHUNK
HALF_LO:    .equ 0                    ; the map's centre column, 256, in bytes
HALF_HI:    .equ 1

            .section code, text
            .public mask_row

mask_row:
            lda     #0
            sta     MULTINA+2
            sta     MULTINA+3
            sta     MULTINB+2
            sta     MULTINB+3
            lda     #CHUNKS
            sta     zp:nz_chunks
            lda     #0                ; the column, counted up alongside Z
            sta     zp:nz_lo
            lda     #0
            sta     zp:nz_lo+1

chunk$:     ldz     #0

pixel$:     ; **The multiplier's B input is sixteen bits for most of what
            ; follows, and one thing here writes all thirty-two.** The
            ; reciprocal of the fade is 0x22CC8, so its third byte is 2, and on
            ; the paths that do not reach the smoothstep it was still sitting
            ; there when the next pixel's square root read its table -- every
            ; interpolation off by two whole units of B. Cleared per pixel, for
            ; six cycles, rather than at each of the five places that would
            ; otherwise have to remember.
            lda     #0
            sta     MULTINB+2
            sta     MULTINB+3

            ; --- r2 = dx*dx + dy*dy ---------------------------------------
            ; The table wants |dx|, so the sign is never formed: past the
            ; centre the column *is* |dx|, and before it the byte's negation
            ; is. **|dx| of 256 squares one past a word**, and a pixel that far
            ; off the axis is outside the circle whatever its row, so column
            ; zero is answered here rather than in the table.
            lda     zp:nz_lo+1
            bne     dxpos$
            lda     zp:nz_lo
            lbeq    zero$
            eor     #0xff
            clc
            adc     #1                ; 256 - column, in one byte
            bra     dxgot$
dxpos$:     lda     zp:nz_lo
dxgot$:     asl     a                 ; two bytes an entry
            tay
            lda     #0
            rol     a
            clc
            adc     zp:nz_sq+1
            sta     zp:nz_ptr+1
            lda     zp:nz_sq
            sta     zp:nz_ptr

            clc
            lda     (nz_ptr),y
            adc     zp:nz_dy2
            sta     zp:nz_d
            iny
            lda     (nz_ptr),y
            adc     zp:nz_dy2+1
            sta     zp:nz_d+1
            lbcs    zero$             ; carried out: r2 >= 1.0, outside

            ; --- r = sqrt(r2), table with the input normalised -------------
            ; shift up in pairs of bits until the top two are set, counting
            ; the rounds; at most eight, and zero exits at once.
            lda     zp:nz_d
            ora     zp:nz_d+1
            beq     rooted$           ; sqrt(0) is 0, and k does not matter
            lda     #0
            sta     zp:nz_n           ; k
norm$:      lda     zp:nz_d+1
            cmp     #0x40             ; x < ONE >> 2 ?
            bcs     table$
            asl     zp:nz_d
            rol     zp:nz_d+1
            asl     zp:nz_d
            rol     zp:nz_d+1
            inc     zp:nz_n
            bra     norm$

table$:     ; SQRT[i] + (DSQRT[i] * w >> 16), i the top byte, w the rest.
            ;
            ; **i * 2 does not fit Y.** Normalising leaves i at 64 or above, so
            ; the entry offset runs to 510 and an 8-bit index wraps on
            ; everything past i = 127 -- which is most of the map. The bit that
            ; falls off the shift goes onto the table's high byte instead, and
            ; Y carries the rest.
            lda     zp:nz_d+1
            asl     a
            tay                       ; (i * 2) low
            lda     #0
            rol     a                 ; ... and its carry
            sta     zp:nz_t

            clc
            lda     zp:nz_sqrt
            sta     zp:nz_ptr
            lda     zp:nz_sqrt+1
            adc     zp:nz_t
            sta     zp:nz_ptr+1
            lda     (nz_ptr),y
            sta     zp:nz_b
            iny
            lda     (nz_ptr),y
            sta     zp:nz_b+1
            dey

            clc
            lda     zp:nz_delta
            sta     zp:nz_ptr
            lda     zp:nz_delta+1
            adc     zp:nz_t
            sta     zp:nz_ptr+1
            lda     (nz_ptr),y
            sta     MULTINA
            iny
            lda     (nz_ptr),y
            sta     MULTINA+1
            lda     #0
            sta     MULTINB
            lda     zp:nz_d           ; w is the low byte, in the high place
            sta     MULTINB+1
            clc
            lda     zp:nz_b
            adc     MULTOUT+2
            sta     zp:nz_b
            lda     zp:nz_b+1
            adc     MULTOUT+3
            sta     zp:nz_b+1

            ; ... and back down by k, which is exact
            ldy     zp:nz_n
            beq     rooted2$
shift$:     lsr     zp:nz_b+1
            ror     zp:nz_b
            dey
            bne     shift$
            bra     rooted2$

rooted$:    lda     #0
            sta     zp:nz_b
            sta     zp:nz_b+1
rooted2$:

            ; --- the coastline's wobble: scale(2n - ONE, WOBBLE) -----------
            ;
            ; **r is signed here even though it is carried in sixteen bits.**
            ; The wobble reaches 0.13 either way, so near the middle of the map
            ; it takes the radius below zero, and out at the rim it can push it
            ; past 1.0. The subtraction that follows gives the right sixteen
            ; bits in the first case and the wrong branch, so the sign is
            ; carried alongside rather than inferred from the borrow.
            lda     #0
            sta     zp:nz_neg
            lda     (nz_top),z
            sta     zp:nz_t
            lda     (nz_bot),z
            sta     zp:nz_d
            inz
            lda     (nz_top),z
            sta     zp:nz_t+1
            lda     (nz_bot),z
            sta     zp:nz_d+1
            dez

            ; n = lerp(top, bot, wy), the same floor as everywhere else
            sec
            lda     zp:nz_d
            sbc     zp:nz_t
            sta     MULTINA
            lda     zp:nz_d+1
            sbc     zp:nz_t+1
            sta     MULTINA+1
            bcs     wup$
            sec
            lda     zp:nz_t
            sbc     zp:nz_d
            sta     MULTINA
            lda     zp:nz_t+1
            sbc     zp:nz_d+1
            sta     MULTINA+1
            lda     zp:nz_wy
            sta     MULTINB
            lda     zp:nz_wy+1
            sta     MULTINB+1
            sec
            lda     zp:nz_t
            sbc     MULTOUT+2
            sta     zp:nz_n
            lda     zp:nz_t+1
            sbc     MULTOUT+3
            sta     zp:nz_n+1
            lda     MULTOUT
            ora     MULTOUT+1
            beq     wdone$
            lda     zp:nz_n
            bne     wnolo$
            dec     zp:nz_n+1
wnolo$:     dec     zp:nz_n
            bra     wdone$

wup$:       lda     zp:nz_wy
            sta     MULTINB
            lda     zp:nz_wy+1
            sta     MULTINB+1
            clc
            lda     zp:nz_t
            adc     MULTOUT+2
            sta     zp:nz_n
            lda     zp:nz_t+1
            adc     MULTOUT+3
            sta     zp:nz_n+1
wdone$:

            ; r += scale(2n - ONE, WOBBLE). 2n - ONE is signed and reaches
            ; -65536 exactly when n is zero, which is a whole bit past the
            ; sixteen the magnitude is carried in -- so that one value is
            ; answered from the identity instead: scale(-ONE, w) is -w.
            lda     zp:nz_n
            ora     zp:nz_n+1
            bne     wmag$
            sec                       ; n == 0: r -= WOBBLE
            lda     zp:nz_b
            sbc     zp:nz_wobble
            sta     zp:nz_b
            lda     zp:nz_b+1
            sbc     zp:nz_wobble+1
            sta     zp:nz_b+1
            lbcs    edge$
            inc     zp:nz_neg
            lbra    edge$

wmag$:      lda     zp:nz_n+1
            bmi     whi$
            ; n < 0.5: the wobble pulls the coast in. |2n - ONE| = (0x8000-n)*2
            sec
            lda     #0x00
            sbc     zp:nz_n
            sta     MULTINA
            lda     #0x80
            sbc     zp:nz_n+1
            sta     MULTINA+1
            asl     MULTINA
            rol     MULTINA+1
            lda     zp:nz_wobble
            sta     MULTINB
            lda     zp:nz_wobble+1
            sta     MULTINB+1
            ; floor of a negative: down one more if anything is left over
            sec
            lda     zp:nz_b
            sbc     MULTOUT+2
            sta     zp:nz_b
            lda     zp:nz_b+1
            sbc     MULTOUT+3
            sta     zp:nz_b+1
            bcs     wnoneg$
            inc     zp:nz_neg
wnoneg$:    lda     MULTOUT
            ora     MULTOUT+1
            beq     edge$
            ; **dec does not touch the carry**, so whether this last step takes
            ; the radius below zero has to be asked before it is taken and not
            ; after -- which is only when the radius is already nothing.
            lda     zp:nz_b
            ora     zp:nz_b+1
            bne     wdec$
            inc     zp:nz_neg
wdec$:      lda     zp:nz_b
            bne     wnolo2$
            dec     zp:nz_b+1
wnolo2$:    dec     zp:nz_b
            lbra    edge$

whi$:       ; n >= 0.5: (2n - ONE) = (n - 0x8000) * 2
            sec
            lda     zp:nz_n
            sbc     #0x00
            sta     MULTINA
            lda     zp:nz_n+1
            sbc     #0x80
            sta     MULTINA+1
            asl     MULTINA
            rol     MULTINA+1
            lda     zp:nz_wobble
            sta     MULTINB
            lda     zp:nz_wobble+1
            sta     MULTINB+1
            clc
            lda     zp:nz_b
            adc     MULTOUT+2
            sta     zp:nz_b
            lda     zp:nz_b+1
            adc     MULTOUT+3
            sta     zp:nz_b+1
            lbcs    zero$             ; past 1.0, which is well past the edge

edge$:      ; --- t = clip((EDGE - r) * 1/FADE), and the mask ---------------
            sec
            lda     zp:nz_edge
            sbc     zp:nz_b
            sta     MULTINA
            lda     zp:nz_edge+1
            sbc     zp:nz_b+1
            sta     MULTINA+1
            bcs     inside$
            ; a borrow means either r > EDGE, which is nothing, or r below zero
            ; -- and in that second case the sixteen bits just computed are
            ; already EDGE + |r|, which is what is wanted.
            lda     zp:nz_neg
            lbeq    zero$
inside$:

            lda     zp:nz_recip
            sta     MULTINB
            lda     zp:nz_recip+1
            sta     MULTINB+1
            lda     zp:nz_recip+2
            sta     MULTINB+2
            lda     zp:nz_recip+3
            sta     MULTINB+3
            lda     MULTOUT+4
            ora     MULTOUT+5
            ora     MULTOUT+6
            ora     MULTOUT+7
            lbne    keep$             ; t clipped to 1.0: the terrain stands

            ; smoothstep(t) = t^2 * (3 - 2t), and then h * that.
            ;
            ; **Take t out of the product before touching either input.** The
            ; multiplier answers combinationally: the instant MULTINA is
            ; written the result changes, so reading MULTOUT+3 afterwards reads
            ; a product of the new A with the old B. Interleaving the two --
            ; store a byte of t, feed a byte to the multiplier -- costs nothing
            ; and looked tidy, and it silently multiplied by rubbish.
            lda     MULTOUT+2
            sta     zp:nz_t
            lda     MULTOUT+3
            sta     zp:nz_t+1

            lda     zp:nz_t
            sta     MULTINA
            sta     MULTINB
            lda     zp:nz_t+1
            sta     MULTINA+1
            sta     MULTINB+1
            lda     #0
            sta     MULTINB+2
            sta     MULTINB+3
            lda     MULTOUT+2
            sta     zp:nz_d           ; t^2
            lda     MULTOUT+3
            sta     zp:nz_d+1

            ; 3 - 2t in Q0.16 is 0x30000 - 2t, which needs seventeen bits, so
            ; it goes in the multiplier's four-byte input rather than a word.
            sec
            lda     #0x00
            sbc     zp:nz_t
            sta     MULTINB
            lda     #0x00
            sbc     zp:nz_t+1
            sta     MULTINB+1
            lda     #0x03
            sbc     #0x00
            sta     MULTINB+2
            lda     #0
            sta     MULTINB+3
            sec
            lda     MULTINB
            sbc     zp:nz_t
            sta     MULTINB
            lda     MULTINB+1
            sbc     zp:nz_t+1
            sta     MULTINB+1
            lda     MULTINB+2
            sbc     #0
            sta     MULTINB+2
            lda     zp:nz_d
            sta     MULTINA
            lda     zp:nz_d+1
            sta     MULTINA+1
            lda     MULTOUT+2
            sta     zp:nz_t           ; the mask
            lda     MULTOUT+3
            sta     zp:nz_t+1

            ; h = h * mask
            lda     [nz_out],z
            sta     MULTINA
            inz
            lda     [nz_out],z
            dez
            sta     MULTINA+1
            lda     zp:nz_t
            sta     MULTINB
            lda     zp:nz_t+1
            sta     MULTINB+1
            lda     #0
            sta     MULTINB+2
            sta     MULTINB+3
            lda     MULTOUT+2
            sta     zp:nz_d
            lda     MULTOUT+3
            sta     zp:nz_d+1
            bra     store$

keep$:      lda     [nz_out],z
            sta     zp:nz_d
            inz
            lda     [nz_out],z
            dez
            sta     zp:nz_d+1
            bra     store$

zero$:      lda     #0
            sta     zp:nz_d
            sta     zp:nz_d+1

store$:     lda     zp:nz_d
            sta     [nz_out],z
            inz
            lda     zp:nz_d+1
            sta     [nz_out],z
            inz

            clc
            lda     zp:nz_d
            adc     zp:nz_sum_a
            sta     zp:nz_sum_a
            lda     zp:nz_d+1
            adc     zp:nz_sum_a+1
            sta     zp:nz_sum_a+1
            clc
            lda     zp:nz_sum_a
            adc     zp:nz_sum_b
            sta     zp:nz_sum_b
            lda     zp:nz_sum_a+1
            adc     zp:nz_sum_b+1
            sta     zp:nz_sum_b+1

            inc     zp:nz_lo
            bne     nohi$
            inc     zp:nz_lo+1
nohi$:      cpz     #CHUNK*2
            lbcc    pixel$

            clc
            lda     zp:nz_out
            adc     #CHUNK*2
            sta     zp:nz_out
            bcc     ptrs$
            inc     zp:nz_out+1
            bne     ptrs$
            inc     zp:nz_out+2
            bne     ptrs$
            inc     zp:nz_out+3
ptrs$:      clc
            lda     zp:nz_top
            adc     #CHUNK*2
            sta     zp:nz_top
            bcc     bot$
            inc     zp:nz_top+1
bot$:       clc
            lda     zp:nz_bot
            adc     #CHUNK*2
            sta     zp:nz_bot
            bcc     done$
            inc     zp:nz_bot+1
done$:      dec     zp:nz_chunks
            lbne    chunk$
            ldz     #0
            rts
