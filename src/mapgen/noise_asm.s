; The map generator's inner loop: one octave's contribution to one row.
;
;   for x in 0..511:
;       acc[x] += mulhi(lerp16(top[x], bot[x], wy), amp)
;
; which is the whole of noise_run's per-pixel work once the two lattice rows
; are cached (see noise.c). In C it cost 18.01 of the generator's 31.53
; seconds, for the reason the renderer's march did before voxel_asm.s existed:
; the compiler will not inline a two-line multiply, keeps the loop's locals on
; the software stack, and cross-calls the body into shared fragments.
;
; **The proof that this is the same arithmetic is the checksum.** The device
; prints one and tools/fbmcheck.py prints the same from the PC, so any
; disagreement shows up as sixteen wrong characters rather than as a map that
; looks a bit different. That is the same bargain the march took: 302 to 182
; cycles a sample, verified pixel-identical.
;
; Two things about the arithmetic that are not free to get wrong:
;
;   - numpy's `>>` on a negative value floors, so a downward interpolation is
;     top - hi - (lo != 0) and not top - hi. The low word of the product is
;     read only for that test.
;   - the multiplier is unsigned, so the sign is taken out of the difference
;     first and put back by choosing an add or a subtract.

            .rtmodel version, "1"
            .rtmodel core, "*"

            .extern nz_top, nz_bot, nz_acc, nz_wy, nz_amp
            .extern nz_t, nz_b, nz_d, nz_n, nz_chunks

MULTINA:    .equ 0xd770
MULTINB:    .equ 0xd774
MULTOUT:    .equ 0xd778

; 64 pixels a chunk is what keeps both index registers eight bits wide: the
; edge rows are 16-bit entries, so Z walks 0..126 by twos, and acc is 32-bit,
; so Y walks 0..252 by fours. Eight chunks make the 512-pixel row, and the
; pointers move on between them.
CHUNK:      .equ 64
CHUNKS:     .equ 512 / CHUNK

            .section code, text
            .public noise_blend

noise_blend:
            ; The multiplier is 32x32. Both inputs are 16-bit here, so the top
            ; halves stay zero -- A's for the whole call, since |d| and n are
            ; both under 65536. B's are written per multiply below, because
            ; amp is ONE on the first octave and does not fit sixteen bits.
            lda     #0
            sta     MULTINA+2
            sta     MULTINA+3
            lda     #CHUNKS
            sta     zp:nz_chunks

chunk$:     ldz     #0
            ldy     #0

pixel$:     ; --- top and bot for this column -------------------------------
            lda     (nz_top),z
            sta     zp:nz_t
            lda     (nz_bot),z
            sta     zp:nz_b
            inz
            lda     (nz_top),z
            sta     zp:nz_t+1
            lda     (nz_bot),z
            sta     zp:nz_b+1
            dez

            ; --- d = bot - top, and its sign ------------------------------
            sec
            lda     zp:nz_b
            sbc     zp:nz_t
            sta     zp:nz_d
            lda     zp:nz_b+1
            sbc     zp:nz_t+1
            sta     zp:nz_d+1
            bcs     up$

            ; bot < top: the multiplier wants the magnitude
            sec
            lda     zp:nz_t
            sbc     zp:nz_b
            sta     zp:nz_d
            lda     zp:nz_t+1
            sbc     zp:nz_b+1
            sta     zp:nz_d+1

            ; --- n = top - ceil(|d| * wy / 65536) -------------------------
            lda     zp:nz_d
            sta     MULTINA
            lda     zp:nz_d+1
            sta     MULTINA+1
            lda     zp:nz_wy
            sta     MULTINB
            lda     zp:nz_wy+1
            sta     MULTINB+1
            lda     #0
            sta     MULTINB+2
            sta     MULTINB+3

            sec
            lda     zp:nz_t
            sbc     MULTOUT+2
            sta     zp:nz_n
            lda     zp:nz_t+1
            sbc     MULTOUT+3
            sta     zp:nz_n+1
            ; floor, not truncate: anything left in the low word rounds the
            ; magnitude up, which takes the result one further down.
            lda     MULTOUT
            ora     MULTOUT+1
            beq     scale$
            lda     zp:nz_n
            bne     nolo$
            dec     zp:nz_n+1
nolo$:      dec     zp:nz_n
            bra     scale$

up$:        ; --- n = top + (d * wy >> 16) ---------------------------------
            lda     zp:nz_d
            sta     MULTINA
            lda     zp:nz_d+1
            sta     MULTINA+1
            lda     zp:nz_wy
            sta     MULTINB
            lda     zp:nz_wy+1
            sta     MULTINB+1
            lda     #0
            sta     MULTINB+2
            sta     MULTINB+3

            clc
            lda     zp:nz_t
            adc     MULTOUT+2
            sta     zp:nz_n
            lda     zp:nz_t+1
            adc     MULTOUT+3
            sta     zp:nz_n+1

scale$:     ; --- acc[x] += n * amp >> 16 ----------------------------------
            lda     zp:nz_n
            sta     MULTINA
            lda     zp:nz_n+1
            sta     MULTINA+1
            lda     zp:nz_amp
            sta     MULTINB
            lda     zp:nz_amp+1
            sta     MULTINB+1
            lda     zp:nz_amp+2
            sta     MULTINB+2
            lda     zp:nz_amp+3
            sta     MULTINB+3

            clc
            lda     (nz_acc),y
            adc     MULTOUT+2
            sta     (nz_acc),y
            iny
            lda     (nz_acc),y
            adc     MULTOUT+3
            sta     (nz_acc),y
            iny
            lda     (nz_acc),y
            adc     #0
            sta     (nz_acc),y
            iny
            lda     (nz_acc),y
            adc     #0
            sta     (nz_acc),y
            iny

            inz
            inz
            cpz     #CHUNK*2
            lbcc    pixel$

            ; --- on to the next chunk of the row --------------------------
            clc
            lda     zp:nz_top
            adc     #CHUNK*2
            sta     zp:nz_top
            bcc     bot$
            inc     zp:nz_top+1
bot$:       clc
            lda     zp:nz_bot
            adc     #CHUNK*2
            sta     zp:nz_bot
            bcc     acc$
            inc     zp:nz_bot+1
acc$:       ; 64 pixels of acc is exactly 256 bytes, so the low byte cannot move
            inc     zp:nz_acc+1

            dec     zp:nz_chunks
            lbne    chunk$
            rts
