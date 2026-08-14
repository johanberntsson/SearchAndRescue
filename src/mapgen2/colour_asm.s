; The colour pass's per-pixel arithmetic: the two dither lattices, and the sun.
;
; This is `dither_at` from src/mapgen2/colour.c, called twice, in assembly. It
; is the densest use of the multiplier in the generator: six `lerp16`s and two
; scalings a pixel, eight trips to $D770 out of the twenty the land path makes.
;
; **What it replaces is not arithmetic, it is the walk to the multiplier.** In
; C each of those eight is a `jsr` to a wrapper that loads its operands off the
; software stack, writes eight bytes of register and reads four back. Here the
; operands are already in zero page and the product windows are read where they
; sit. The same numbers come out -- the colour checksum is unchanged -- for a
; fraction of the instructions.
;
; The shape, from tools/genmap.py's `value_noise` and `scale`:
;
;   n = lerp(lerp(c0[ix0], c0[ix1], wx),
;            lerp(c1[ix0], c1[ix1], wx), wy)
;   d = scale(2n - ONE, amp)
;
; **x first and then y**, which is numpy's order. Bilinear in fixed point is
; not order-independent, so the two inner lerps cannot be exchanged for one
; vertical pair however much cheaper that would be.
;
; The caller sets cl_ox0/cl_ox1 (the lattice columns, already doubled to byte
; offsets), cl_wx, and once a row cl_m0/cl_m1/cl_s0/cl_s1 and cl_wy. The two
; signed results land in cl_dith[0] and cl_dith[1].

            .rtmodel version, "1"
            .rtmodel core, "*"

            .extern cl_m0, cl_m1, cl_s0, cl_s1, cl_wy
            .extern cl_ox0, cl_ox1, cl_wx
            .extern cl_p0, cl_p1, cl_a, cl_b, cl_w, cl_r, cl_t0, cl_v
            .extern cl_amps, cl_dith
            .extern cl_hl, cl_hr, cl_neg, recip_sun
            .extern cl_sunref, cl_sunsat, cl_tanhp, cl_sunv

MULTINA:    .equ 0xd770
MULTINB:    .equ 0xd774
MULTOUT:    .equ 0xd778

            .section code, text
            .public cl_dither

cl_dither:
            ; the ramp's lattice into cl_dith[0]
            lda     zp:cl_m0
            sta     zp:cl_p0
            lda     zp:cl_m0+1
            sta     zp:cl_p0+1
            lda     zp:cl_m1
            sta     zp:cl_p1
            lda     zp:cl_m1+1
            sta     zp:cl_p1+1
            ldx     #0
            jsr     one$

            ; the sun's, into cl_dith[1]
            lda     zp:cl_s0
            sta     zp:cl_p0
            lda     zp:cl_s0+1
            sta     zp:cl_p0+1
            lda     zp:cl_s1
            sta     zp:cl_p1
            lda     zp:cl_s1+1
            sta     zp:cl_p1+1
            ldx     #4
            jsr     one$
            ; **Z must be zero on the way back to C.** Calypsi reaches through
            ; pointers with `lda (zp),z` and leaves the index there between
            ; uses, so a routine that returns with Z set offsets every later
            ; read. The lattice loads above leave it at the last column read.
            ldz     #0
            rts

