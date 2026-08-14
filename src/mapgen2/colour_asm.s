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
