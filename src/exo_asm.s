; Exomizer decruncher: a port of exomizer's reference decoder (exodec.c) for
; the -P0 (exomizer-2) stream format, reading and writing 32-bit addresses so
; it can unpack straight into attic RAM.
;
; Ported from the one in mega65/ozmoo-z6 (asm/pictures-mega65.asm), which is
; where this format and these flags are already proven. tools/convmap.py
; crunches with the matching flags: raw -q -C -P0 -c -m <window>.
;
; The crunched stream is read FORWARDS and the plaintext written forwards;
; back-references are read straight out of the output already written, so
; there is no ring buffer and no window limit beyond what the cruncher used.
;
;   exo_cr  reads the crunched stream forwards from staging
;   exo_src reads a back-reference from the plaintext already written
;   exo_out is the running output pointer
;
; All three are 32-bit zero page pointers for the 45GS02's [zp],z addressing.
; State and tables are declared in src/exo.c, which also sets the pointers up.

            .extern exo_cr, exo_src, exo_out
            .extern exo_bitbuf, exo_bits_lo, exo_bits_hi, exo_count
            .extern exo_len_lo, exo_len_hi
            .extern exo_a_lo, exo_a_hi, exo_b, exo_t_lo, exo_t_hi
            .extern exo_c_lo, exo_c_hi, exo_idx
            .extern exo_tabl_lo, exo_tabl_hi, exo_tabl_bi
            .extern exo_tabl_bit, exo_tabl_off

            .section code,text
            .public exo_decrunch_asm

; ---------------------------------------------------------------------------
; Write A to the output and step exo_out on. Three bytes of carry is enough:
; nothing here spans a megabyte.
exo_store:     ldz     #0
            sta     [exo_out],z
            inc     zp:exo_out
            bne     sdone$
            inc     zp:exo_out+1
            bne     sdone$
            inc     zp:exo_out+2
sdone$:     rts

; A = next crunched byte, read forwards from staging.
exo_getcr:     ldz     #0
            lda     [exo_cr],z
            inc     zp:exo_cr
            bne     gdone$
            inc     zp:exo_cr+1
            bne     gdone$
            inc     zp:exo_cr+2
gdone$:     rts

; Get X bits (0..16), MSB first, into A / exo_bits_lo / exo_bits_hi. Many
; table entries ask for zero bits, which must return 0 without touching the
; stream -- the loop is a do-while, so the zero case is guarded up front.
exo_getbits:   stx     exo_count
            lda     #0
            sta     exo_bits_lo
            sta     exo_bits_hi
            cpx     #0
            beq     gbret$
gbloop$:    lsr     exo_bitbuf      ; rot(0): carry = bit 0, 0 into the top
            lda     exo_bitbuf
            bne     gbshift$        ; buffer not empty: carry is the data bit
            jsr     exo_getcr          ; empty: refill and rot(1)
            lsr     a               ; carry = new byte's bit 0
            ora     #0x80           ; rot(1) sets the top bit; ora keeps carry
            sta     exo_bitbuf
gbshift$:   rol     exo_bits_lo     ; val = (val << 1) | carry
            rol     exo_bits_hi
            dec     exo_count
            bne     gbloop$
gbret$:     lda     exo_bits_lo
            rts

; exo_a += 1 << A, where A is the shift count 0..15.
exo_shiftadd:  tax
            lda     #1
            sta     exo_t_lo
            lda     #0
            sta     exo_t_hi
            cpx     #0
            beq     saadd$
sashift$:   asl     exo_t_lo
            rol     exo_t_hi
            dex
            bne     sashift$
saadd$:     clc
            lda     exo_a_lo
            adc     exo_t_lo
            sta     exo_a_lo
            lda     exo_a_hi
            adc     exo_t_hi
            sta     exo_a_hi
            rts

