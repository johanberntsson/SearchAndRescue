; The renderer's inner loop, in 45GS02 assembly.
;
; The C version of this measured 1392 cycles per heightmap sample: the compiler
; builds the loop out of jsr fragments and keeps locals on the software stack,
; so a handful of adds and one memory read turn into hundreds of cycles. This
; keeps everything in zero page, samples the maps with a single 32-bit indirect
; load, and drives the hardware multiplier directly.
;
; Parameters live in zero page and are set up by voxel_column() in voxel.c.

            .extern vx_hptr, vx_cptr, vx_fbtop
            .extern vx_px, vx_py, vx_stepx, vx_stepy
            .extern vx_camh, vx_ybuf, vx_ys, vx_tmp
            .extern vx_bands, vx_bandsteps, vx_band, vx_step
            .extern vx_inv_z, vx_horiz
#if PROFILE_DETAIL
            ; Per-column event counts, summed into the profiler by column().
            ; A column cannot take more than 64 samples, draw more than 64
            ; spans or fill more than FB_HEIGHT pixels, so a byte holds each.
            .extern vx_n_sample, vx_n_span, vx_n_spanpix
#endif

MULTINA:    .equlab 0xd770
MULTINB:    .equlab 0xd774
MULTOUT:    .equlab 0xd778

            .section code,text
            .public voxel_column_asm

voxel_column_asm:
            ; The height difference is biased positive (CAM_BIAS in voxel.c),
            ; so the multiplier's A input is unsigned and its top half is zero
            ; for every sample. The C side's own multiplies write all four
            ; bytes, so it has to be set again each column.
            lda     #0
            sta     MULTINA+2
            sta     MULTINA+3

            ; Y indexes inv_z and vx_horiz for the whole march. Nothing else
            ; in the step or span loops touches it.
            ldy     #0
            lda     zp:vx_bands
            sta     zp:vx_band

band$:      lda     zp:vx_bandsteps
            sta     zp:vx_step

step$:
#if PROFILE_DETAIL
            inc     zp:vx_n_sample
#endif
            ; px += stepx, py += stepy, in 8.8. Two 16-bit adds rather than
            ; one 32-bit one, because a carry out of px must not reach py: px
            ; and py wrap independently, which is what wraps the map.
            ;
            ; The whole-cell halves are kept *in the map pointer*. The map is
            ; 256x256 on a 64K boundary, so those two bytes already are the
            ; cell address -- adding into them in place is what the pointer
            ; wanted anyway, and it saves copying them across every sample.
            clc
            lda     zp:vx_px
            adc     zp:vx_stepx
            sta     zp:vx_px
            lda     zp:vx_hptr
            adc     zp:vx_stepx+1
            sta     zp:vx_hptr
            clc
            lda     zp:vx_py
            adc     zp:vx_stepy
            sta     zp:vx_py
            lda     zp:vx_hptr+1
            adc     zp:vx_stepy+1
            sta     zp:vx_hptr+1

            ldz     #0
            lda     [vx_hptr],z
            sta     zp:vx_tmp

            ; dh = camh - height, positive because camh carries CAM_BIAS.
            sec
            lda     zp:vx_camh
            sbc     zp:vx_tmp
            sta     MULTINA
            lda     zp:vx_camh+1
            sbc     #0
            sta     MULTINA+1

            ; MULTINB = inv_z[k]. Its top half is zeroed by column().
            lda     vx_inv_z,y
            sta     MULTINB
            lda     vx_inv_z+1,y
            sta     MULTINB+1

            ; ys = horiz[k] + (product >> 8): bytes 1 and 2 are the shift.
            ; vx_horiz has the bias taken back out of the horizon.
            clc
            lda     vx_horiz,y
            adc     MULTOUT+1
            sta     zp:vx_ys
            lda     vx_horiz+1,y
            adc     MULTOUT+2
            sta     zp:vx_ys+1

            bmi     clamp$          ; above the top of the screen
            bne     next$           ; 256 or more, so below anything drawn
            lda     zp:vx_ys
            cmp     zp:vx_ybuf
            bcs     next$           ; hidden behind something nearer
            bra     draw$
clamp$:     lda     #0
            sta     zp:vx_ys

draw$:      ; Same cell in the colour map, which is at twice the resolution:
            ; four 64K planes, one per half-cell corner. The cell address is
            ; the same two bytes, and bit 7 of each position fraction picks
            ; the plane, which is the bank byte. X is free until the span
            ; length is worked out below.
            lda     zp:vx_hptr
            sta     zp:vx_cptr
            lda     zp:vx_hptr+1
            sta     zp:vx_cptr+1
            ldx     #0
            lda     zp:vx_px
            bpl     xlo$
            ldx     #1
xlo$:       lda     zp:vx_py
            bpl     ylo$
            inx
            inx
ylo$:       stx     zp:vx_cptr+2
            ldz     #0
            lda     [vx_cptr],z
            sta     zp:vx_tmp       ; colour

            ; vx_fbtop already addresses row vx_ybuf, so there is no address to
            ; work out: the span fills upwards from there and lands on row
            ; vx_ys, which is where the next span starts.
            sec
            lda     zp:vx_ybuf
            sbc     zp:vx_ys
            tax
#if PROFILE_DETAIL
            ; A is the span length and is about to be reloaded from vx_tmp.
            inc     zp:vx_n_span
            clc
            adc     zp:vx_n_spanpix
            sta     zp:vx_n_spanpix
#endif
            ldz     #0
            ; One pixel every 8 bytes, upwards. Stepping back before the write
            ; rather than after it is what leaves the pointer on row vx_ys.
fill$:      lda     zp:vx_fbtop
            sec
            sbc     #8
            sta     zp:vx_fbtop
            bcs     nowrap$
            dec     zp:vx_fbtop+1
nowrap$:    lda     zp:vx_tmp
            sta     [vx_fbtop],z
            dex
            bne     fill$

            lda     zp:vx_ys
            sta     zp:vx_ybuf
            beq     done$           ; column is full

            ; Y advances here and nowhere else: every path that goes round
            ; again passes through, and the flags the branches above depend on
            ; come from the adc, which an iny would overwrite.
next$:      iny
            iny
            dec     zp:vx_step
            lbne    step$

            asl     zp:vx_stepx
            rol     zp:vx_stepx+1
            asl     zp:vx_stepy
            rol     zp:vx_stepy+1
            dec     zp:vx_band
            lbne    band$

            ; Whatever is left above vx_ybuf is sky, and voxel_render has
            ; already DMAd that in. Nothing to do.
done$:      rts
