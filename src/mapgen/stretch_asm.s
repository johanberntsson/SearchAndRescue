; The percentile stretch's two passes over the field, a row at a time.
;
;   stretch_hist   hist[v >> 6]++                              -- measure
;   stretch_apply  v = floor + (clip((v-lo)*recip) * range)     -- paint
;
; Both are per-pixel loops and so both are assembly: the C compiler costs this
; project about 8x on a loop of this shape, which is the whole finding of
; documentation/on-device-maps.md's step 2b.
;
; They read the field out of attic RAM, which is the expensive direction -- 41
; cycles a sequential byte against 26 in chip RAM, and a posted write back is
; nearly free at +3. That asymmetry is why there are two passes and not ten:
; every pass over the field costs half a second before it does any arithmetic.

            .rtmodel version, "1"
            .rtmodel core, "*"

            .extern nz_out, nz_hist, nz_lo, nz_recip, nz_ptr
            .extern nz_t, nz_d, nz_sum_a, nz_sum_b, nz_chunks
            .extern nz_floor, nz_range

MULTINA:    .equ 0xd770
MULTINB:    .equ 0xd774
MULTOUT:    .equ 0xd778

; 64 pixels a chunk: 128 bytes of field, so Z walks 0..126 by twos and the
; end test is an immediate that fits. 128 pixels would be 256 bytes, which Z
; wraps on -- the same 64 the other two loops use, for the same reason.
CHUNK:      .equ 64
CHUNKS:     .equ 512 / CHUNK

            .section code, text
            .public stretch_hist, stretch_apply

; --- measure ---------------------------------------------------------------
;
; The bucket is v >> 6 and the counters are four bytes, so the offset into the
; table is (v >> 6) << 2, which is (v >> 4) with its low two bits cleared --
; one shift of each byte rather than a shift of the value and then a scale.
stretch_hist:
            lda     #CHUNKS
            sta     zp:nz_chunks

hchunk$:    ldz     #0

hpixel$:    lda     [nz_out],z
            sta     zp:nz_t
            inz
            lda     [nz_out],z
            inz
            sta     zp:nz_t+1

            ; offset high: vhi >> 4, straight onto the table's high byte
            lsr     a
            lsr     a
            lsr     a
            lsr     a
            clc
            adc     zp:nz_hist+1
            sta     zp:nz_ptr+1

            ; offset low: ((vhi << 4) | (vlo >> 4)) & 0xfc
            lda     zp:nz_t+1
            asl     a
            asl     a
            asl     a
            asl     a
            sta     zp:nz_d
            lda     zp:nz_t
            lsr     a
            lsr     a
            lsr     a
            lsr     a
            ora     zp:nz_d
            and     #0xfc
            clc
            adc     zp:nz_hist
            sta     zp:nz_ptr
            bcc     hadd$
            inc     zp:nz_ptr+1

hadd$:      ldy     #0
            lda     (nz_ptr),y
            clc
            adc     #1
            sta     (nz_ptr),y
            bcc     hnext$
            iny
            lda     (nz_ptr),y
            adc     #0
            sta     (nz_ptr),y
            bcc     hnext$
            iny
            lda     (nz_ptr),y
            adc     #0
            sta     (nz_ptr),y
            bcc     hnext$
            iny
            lda     (nz_ptr),y
            adc     #0
            sta     (nz_ptr),y

hnext$:     cpz     #CHUNK*2
            lbcc    hpixel$

            clc
            lda     zp:nz_out
            adc     #CHUNK*2
            sta     zp:nz_out
            bcc     hdone$
            inc     zp:nz_out+1
            bne     hdone$
            inc     zp:nz_out+2
            bne     hdone$
            inc     zp:nz_out+3
hdone$:     dec     zp:nz_chunks
            lbne    hchunk$
            ldz     #0
            rts

; --- paint -----------------------------------------------------------------
;
; The reciprocal is one number for the whole field, so it goes into the
; multiplier's B input once and stays there for both chunks and every row.
stretch_apply:
            lda     #0
            sta     MULTINA+2
            sta     MULTINA+3
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

achunk$:    ldz     #0

apixel$:    lda     [nz_out],z
            sta     zp:nz_t
            inz
            lda     [nz_out],z
            sta     zp:nz_t+1
            dez

            ; d = v - lo, floored at zero
            sec
            lda     zp:nz_t
            sbc     zp:nz_lo
            sta     MULTINA
            lda     zp:nz_t+1
            sbc     zp:nz_lo+1
            sta     MULTINA+1
            bcs     ascale$
            lda     #0                ; below the cut: the bottom of the range
            sta     MULTINA
            sta     MULTINA+1

ascale$:    ; anything above the sixteen bits we keep is over 1.0 and clips.
            ; The top half per cent of the field lands here by construction.
            lda     MULTOUT+4
            ora     MULTOUT+5
            ora     MULTOUT+6
            ora     MULTOUT+7
            beq     akeep$
            lda     #0xff
            sta     zp:nz_d
            sta     zp:nz_d+1
            bra     astore$

akeep$:     lda     MULTOUT+2
            sta     zp:nz_d
            lda     MULTOUT+3
            sta     zp:nz_d+1

            ; --- and onto the type's floor and range, in the same pass -----
            ; base_terrain's `floor + scale(n, range)`. Folded in here rather
            ; than given a pass of its own: it is a per-pixel function of one
            ; value, and a pass over the field costs about a second before it
            ; does any arithmetic.
            ;
            ; The multiplier's B is the stretch's reciprocal for the whole
            ; loop, so `range` goes in A and the value in B, which is the
            ; other way round from everything else here and costs nothing --
            ; the product is the same either way.
astore$:    lda     zp:nz_d
            sta     MULTINB
            lda     zp:nz_d+1
            sta     MULTINB+1
            lda     #0
            sta     MULTINB+2
            sta     MULTINB+3
            lda     zp:nz_range
            sta     MULTINA
            lda     zp:nz_range+1
            sta     MULTINA+1

            clc
            lda     MULTOUT+2
            adc     zp:nz_floor
            sta     zp:nz_d
            lda     MULTOUT+3
            adc     zp:nz_floor+1
            sta     zp:nz_d+1

            ; the reciprocal goes back into B for the next pixel
            lda     zp:nz_recip
            sta     MULTINB
            lda     zp:nz_recip+1
            sta     MULTINB+1
            lda     zp:nz_recip+2
            sta     MULTINB+2
            lda     zp:nz_recip+3
            sta     MULTINB+3

            lda     zp:nz_d
            sta     [nz_out],z
            inz
            lda     zp:nz_d+1
            sta     [nz_out],z
            inz

            ; Fletcher: a += v, b += a, both wrapping at 16 bits
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

            cpz     #CHUNK*2
            lbcc    apixel$

            clc
            lda     zp:nz_out
            adc     #CHUNK*2
            sta     zp:nz_out
            bcc     adone$
            inc     zp:nz_out+1
            bne     adone$
            inc     zp:nz_out+2
            bne     adone$
            inc     zp:nz_out+3
adone$:     dec     zp:nz_chunks
            lbne    achunk$
            ldz     #0
            rts
