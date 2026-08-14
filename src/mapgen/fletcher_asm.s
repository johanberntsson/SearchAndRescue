; One row of a field's Fletcher checksum.
;
; **This is verification scaffolding and it was the most expensive thing in
; stage one.** Seven passes check their work by summing the whole 512x512
; field, and in C that is 262144 iterations of
;
;     a += v; b += a;
;
; at something like three hundred cycles each, because two 16-bit adds with a
; carry between them is a dozen instructions and Calypsi reaches the buffer
; through the software stack. Here it is a zero-page pointer and about thirty.
;
; The row is already in chip RAM: the caller DMAs it down out of attic RAM,
; which costs 16 cycles a byte and is the one part of this that cannot be made
; cheaper. Reading the field directly with far loads -- which `lakes_fill` did
; -- is a quarter of a million of them at +15 cycles apiece over chip RAM.
;
; Z is only eight bits, so the row is walked in four chunks of 128 entries with
; the pointer stepped a page at a time between them. INZ leaves the carry
; alone, which is what lets the two halves of each add chain across it.

            .rtmodel version, "1"
            .rtmodel core, "*"

; **It borrows the store pass's zero page.** nz_top is the row, nz_lead walks
; it, and nz_sum_a/nz_sum_b are the two accumulators -- all four are free
; between passes, and zero page had four bytes left, not twelve.
            .extern nz_top, nz_bot, nz_lead, nz_trail
            .extern nz_sum_a, nz_sum_b, nz_t, nz_floor

CHUNKS:     .equ 4                    ; 4 pages of 128 entries is 512

            .section code, text
            .public fl_row

fl_row:
            lda     zp:nz_top
            sta     zp:nz_lead
            lda     zp:nz_top+1
            sta     zp:nz_lead+1
            ldx     #CHUNKS

chunk$:
            ldz     #0
elem$:
            clc
            lda     (zp:nz_lead),z
            adc     zp:nz_sum_a
            sta     zp:nz_sum_a
            inz
            lda     (zp:nz_lead),z
            adc     zp:nz_sum_a+1
            sta     zp:nz_sum_a+1
            clc
            lda     zp:nz_sum_a
            adc     zp:nz_sum_b
            sta     zp:nz_sum_b
            lda     zp:nz_sum_a+1
            adc     zp:nz_sum_b+1
            sta     zp:nz_sum_b+1
            inz
            bne     elem$

            inc     zp:nz_lead+1
            dex
            bne     chunk$

            ldz     #0                ; Calypsi keeps its pointer index in Z
            rts

; --- the rivers' two row scans ----------------------------------------------
;
; `rivers_carve` reads the whole field three times before a single river is
; walked -- once for the low and high of the terrain, once to count the dry
; cells above the cut, and once to find the three that were picked. The walk
; itself is a few thousand steps and costs nothing; those three scans were most
; of the pass.
;
; Both routines here take their row in nz_top (and the level row in nz_bot),
; walk it with nz_lead and nz_trail, and leave their answer in nz_sum_a and
; nz_sum_b -- the same four slots the checksum borrows, and free for the same
; reason.

            .public fl_minmax, fl_count

; nz_sum_a = min(nz_sum_a, row), nz_sum_b = max(nz_sum_b, row).
fl_minmax:
            lda     zp:nz_top
            sta     zp:nz_lead
            lda     zp:nz_top+1
            sta     zp:nz_lead+1
            ldx     #CHUNKS
mchunk$:
            ldz     #0
melem$:
            lda     (zp:nz_lead),z
            sta     zp:nz_t
            inz
            lda     (zp:nz_lead),z
            sta     zp:nz_t+1
            dez

            lda     zp:nz_t
            cmp     zp:nz_sum_a
            lda     zp:nz_t+1
            sbc     zp:nz_sum_a+1
            bcs     mhi$
            lda     zp:nz_t
            sta     zp:nz_sum_a
            lda     zp:nz_t+1
            sta     zp:nz_sum_a+1
mhi$:
            lda     zp:nz_sum_b
            cmp     zp:nz_t
            lda     zp:nz_sum_b+1
            sbc     zp:nz_t+1
            bcs     mnext$
            lda     zp:nz_t
            sta     zp:nz_sum_b
            lda     zp:nz_t+1
            sta     zp:nz_sum_b+1
mnext$:
            inz
            inz
            bne     melem$
            inc     zp:nz_lead+1
            dex
            bne     mchunk$
            ldz     #0
            rts

; nz_sum_a = how many cells of this row are dry and stand above nz_floor.
;
; Dry is 0xFFFF, so the two bytes ANDed together are 0xFF and nothing else is
; -- one test instead of two comparisons. The height test is `cut < h`, done as
; a subtraction in that order so the borrow is the answer.
fl_count:
            lda     zp:nz_top
            sta     zp:nz_lead
            lda     zp:nz_top+1
            sta     zp:nz_lead+1
            lda     zp:nz_bot
            sta     zp:nz_trail
            lda     zp:nz_bot+1
            sta     zp:nz_trail+1
            lda     #0
            sta     zp:nz_sum_a
            sta     zp:nz_sum_a+1
            ldx     #CHUNKS
cchunk$:
            ldz     #0
celem$:
            lda     (zp:nz_trail),z
            inz
            and     (zp:nz_trail),z
            dez
            cmp     #0xff
            bne     cnext$

            lda     zp:nz_floor
            cmp     (zp:nz_lead),z
            inz
            lda     zp:nz_floor+1
            sbc     (zp:nz_lead),z
            dez
            bcs     cnext$

            inc     zp:nz_sum_a
            bne     cnext$
            inc     zp:nz_sum_a+1
cnext$:
            inz
            inz
            bne     celem$
            inc     zp:nz_lead+1
            inc     zp:nz_trail+1
            dex
            bne     cchunk$
            ldz     #0
            rts