; One lattice. X is the byte offset of this lattice's amp and result, so 0 for
; the ramp's and 4 for the sun's. X is not touched below; `lerp$` works in A
; and Z only.
one$:
            ; **The high halves are cleared here and not once at entry.** The
            ; amp is seventeen bits, so the scaling at the bottom writes all
            ; four bytes of B, and the second lattice's lerps would then read
            ; whatever the first one's amp left there. mask_asm.s learned the
            ; same lesson the same way.
            lda     #0
            sta     MULTINA+2
            sta     MULTINA+3
            sta     MULTINB+2
            sta     MULTINB+3

            ; top = lerp(c0[ix0], c0[ix1], wx)
            lda     zp:cl_ox0     ; LDZ has no zero-page mode
            taz
            lda     (zp:cl_p0),z
            sta     zp:cl_a
            inz
            lda     (zp:cl_p0),z
            sta     zp:cl_a+1
            lda     zp:cl_ox1
            taz
            lda     (zp:cl_p0),z
            sta     zp:cl_b
            inz
            lda     (zp:cl_p0),z
            sta     zp:cl_b+1
            lda     zp:cl_wx
            sta     zp:cl_w
            lda     zp:cl_wx+1
            sta     zp:cl_w+1
            jsr     cl_lerp
            lda     zp:cl_r
            sta     zp:cl_t0
            lda     zp:cl_r+1
            sta     zp:cl_t0+1

            ; bot = lerp(c1[ix0], c1[ix1], wx) -- w is still wx
            lda     zp:cl_ox0     ; LDZ has no zero-page mode
            taz
            lda     (zp:cl_p1),z
            sta     zp:cl_a
            inz
            lda     (zp:cl_p1),z
            sta     zp:cl_a+1
            lda     zp:cl_ox1
            taz
            lda     (zp:cl_p1),z
            sta     zp:cl_b
            inz
            lda     (zp:cl_p1),z
            sta     zp:cl_b+1
            jsr     cl_lerp

            ; n = lerp(top, bot, wy)
            lda     zp:cl_r
            sta     zp:cl_b
            lda     zp:cl_r+1
            sta     zp:cl_b+1
            lda     zp:cl_t0
            sta     zp:cl_a
            lda     zp:cl_t0+1
            sta     zp:cl_a+1
            lda     zp:cl_wy
            sta     zp:cl_w
            lda     zp:cl_wy+1
            sta     zp:cl_w+1
            jsr     cl_lerp

            ; --- scale(2n - ONE, amp) -------------------------------------
            ; The magnitude of 2n - ONE, which reaches 0x10000 exactly when n
            ; is zero -- seventeen bits, so MULTINA+2 is part of it.
            lda     zp:cl_r+1
            bmi     up$

            ; n < 0x8000: the term is negative. mag = (0x8000 - n) * 2
            sec
            lda     #0
            sbc     zp:cl_r
            sta     MULTINA
            lda     #0x80
            sbc     zp:cl_r+1
            sta     MULTINA+1
            jsr     dbl$
            jsr     amp$

            ; numpy floors a negative product, so any bit the shift discards
            ; rounds the magnitude away from zero before it is negated.
            lda     MULTOUT+2
            sta     zp:cl_v
            lda     MULTOUT+3
            sta     zp:cl_v+1
            lda     MULTOUT+4
            sta     zp:cl_v+2
            lda     MULTOUT+5
            sta     zp:cl_v+3
            lda     MULTOUT
            ora     MULTOUT+1
            beq     neg$
            inc     zp:cl_v
            bne     neg$
            inc     zp:cl_v+1
            bne     neg$
            inc     zp:cl_v+2
            bne     neg$
            inc     zp:cl_v+3
neg$:
            sec
            lda     #0
            sbc     zp:cl_v
            sta     cl_dith,x
            lda     #0
            sbc     zp:cl_v+1
            sta     cl_dith+1,x
            lda     #0
            sbc     zp:cl_v+2
            sta     cl_dith+2,x
            lda     #0
            sbc     zp:cl_v+3
            sta     cl_dith+3,x
            rts

            ; n >= 0x8000: positive, and the shift discards nothing that
            ; matters because a positive product truncates towards zero.
up$:
            sec
            lda     zp:cl_r
            sbc     #0
            sta     MULTINA
            lda     zp:cl_r+1
            sbc     #0x80
            sta     MULTINA+1
            jsr     dbl$
            jsr     amp$
            lda     MULTOUT+2
            sta     cl_dith,x
            lda     MULTOUT+3
            sta     cl_dith+1,x
            lda     MULTOUT+4
            sta     cl_dith+2,x
            lda     MULTOUT+5
            sta     cl_dith+3,x
            rts

; MULTINA <<= 1, carrying into the third byte. The fourth is cleared because
; the lerps above leave it zero and the product must not see a stale one.
dbl$:
            asl     MULTINA
            rol     MULTINA+1
            lda     #0
            adc     #0
            sta     MULTINA+2
            lda     #0
            sta     MULTINA+3
            rts

; MULTINB = cl_amps[x], all four bytes: MOTTLE is 1.4 in Q0.16 and does not
; fit a word.
amp$:
            lda     cl_amps,x
            sta     MULTINB
            lda     cl_amps+1,x
            sta     MULTINB+1
            lda     cl_amps+2,x
            sta     MULTINB+2
            lda     cl_amps+3,x
            sta     MULTINB+3
            rts

