; Memory benchmarks, in assembly.
;
; These exist to compare chip RAM against attic RAM, and the C compiler will
; not give a fair comparison: it cross-calls loop bodies into shared fragments
; and the two versions come out as different instruction sequences. Measured
; that way the attic RAM read appeared *faster* than the chip RAM one, which
; is nonsense. Here each routine is one loop, and chip and attic differ only
; in the pointer the caller sets up.
;
; Every loop carries the same 16-bit counter tail, so bench_empty subtracts
; out exactly rather than approximately.
;
; Parameters live in zero page and are set up by profile_bench() in profile.c.

            .extern bn_ptr, bn_px, bn_py, bn_n, bn_sink

; The steps a ray march walks the map with, in 8.8 cells. Deliberately not a
; whole number of cells in either axis: consecutive samples must land in
; different cache lines, or this measures the cache and not the memory.
WALK_X:     .equ 0x0187
WALK_Y:     .equ 0x00c5

            .section code,text
            .public bench_empty, bench_read_walk, bench_read_seq
            .public bench_write_span
            .public profile_irq_off, profile_irq_on

; Nothing may interrupt a measurement.
;
; The calibration times SIXTEEN RASTER LINES -- about a millisecond -- and an
; interrupt landing inside that window scales every figure the profiler prints
; for the rest of the run, including the frame rate on the panel. With the
; title music playing at 50 Hz there is a real chance of it on any given boot,
; and the ROM's own handler was always a smaller version of the same risk.
; Cheap to rule out: the profiler owns CIA2 rather than any interrupt, so it
; needs nothing from one.
profile_irq_off:
            sei
            rts

profile_irq_on:
            cli
            rts

; Loop overhead alone.
bench_empty:
empty$:
            lda     zp:bn_n
            bne     ehi$
            dec     zp:bn_n+1
ehi$:       dec     zp:bn_n
            lda     zp:bn_n
            ora     zp:bn_n+1
            bne     empty$
            rts

; A heightmap sample the way voxel_asm.s takes one: step the 8.8 position,
; drop the two high coordinate bytes into the pointer, read the cell.
bench_read_walk:
walk$:
            clc
            lda     zp:bn_px
            adc     #(WALK_X & 0xff)
            sta     zp:bn_px
            lda     zp:bn_px+1
            adc     #(WALK_X >> 8)
            sta     zp:bn_px+1
            clc
            lda     zp:bn_py
            adc     #(WALK_Y & 0xff)
            sta     zp:bn_py
            lda     zp:bn_py+1
            adc     #(WALK_Y >> 8)
            sta     zp:bn_py+1

            lda     zp:bn_px+1
            sta     zp:bn_ptr
            lda     zp:bn_py+1
            sta     zp:bn_ptr+1
            ldz     #0
            lda     [bn_ptr],z
            sta     zp:bn_sink

            lda     zp:bn_n
            bne     whi$
            dec     zp:bn_n+1
whi$:       dec     zp:bn_n
            lda     zp:bn_n
            ora     zp:bn_n+1
            bne     walk$
            rts

; The same read, walking straight up memory instead. The gap between this and
; bench_read_walk is what the attic RAM cache line is worth.
bench_read_seq:
seq$:
            ldz     #0
            lda     [bn_ptr],z
            sta     zp:bn_sink
            inc     zp:bn_ptr
            bne     snw$
            inc     zp:bn_ptr+1
snw$:
            lda     zp:bn_n
            bne     shi$
            dec     zp:bn_n+1
shi$:       dec     zp:bn_n
            lda     zp:bn_n
            ora     zp:bn_n+1
            bne     seq$
            rts

; A terrain span pixel: byte write at a stride of 8, exactly the fill loop in
; voxel_asm.s. This is the figure that decides whether a back buffer could
; live in attic RAM and be DMAd back.
bench_write_span:
span$:
            ldz     #0
            lda     zp:bn_sink
            sta     [bn_ptr],z
            clc
            lda     zp:bn_ptr
            adc     #8
            sta     zp:bn_ptr
            bcc     pnw$
            inc     zp:bn_ptr+1
pnw$:
            lda     zp:bn_n
            bne     phi$
            dec     zp:bn_n+1
phi$:       dec     zp:bn_n
            lda     zp:bn_n
            ora     zp:bn_n+1
            bne     span$
            rts
