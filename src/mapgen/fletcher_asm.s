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
            .extern nz_top, nz_lead, nz_sum_a, nz_sum_b

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