; --- shared ----------------------------------------------------------------
;
; cl_r = a + ((b - a) * w >> 16), the arithmetic shift spelled out.
;
; The multiplier is unsigned, so the two directions are separate routines
; rather than one signed multiply: downwards, numpy's floor is -(P >> 16) - 1
; whenever any low bit of P survives, which is what the low word is read for.
; This is `lerp16` in src/mapgen/fixed.h, instruction for instruction.
cl_lerp:
            lda     zp:cl_b
            cmp     zp:cl_a
            lda     zp:cl_b+1
            sbc     zp:cl_a+1
            bcc     down$

            sec
            lda     zp:cl_b
            sbc     zp:cl_a
            sta     MULTINA
            lda     zp:cl_b+1
            sbc     zp:cl_a+1
            sta     MULTINA+1
            lda     zp:cl_w
            sta     MULTINB
            lda     zp:cl_w+1
            sta     MULTINB+1
            clc
            lda     zp:cl_a
            adc     MULTOUT+2
            sta     zp:cl_r
            lda     zp:cl_a+1
            adc     MULTOUT+3
            sta     zp:cl_r+1
            rts

down$:
            sec
            lda     zp:cl_a
            sbc     zp:cl_b
            sta     MULTINA
            lda     zp:cl_a+1
            sbc     zp:cl_b+1
            sta     MULTINA+1
            lda     zp:cl_w
            sta     MULTINB
            lda     zp:cl_w+1
            sta     MULTINB+1
            sec
            lda     zp:cl_a
            sbc     MULTOUT+2
            sta     zp:cl_r
            lda     zp:cl_a+1
            sbc     MULTOUT+3
            sta     zp:cl_r+1
            lda     MULTOUT
            ora     MULTOUT+1
            beq     exact$
            lda     zp:cl_r
            bne     nolo$
            dec     zp:cl_r+1
nolo$:
            dec     zp:cl_r
exact$:
            rts


; --- the sun -----------------------------------------------------------------
;
; `sunlight_at` and `tanh16` from colour.c, times SHADES, in one routine. The
; caller leaves the two neighbouring heights in cl_hl and cl_hr and reads the
; answer from cl_sunv.
;
; **The sun is due west and on the horizon**, so the gradient dot product is
; one negation: rise is hl - hr and nothing else. See tools/genmap.py for how
; that bearing was measured off the hand-drawn map.
;
; Three things make this cheaper than it looks:
;
;   - **|rise| fits a word.** It is the difference of two Q0.16 heights, so the
;     32-bit `ddx` the C carried was never needed.
;   - **tanh saturates**, so any |rise| at or past 4 * SUN_REF gives the same
;     answer and needs no division at all.
;   - below that, `floor(|rise| << 16 / SUN_REF)` is a reciprocal multiply that
;     is exact or one low -- never more, because the numerator is under 2^30 --
;     so one comparison decides the correction. The C version does the same and
;     explains why.
;
; **It works in the dither's scratch.** Zero page is 91% full and the two are
; never live at once: the caller runs the sun, then the dither, then reads both
; results. cl_a, cl_b, cl_w, cl_r, cl_v and cl_p0 are all borrowed here.

            .section code, text
            .public cl_sun

cl_sun:
            ; m = |hr - hl| in cl_a, and cl_neg set when rise = hl - hr is
            ; negative. The equal case takes the negative branch and does not
            ; care: m is zero, so tanh is zero and the two agree.
            sec
            lda     zp:cl_hr
            sbc     zp:cl_hl
            sta     zp:cl_a
            lda     zp:cl_hr+1
            sbc     zp:cl_hl+1
            sta     zp:cl_a+1
            bcs     falls$

            sec
            lda     zp:cl_hl
            sbc     zp:cl_hr
            sta     zp:cl_a
            lda     zp:cl_hl+1
            sbc     zp:cl_hr+1
            sta     zp:cl_a+1
            lda     #0
            sta     zp:cl_neg
            bra     sat$
falls$:
            lda     #1
            sta     zp:cl_neg

sat$:
            ; past 4 * SUN_REF the tanh table is flat: m2 is 1.0 and there is
            ; no division to do.
            lda     zp:cl_a
            cmp     cl_sunsat
            lda     zp:cl_a+1
            sbc     cl_sunsat+1
            bcc     divide$
            lda     #0
            sta     zp:cl_v
            sta     zp:cl_v+1
            sta     zp:cl_v+3
            lda     #1
            sta     zp:cl_v+2
            jmp     look$