; Cooked code: base = tabl_lo/hi[X], result = base + get_bits(tabl_bi[X]),
; left in exo_c_lo/exo_c_hi.
exo_cooked:    lda     exo_tabl_lo,x
            sta     exo_c_lo
            lda     exo_tabl_hi,x
            sta     exo_c_hi
            lda     exo_tabl_bi,x
            tax
            jsr     exo_getbits
            clc
            adc     exo_c_lo
            sta     exo_c_lo
            lda     exo_bits_hi
            adc     exo_c_hi
            sta     exo_c_hi
            rts

; Seed the bit buffer and build the 52-entry decode table (exodec.c
; table_init): a = 1 at each 16-entry boundary, else a += 1 << b, where b is
; the previous entry's 4-bit field.
exo_init:      jsr     exo_getcr
            sta     exo_bitbuf
            lda     #0
            sta     exo_a_lo
            sta     exo_a_hi
            sta     exo_b
            ldy     #0
eiloop$:    tya
            and     #0x0f
            bne     eiadd$
            lda     #1              ; boundary: a = 1
            sta     exo_a_lo
            lda     #0
            sta     exo_a_hi
            bra     eistore$
eiadd$:     lda     exo_b           ; a += 1 << b
            jsr     exo_shiftadd
eistore$:   lda     exo_a_lo
            sta     exo_tabl_lo,y
            lda     exo_a_hi
            sta     exo_tabl_hi,y
            ldx     #4              ; b = get_bits(4)
            jsr     exo_getbits
            sta     exo_b
            sta     exo_tabl_bi,y
            iny
            cpy     #52
            bne     eiloop$
            rts

; ---------------------------------------------------------------------------
exo_decrunch_asm:
            jsr     exo_init
loop$:      ldx     #1              ; one flag bit: 1 = literal, 0 = sequence
            jsr     exo_getbits
            beq     seq$
            jsr     exo_getcr          ; literal byte, straight to the output
            jsr     exo_store
            bra     loop$

seq$:       ldy     #0              ; gamma: count leading zero bits into Y
gamma$:     ldx     #1
            jsr     exo_getbits
            bne     havegamma$
            iny
            bra     gamma$
havegamma$: cpy     #16             ; gamma 16 is the end-of-stream marker
            bne     notend$
            jmp     done$
notend$:    tya                     ; length = cooked(gamma)
            tax
            jsr     exo_cooked
            lda     exo_c_lo
            sta     exo_len_lo
            lda     exo_c_hi
            sta     exo_len_hi

            ; i = min(length, 3) - 1: which of the three offset tables to use.
            lda     exo_len_hi
            bne     i2$
            lda     exo_len_lo
            cmp     #3
            bcs     i2$
            sec                     ; length is 1 or 2: i = length - 1
            sbc     #1
            tax
            bra     havei$
i2$:        ldx     #2
havei$:     stx     exo_idx         ; keep i across the get_bits below
            lda     exo_tabl_bit,x
            tax
            jsr     exo_getbits        ; v2 = tabl_off[i] + get_bits(tabl_bit[i])
            ldx     exo_idx
            clc
            adc     exo_tabl_off,x
            tax
            jsr     exo_cooked         ; offset = cooked(v2)

            ; src = out - offset, a 32-bit subtract.
            sec
            lda     zp:exo_out
            sbc     exo_c_lo
            sta     zp:exo_src
            lda     zp:exo_out+1
            sbc     exo_c_hi
            sta     zp:exo_src+1
            lda     zp:exo_out+2
            sbc     #0
            sta     zp:exo_src+2
            lda     zp:exo_out+3
            sbc     #0
            sta     zp:exo_src+3

copy$:      ldz     #0              ; copy length bytes forward, src -> output
            lda     [exo_src],z
            jsr     exo_store
            inc     zp:exo_src
            bne     cnw$
            inc     zp:exo_src+1
            bne     cnw$
            inc     zp:exo_src+2
cnw$:       lda     exo_len_lo      ; length -= 1
            bne     cdec$
            dec     exo_len_hi
cdec$:      dec     exo_len_lo
            lda     exo_len_lo
            ora     exo_len_hi
            bne     copy$
            jmp     loop$           ; too far for a relative branch back

done$:      rts
