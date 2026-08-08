; The renderer's inner loop, in 45GS02 assembly.
;
; The C version of this measured 1392 cycles per heightmap sample: the compiler
; builds the loop out of jsr fragments and keeps locals on the software stack,
; so a handful of adds and one memory read turn into hundreds of cycles. This
; keeps everything in zero page, samples the maps with a single 32-bit indirect
; load, and drives the hardware multiplier directly.
;
; Parameters live in zero page and are set up by voxel_column() in voxel.c.

            .extern vx_hptr, vx_cptr, vx_fb, vx_fbbase
            .extern vx_px, vx_py, vx_stepx, vx_stepy
            .extern vx_camh, vx_horizon, vx_ybuf, vx_ys, vx_tmp
            .extern vx_bands, vx_bandsteps, vx_band, vx_step
            .extern vx_inv_z, vx_sky

MULTINA:    .equlab 0xd770
MULTINB:    .equlab 0xd774
MULTOUT:    .equlab 0xd778

            .section code,text
            .public voxel_column_asm

voxel_column_asm:
            ; Y indexes inv_z for the whole march. Nothing else in the step or
            ; span loops touches it, and the sky fill reloads it afterwards.
            ldy     #0
            lda     zp:vx_bands
            sta     zp:vx_band

band$:      lda     zp:vx_bandsteps
            sta     zp:vx_step

step$:      ; px += stepx, py += stepy. Two 16-bit adds rather than one 32-bit
            ; one, because a carry out of px must not reach py: px and py wrap
            ; independently, which is what wraps the map.
            clc
            lda     zp:vx_px
            adc     zp:vx_stepx
            sta     zp:vx_px
            lda     zp:vx_px+1
            adc     zp:vx_stepx+1
            sta     zp:vx_px+1
            clc
            lda     zp:vx_py
            adc     zp:vx_stepy
            sta     zp:vx_py
            lda     zp:vx_py+1
            adc     zp:vx_stepy+1
            sta     zp:vx_py+1

            ; The map is 256x256 at a 64K boundary, so the cell address is just
            ; the two high bytes dropped into the low half of the pointer.
            lda     zp:vx_px+1
            sta     zp:vx_hptr
            lda     zp:vx_py+1
            sta     zp:vx_hptr+1
            ldz     #0
            lda     [vx_hptr],z
            sta     zp:vx_tmp

            ; dh = camh - height
            sec
            lda     zp:vx_camh
            sbc     zp:vx_tmp
            sta     MULTINA
            lda     zp:vx_camh+1
            sbc     #0
            sta     MULTINA+1
            ; sign extend into the top half
            bmi     neg$
            lda     #0x00
            bra     sign$
neg$:       lda     #0xff
sign$:      sta     MULTINA+2
            sta     MULTINA+3

            ; MULTINB = inv_z[k]. Its top half is zero for the whole run and is
            ; set once in voxel_init.
            lda     vx_inv_z,y
            sta     MULTINB
            iny
            lda     vx_inv_z,y
            sta     MULTINB+1
            iny

            ; ys = horizon + (product >> 8): bytes 1 and 2 are the shift.
            clc
            lda     zp:vx_horizon
            adc     MULTOUT+1
            sta     zp:vx_ys
            lda     zp:vx_horizon+1
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

draw$:      ; Same cell in the colour map: only the bank byte differs, and the
            ; C side has already set it up.
            lda     zp:vx_hptr
            sta     zp:vx_cptr
            lda     zp:vx_hptr+1
            sta     zp:vx_cptr+1
            ldz     #0
            lda     [vx_cptr],z
            sta     zp:vx_tmp       ; colour

            ; fb = fbbase + ys * 8
            lda     zp:vx_ys
            asl     a
            sta     zp:vx_fb
            lda     #0
            rol     a
            sta     zp:vx_fb+1
            asl     zp:vx_fb
            rol     zp:vx_fb+1
            asl     zp:vx_fb
            rol     zp:vx_fb+1
            clc
            lda     zp:vx_fb
            adc     zp:vx_fbbase
            sta     zp:vx_fb
            lda     zp:vx_fb+1
            adc     zp:vx_fbbase+1
            sta     zp:vx_fb+1
            lda     zp:vx_fbbase+2
            adc     #0
            sta     zp:vx_fb+2
            lda     zp:vx_fbbase+3
            adc     #0
            sta     zp:vx_fb+3

            ; Fill down to whatever was already drawn, one pixel every 8 bytes.
            sec
            lda     zp:vx_ybuf
            sbc     zp:vx_ys
            tax
            ldz     #0
fill$:      lda     zp:vx_tmp
            sta     [vx_fb],z
            clc
            lda     zp:vx_fb
            adc     #8
            sta     zp:vx_fb
            bcc     nowrap$
            inc     zp:vx_fb+1
nowrap$:    dex
            bne     fill$

            lda     zp:vx_ys
            sta     zp:vx_ybuf
            beq     sky$            ; column is full

next$:      dec     zp:vx_step
            lbne    step$

            asl     zp:vx_stepx
            rol     zp:vx_stepx+1
            asl     zp:vx_stepy
            rol     zp:vx_stepy+1
            dec     zp:vx_band
            lbne    band$

sky$:       ldx     zp:vx_ybuf
            beq     done$
            lda     zp:vx_fbbase
            sta     zp:vx_fb
            lda     zp:vx_fbbase+1
            sta     zp:vx_fb+1
            lda     zp:vx_fbbase+2
            sta     zp:vx_fb+2
            lda     zp:vx_fbbase+3
            sta     zp:vx_fb+3
            ldy     #0
            ldz     #0
skyloop$:   lda     vx_sky,y
            sta     [vx_fb],z
            clc
            lda     zp:vx_fb
            adc     #8
            sta     zp:vx_fb
            bcc     skynowrap$
            inc     zp:vx_fb+1
skynowrap$: iny
            dex
            bne     skyloop$

done$:      rts