divide$:
            ; e = (m * recip_sun) >> 16, which is (m << 16) / SUN_REF to within
            ; one -- the shift cancels against the one this read performs.
            lda     zp:cl_a
            sta     MULTINA
            lda     zp:cl_a+1
            sta     MULTINA+1
            lda     #0
            sta     MULTINA+2
            sta     MULTINA+3
            lda     zp:recip_sun
            sta     MULTINB
            lda     zp:recip_sun+1
            sta     MULTINB+1
            lda     zp:recip_sun+2
            sta     MULTINB+2
            lda     zp:recip_sun+3
            sta     MULTINB+3
            lda     MULTOUT+2
            sta     zp:cl_v
            lda     MULTOUT+3
            sta     zp:cl_v+1
            lda     MULTOUT+4
            sta     zp:cl_v+2
            lda     MULTOUT+5
            sta     zp:cl_v+3

            ; the correction: if (e + 1) * SUN_REF still fits under m << 16,
            ; the estimate was the one that came up short.
            clc
            lda     zp:cl_v
            adc     #1
            sta     MULTINA
            lda     zp:cl_v+1
            adc     #0
            sta     MULTINA+1
            lda     zp:cl_v+2
            adc     #0
            sta     MULTINA+2
            lda     zp:cl_v+3
            adc     #0
            sta     MULTINA+3
            jsr     sref$
            jsr     undermag$
            bcc     signed$
            jsr     bump$

signed$:
            ; the negative side takes a ceiling, not a floor: if the division
            ; left a remainder the magnitude rounds up before it is negated.
            lda     zp:cl_neg
            beq     quarter$
            lda     zp:cl_v
            sta     MULTINA
            lda     zp:cl_v+1
            sta     MULTINA+1
            lda     zp:cl_v+2
            sta     MULTINA+2
            lda     zp:cl_v+3
            sta     MULTINA+3
            jsr     sref$
            lda     MULTOUT
            bne     rounds$
            lda     MULTOUT+1
            bne     rounds$
            lda     MULTOUT+2
            cmp     zp:cl_a
            bne     rounds$
            lda     MULTOUT+3
            cmp     zp:cl_a+1
            beq     quarter$
rounds$:
            jsr     bump$

quarter$:
            ; the table is read over 0..4, so the argument is divided by four
            ; -- which is where tanh16's `m >>= 2` went.
            lsr     zp:cl_v+3
            ror     zp:cl_v+2
            ror     zp:cl_v+1
            ror     zp:cl_v
            lsr     zp:cl_v+3
            ror     zp:cl_v+2
            ror     zp:cl_v+1
            ror     zp:cl_v

look$:
            ; a 257-entry table read at eight bits with the rest interpolated.
            ; The third byte is set only when the argument is exactly 1.0,
            ; which is the last entry and has no successor to interpolate with.
            lda     zp:cl_v+2
            bne     last$

            lda     zp:cl_v+1
            asl     a
            sta     zp:cl_p0
            lda     #0
            rol     a
            sta     zp:cl_p0+1
            clc
            lda     zp:cl_p0
            adc     cl_tanhp
            sta     zp:cl_p0
            lda     zp:cl_p0+1
            adc     cl_tanhp+1
            sta     zp:cl_p0+1
            ldz     #0
            lda     (zp:cl_p0),z
            sta     zp:cl_a
            inz
            lda     (zp:cl_p0),z
            sta     zp:cl_a+1
            inz
            lda     (zp:cl_p0),z
            sta     zp:cl_b
            inz
            lda     (zp:cl_p0),z
            sta     zp:cl_b+1
            lda     #0
            sta     zp:cl_w
            lda     zp:cl_v
            sta     zp:cl_w+1
            lda     #0
            sta     MULTINA+2
            sta     MULTINA+3
            sta     MULTINB+2
            sta     MULTINB+3
            jsr     cl_lerp
            jmp     lit$

last$:
            clc
            lda     cl_tanhp
            sta     zp:cl_p0
            lda     cl_tanhp+1
            adc     #2                ; 256 entries of two bytes
            sta     zp:cl_p0+1
            ldz     #0
            lda     (zp:cl_p0),z
            sta     zp:cl_r
            inz
            lda     (zp:cl_p0),z
            sta     zp:cl_r+1

lit$:
            ; ONE - tanh, in three bytes because it reaches 1.0 exactly, then
            ; halved. Positive throughout, so the shift needs no sign.
            lda     zp:cl_neg
            bne     away$
            sec
            lda     #0
            sbc     zp:cl_r
            sta     zp:cl_v
            lda     #0
            sbc     zp:cl_r+1
            sta     zp:cl_v+1
            lda     #1
            sbc     #0
            sta     zp:cl_v+2
            bra     half$
away$:
            lda     zp:cl_r
            sta     zp:cl_v
            lda     zp:cl_r+1
            sta     zp:cl_v+1
            lda     #1
            sta     zp:cl_v+2
half$:
            lsr     zp:cl_v+2
            ror     zp:cl_v+1
            ror     zp:cl_v

            ; times SHADES, which is what the caller wants of it
            lda     zp:cl_v
            sta     MULTINA
            lda     zp:cl_v+1
            sta     MULTINA+1
            lda     #0
            sta     MULTINA+2
            sta     MULTINA+3
            lda     #6
            sta     MULTINB
            lda     #0
            sta     MULTINB+1
            sta     MULTINB+2
            sta     MULTINB+3
            lda     MULTOUT
            sta     cl_sunv
            lda     MULTOUT+1
            sta     cl_sunv+1
            lda     MULTOUT+2
            sta     cl_sunv+2
            lda     MULTOUT+3
            sta     cl_sunv+3
            ldz     #0                ; see the note at the end of cl_dither
            rts

; MULTINB = SUN_REF, all four bytes.
sref$:
            lda     cl_sunref
            sta     MULTINB
            lda     cl_sunref+1
            sta     MULTINB+1
            lda     cl_sunref+2
            sta     MULTINB+2
            lda     cl_sunref+3
            sta     MULTINB+3
            rts

; Carry set if the product is at or under m << 16 -- whose four bytes are
; 0, 0, m_lo, m_hi, so no copy of it has to exist anywhere.
undermag$:
            sec
            lda     #0
            sbc     MULTOUT
            lda     #0
            sbc     MULTOUT+1
            lda     zp:cl_a
            sbc     MULTOUT+2
            lda     zp:cl_a+1
            sbc     MULTOUT+3
            rts

bump$:
            inc     zp:cl_v
            bne     bumped$
            inc     zp:cl_v+1
            bne     bumped$
            inc     zp:cl_v+2
            bne     bumped$
            inc     zp:cl_v+3
bumped$:
            rts

; --- the land ramp -----------------------------------------------------------
;
; What is left of the land pixel after the sun and the dither: the height
; through the gamma, the slope that pushes a pixel up the ramp, and the two
; clamps that turn the result into a palette index.
;
;   t     = gamma(clip(h - sea) / top)
;   slope = sqrt(dy^2 + dx^2) / SLOPE_REF
;   t     = (t + slope * SLOPE_PUSH) * CEILING
;   step  = clip((t * 21 + mottle) >> 16, 0, 20)
;   face  = clip((sun + smottle) >> 16, 0, 5)
;   index = 24 + step * 6 + face
;
; The caller leaves h in cl_hh and the two vertical neighbours in cl_du and
; cl_dd; cl_hl and cl_hr are still the sun's, and the horizontal difference is
; taken from them again rather than passed. cl_dith and cl_sunv are read where
; the two routines above left them. The answer is one byte in cl_idx.
;
; Two things the C version does that are dead here and are not ported:
;
;   - **the sum of two squares cannot exceed 1.0 once shifted**, because it is
;     a 32-bit value shifted right sixteen, so `sq > ONE` is unreachable. Only
;     the carry out of the addition means anything, and it means 1.0.
;   - for the same reason `sqrt16`'s `x >= ONE` early-out never fires, so the
;     root below is always the table read.

            .extern cl_hh, cl_du, cl_dd, cl_sea, cl_idx, cl_s
            .extern cl_gammap, cl_sqrtp, cl_sqrtdp, cl_ceiling, cl_push
            .extern recip_top, recip_slope

            .section code, text
            .public cl_land

cl_land:
            ; --- t = (h - sea) / (top - sea) ------------------------------
            lda     cl_hh
            cmp     cl_sea
            lda     cl_hh+1
            sbc     cl_sea+1
            bcc     under$

            sec
            lda     cl_hh
            sbc     cl_sea
            sta     MULTINA
            lda     cl_hh+1
            sbc     cl_sea+1
            sta     MULTINA+1
            lda     #0
            sta     MULTINA+2
            sta     MULTINA+3
            lda     zp:recip_top
            sta     MULTINB
            lda     zp:recip_top+1
            sta     MULTINB+1
            lda     zp:recip_top+2
            sta     MULTINB+2
            lda     zp:recip_top+3
            sta     MULTINB+3
            lda     MULTOUT+2
            sta     zp:cl_v
            lda     MULTOUT+3
            sta     zp:cl_v+1
            lda     MULTOUT+4
            sta     zp:cl_v+2
            lda     MULTOUT+5
            sta     zp:cl_v+3
            bra     ceil$
under$:
            lda     #0
            sta     zp:cl_v
            sta     zp:cl_v+1
            sta     zp:cl_v+2
            sta     zp:cl_v+3

ceil$:
            ; the gamma is read over 0..2, so clip there and halve
            lda     zp:cl_v+3
            bne     over$
            lda     zp:cl_v+2
            cmp     #3
            bcs     over$
            cmp     #2
            bcc     halve$
            lda     zp:cl_v
            ora     zp:cl_v+1
            beq     halve$
over$:
            lda     #0
            sta     zp:cl_v
            sta     zp:cl_v+1
            sta     zp:cl_v+3
            lda     #2
            sta     zp:cl_v+2
halve$:
            lsr     zp:cl_v+3
            ror     zp:cl_v+2
            ror     zp:cl_v+1
            ror     zp:cl_v

            ; --- t = gamma[t], interpolated -------------------------------
            ; A 257-entry table of longwords. The third byte is set only when
            ; the argument is exactly 1.0, which is the last entry.
            lda     zp:cl_v+2
            beq     ginterp$
            jmp     gtop$
ginterp$:
            lda     zp:cl_v+1
            asl     a
            sta     zp:cl_p0
            lda     #0
            rol     a
            sta     zp:cl_p0+1
            asl     zp:cl_p0
            rol     zp:cl_p0+1
            clc
            lda     zp:cl_p0
            adc     cl_gammap
            sta     zp:cl_p0
            lda     zp:cl_p0+1
            adc     cl_gammap+1
            sta     zp:cl_p0+1

            ldz     #0
            lda     (zp:cl_p0),z
            sta     zp:cl_s
            inz
            lda     (zp:cl_p0),z
            sta     zp:cl_s+1
            inz
            lda     (zp:cl_p0),z
            sta     zp:cl_s+2
            inz
            lda     (zp:cl_p0),z
            sta     zp:cl_s+3
            ; the next entry, differenced against this one as it is read: the
            ; ramp only rises, so the difference is never negative.
            inz
            sec
            lda     (zp:cl_p0),z
            sbc     zp:cl_s
            sta     MULTINA
            inz
            lda     (zp:cl_p0),z
            sbc     zp:cl_s+1
            sta     MULTINA+1
            inz
            lda     (zp:cl_p0),z
            sbc     zp:cl_s+2
            sta     MULTINA+2
            inz
            lda     (zp:cl_p0),z
            sbc     zp:cl_s+3
            sta     MULTINA+3
            lda     #0
            sta     MULTINB
            lda     zp:cl_v                 ; the weight, one byte shifted up
            sta     MULTINB+1
            lda     #0
            sta     MULTINB+2
            sta     MULTINB+3
            clc
            lda     zp:cl_s
            adc     MULTOUT+2
            sta     zp:cl_v
            lda     zp:cl_s+1
            adc     MULTOUT+3
            sta     zp:cl_v+1
            lda     zp:cl_s+2
            adc     MULTOUT+4
            sta     zp:cl_v+2
            lda     zp:cl_s+3
            adc     MULTOUT+5
            sta     zp:cl_v+3
            bra     rise$
gtop$:
            clc
            lda     cl_gammap
            sta     zp:cl_p0
            lda     cl_gammap+1
            adc     #4                      ; 256 entries of four bytes
            sta     zp:cl_p0+1
            ldz     #0
            lda     (zp:cl_p0),z
            sta     zp:cl_v
            inz
            lda     (zp:cl_p0),z
            sta     zp:cl_v+1
            inz
            lda     (zp:cl_p0),z
            sta     zp:cl_v+2
            inz
            lda     (zp:cl_p0),z
            sta     zp:cl_v+3

rise$:
            ; --- the slope ------------------------------------------------
            sec
            lda     cl_dd
            sbc     cl_du
            sta     zp:cl_a
            lda     cl_dd+1
            sbc     cl_du+1
            sta     zp:cl_a+1
            bcs     dyup$
            sec
            lda     cl_du
            sbc     cl_dd
            sta     zp:cl_a
            lda     cl_du+1
            sbc     cl_dd+1
            sta     zp:cl_a+1
dyup$:
            jsr     square$
            lda     MULTOUT
            sta     zp:cl_s
            lda     MULTOUT+1
            sta     zp:cl_s+1
            lda     MULTOUT+2
            sta     zp:cl_s+2
            lda     MULTOUT+3
            sta     zp:cl_s+3

            sec
            lda     zp:cl_hr
            sbc     zp:cl_hl
            sta     zp:cl_a
            lda     zp:cl_hr+1
            sbc     zp:cl_hl+1
            sta     zp:cl_a+1
            bcs     dxup$
            sec
            lda     zp:cl_hl
            sbc     zp:cl_hr
            sta     zp:cl_a
            lda     zp:cl_hl+1
            sbc     zp:cl_hr+1
            sta     zp:cl_a+1
dxup$:
            jsr     square$
            clc
            lda     zp:cl_s
            adc     MULTOUT
            sta     zp:cl_s
            lda     zp:cl_s+1
            adc     MULTOUT+1
            sta     zp:cl_s+1
            lda     zp:cl_s+2
            adc     MULTOUT+2
            sta     zp:cl_s+2
            lda     zp:cl_s+3
            adc     MULTOUT+3
            sta     zp:cl_s+3
            bcs     steep$

            lda     zp:cl_s+2
            sta     zp:cl_a
            lda     zp:cl_s+3
            sta     zp:cl_a+1
            jsr     root$
            bra     push$
steep$:
            lda     #0                      ; the sum carried: 1.0, and its
            sta     zp:cl_s                 ; root is 1.0 as well
            sta     zp:cl_s+1
            sta     zp:cl_s+3
            lda     #1
            sta     zp:cl_s+2

push$:
            ; slope / SLOPE_REF, clipped at 1.0
            jsr     slopein$
            lda     zp:recip_slope
            sta     MULTINB
            lda     zp:recip_slope+1
            sta     MULTINB+1
            lda     zp:recip_slope+2
            sta     MULTINB+2
            lda     zp:recip_slope+3
            sta     MULTINB+3
            lda     MULTOUT+2
            sta     zp:cl_s
            lda     MULTOUT+3
            sta     zp:cl_s+1
            lda     MULTOUT+4
            sta     zp:cl_s+2
            lda     MULTOUT+5
            sta     zp:cl_s+3

            lda     zp:cl_s+3
            bne     clip$
            lda     zp:cl_s+2
            beq     pushed$
            cmp     #1
            bne     clip$
            lda     zp:cl_s
            ora     zp:cl_s+1
            beq     pushed$
clip$:
            lda     #0
            sta     zp:cl_s
            sta     zp:cl_s+1
            sta     zp:cl_s+3
            lda     #1
            sta     zp:cl_s+2
pushed$:
            ; t += slope * SLOPE_PUSH
            jsr     slopein$
            lda     cl_push
            sta     MULTINB
            lda     cl_push+1
            sta     MULTINB+1
            lda     cl_push+2
            sta     MULTINB+2
            lda     cl_push+3
            sta     MULTINB+3
            clc
            lda     zp:cl_v
            adc     MULTOUT+2
            sta     zp:cl_v
            lda     zp:cl_v+1
            adc     MULTOUT+3
            sta     zp:cl_v+1
            lda     zp:cl_v+2
            adc     MULTOUT+4
            sta     zp:cl_v+2
            lda     zp:cl_v+3
            adc     MULTOUT+5
            sta     zp:cl_v+3

            ; t *= the type's ceiling
            jsr     tin$
            lda     cl_ceiling
            sta     MULTINB
            lda     cl_ceiling+1
            sta     MULTINB+1
            lda     cl_ceiling+2
            sta     MULTINB+2
            lda     cl_ceiling+3
            sta     MULTINB+3
            lda     MULTOUT+2
            sta     zp:cl_v
            lda     MULTOUT+3
            sta     zp:cl_v+1
            lda     MULTOUT+4
            sta     zp:cl_v+2
            lda     MULTOUT+5
            sta     zp:cl_v+3

            ; --- step = clip((t * 21 + mottle) >> 16, 0, 20) --------------
            jsr     tin$
            lda     #21
            sta     MULTINB
            lda     #0
            sta     MULTINB+1
            sta     MULTINB+2
            sta     MULTINB+3
            clc
            lda     MULTOUT
            adc     cl_dith
            lda     MULTOUT+1
            adc     cl_dith+1
            lda     MULTOUT+2
            adc     cl_dith+2
            sta     zp:cl_a
            lda     MULTOUT+3
            adc     cl_dith+3
            bmi     nostep$
            bne     maxstep$
            lda     zp:cl_a
            cmp     #21
            bcc     gotstep$
maxstep$:
            lda     #20
            sta     zp:cl_a
            bra     gotstep$
nostep$:
            lda     #0
            sta     zp:cl_a
gotstep$:
            ; --- face = clip((sun + smottle) >> 16, 0, 5) -----------------
            clc
            lda     cl_sunv
            adc     cl_dith+4
            lda     cl_sunv+1
            adc     cl_dith+5
            lda     cl_sunv+2
            adc     cl_dith+6
            sta     zp:cl_b
            lda     cl_sunv+3
            adc     cl_dith+7
            bmi     noface$
            bne     maxface$
            lda     zp:cl_b
            cmp     #6
            bcc     gotface$
maxface$:
            lda     #5
            sta     zp:cl_b
            bra     gotface$
noface$:
            lda     #0
            sta     zp:cl_b
gotface$:
            ; --- 24 + step * 6 + face -------------------------------------
            lda     zp:cl_a
            asl     a
            sta     zp:cl_t0
            asl     a
            clc
            adc     zp:cl_t0
            clc
            adc     zp:cl_b
            clc
            adc     #24
            sta     cl_idx
            ldz     #0                ; see the note at the end of cl_dither
            rts

; cl_a squared, in MULTOUT. Both differences fit a word, so both squares fit a
; longword and only the low half of the product is ever read.
square$:
            lda     zp:cl_a
            sta     MULTINA
            sta     MULTINB
            lda     zp:cl_a+1
            sta     MULTINA+1
            sta     MULTINB+1
            lda     #0
            sta     MULTINA+2
            sta     MULTINA+3
            sta     MULTINB+2
            sta     MULTINB+3
            rts

; MULTINA = the slope, MULTINA = t: the two operands this routine multiplies
; more than once.
slopein$:
            lda     zp:cl_s
            sta     MULTINA
            lda     zp:cl_s+1
            sta     MULTINA+1
            lda     zp:cl_s+2
            sta     MULTINA+2
            lda     zp:cl_s+3
            sta     MULTINA+3
            rts
tin$:
            lda     zp:cl_v
            sta     MULTINA
            lda     zp:cl_v+1
            sta     MULTINA+1
            lda     zp:cl_v+2
            sta     MULTINA+2
            lda     zp:cl_v+3
            sta     MULTINA+3
            rts

; The square root of cl_a, a Q0.16 fraction under 1.0, into cl_s.
;
; **The input is normalised into 0.25..1 first**, and that is the whole trick:
; a root's slope is infinite at zero, so a table read straight at small x is
; wrong by up to 1.5% of full scale. Shifting up in pairs of bits until the top
; is set puts every input on the flat part of the curve, and shifting the
; result back down by half as many is exact, because sqrt(4^k) is 2^k.
root$:
            lda     zp:cl_a
            ora     zp:cl_a+1
            bne     rt1$
            sta     zp:cl_s
            sta     zp:cl_s+1
            sta     zp:cl_s+2
            sta     zp:cl_s+3
            rts
rt1$:
            ldx     #0
rtnorm$:
            lda     zp:cl_a+1
            cmp     #0x40                   ; 0.25 in the high byte
            bcs     rtread$
            asl     zp:cl_a
            rol     zp:cl_a+1
            asl     zp:cl_a
            rol     zp:cl_a+1
            inx
            bra     rtnorm$
rtread$:
            lda     zp:cl_a+1
            asl     a
            sta     zp:cl_t0
            lda     #0
            rol     a
            sta     zp:cl_t0+1

            clc
            lda     zp:cl_t0
            adc     cl_sqrtdp
            sta     zp:cl_p0
            lda     zp:cl_t0+1
            adc     cl_sqrtdp+1
            sta     zp:cl_p0+1
            ldz     #0
            lda     (zp:cl_p0),z
            sta     MULTINA
            inz
            lda     (zp:cl_p0),z
            sta     MULTINA+1
            lda     #0
            sta     MULTINA+2
            sta     MULTINA+3
            sta     MULTINB
            lda     zp:cl_a
            sta     MULTINB+1
            lda     #0
            sta     MULTINB+2
            sta     MULTINB+3

            clc
            lda     zp:cl_t0
            adc     cl_sqrtp
            sta     zp:cl_p0
            lda     zp:cl_t0+1
            adc     cl_sqrtp+1
            sta     zp:cl_p0+1
            ldz     #0
            clc
            lda     (zp:cl_p0),z
            adc     MULTOUT+2
            sta     zp:cl_s
            inz
            lda     (zp:cl_p0),z
            adc     MULTOUT+3
            sta     zp:cl_s+1
            lda     #0
            adc     #0
            sta     zp:cl_s+2               ; sqrt(1.0) is seventeen bits
            lda     #0
            sta     zp:cl_s+3
rtdown$:
            cpx     #0
            beq     rtend$
            lsr     zp:cl_s+2
            ror     zp:cl_s+1
            ror     zp:cl_s
            dex
            bra     rtdown$
rtend$:
            rts
