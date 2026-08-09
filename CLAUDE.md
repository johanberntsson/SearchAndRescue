# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

A MEGA65 heightfield voxel flight simulator / drone search-and-rescue game, written in C (Calypsi) with the rendering inner loop in 45GS02 assembly. `documentation/vision.md` holds the full technical and gameplay design; `todo.txt` is the authoritative "what's next" and should be updated as work lands.

Currently: a flyable voxel engine at about 10 fps, with an FPS readout as the first piece of the HUD. No game yet.

## Build and run

```sh
make run          # build build/sar.d81 and boot it in xemu
make prg          # skip the disk, run the PRG directly (no resources available)
make PROFILE=0    # without the per-column instrumentation
make clean
```

`make run` launches `xemu-xmega65`, a GUI emulator that blocks until closed. For automated checks, run it headless and screenshot on exit:

```sh
timeout -s INT 100 xemu-xmega65 -besure -headless -sleepless \
    -8 build/sar.d81 -screenshot out.png -dumpmem mem.bin
python3 tools/profread.py mem.bin
```

`-sleepless` is fine here — see Performance for why it must never be used to time anything from the outside.

`-dumpmem` writes 384 KB of chip RAM, so `mem.bin[0x10000]` onwards is the framebuffer and `mem.bin[0x40000]` the heightmap. Reading a framebuffer back and de-swizzling it with the column-strip formula below is the fastest way to tell "the renderer is wrong" apart from "the display is wrong"; both have happened. There are no tests and no linter.

## Toolchain

Calypsi 6502 C compiler 5.18, installed system-wide (`/usr/local/bin/cc6502`, `ln6502`; headers, libraries and linker rules under `/usr/local/lib/calypsi-6502-5.18/`). `--target=mega65` selects the 45GS02 core, puts the MEGA65 SDK headers on the include path (`<mega65.h>` gives `VICIV`, `PALETTE`, `CIA1`, `MATH` …) and links the board support library.

**`ln6502 -o` names the ELF output, not the PRG.** The PRG is written alongside it under the same stem, so the Makefile links to `build/sar.elf` and copies the resulting `build/sar.prg` to `build/autoboot.c65`. Linking straight to `autoboot.c65` silently produces an ELF file that the MEGA65 tries to RUN as BASIC.

`mega65-plain.scm` resolves from the toolchain's `linker-rules/` directory, not this repo. It gives the program `$2001-$9FFF` — 32 KB for code, data, stack and heap — and emits a C65 BASIC stub (`SYS 8206`), which is exactly what `autoboot.c65` needs.

Data above 64 K is reached with Calypsi's `__far` pointers. Their index type is `int16_t`, which cannot span a whole 64 K map; `src/voxel.c` biases the base pointer by 32 K and XORs the offset (`MAP_BIAS`) so a signed index covers the map.

## Memory map

Only banks 1, 4 and 5 are free: `$20000-$3FFFF` holds the C65 ROM, and **colour RAM is aliased into chip RAM at `$1F800`**, so bank 1 effectively ends there.

| Address | Size | Contents |
|---|---|---|
| `$2001-$9FFF` | 32 KB | Program, data, screen tables, load bounce buffer |
| `$10000-$15EFF` | 24320 | Framebuffer A (160x152, the 3D view) |
| `$16000-$1BEFF` | 24320 | Framebuffer B |
| `$1C000` | 1216 | One column strip of sky, DMAd across the buffer each frame |

`make WIDE=1` renders all 320 pixels instead of stretching 160. That needs
48640 bytes a buffer, so B moves to `$50000` and the colourmap moves out to
attic RAM at `$8000000` — the only rearrangement that fits, and the reason
the attic figures were measured. It costs about half the frame rate:
| `$1F800-$1FFFF` | 2 KB | Colour RAM alias — **do not write** |
| `$40000-$4FFFF` | 64 KB | Heightmap, only when it is 256x256 |
| `$8000000-$80FFFFF` | 1 MB | Colourmap planes, attic RAM |
| `$8100000-$81FFFFF` | 1 MB | Heightmap planes, when larger than 256x256 |
| `$8200000-` | | Staging for the crunched stream being unpacked |

Bank 5 is free at 160 wide (`make WIDE=1` puts the second framebuffer there),
and bank 4 is free too whenever the heightmap is above 256x256.

**Both maps ship as 256x256 planes, one per sub-cell corner** — a map of SIZE
is `(SIZE/256)^2` of them — rather than as one big array. Every plane is then
on a 64K boundary, so a cell is still addressed by dropping the two high
coordinate bytes into a pointer and the sub-cell picks the *bank byte*. One
big array would need a shift and an OR on every read. `HGT_SIZE` and
`COL_SIZE` are build knobs (see Performance); the plane number comes from a
256-byte lookup on each position fraction, so it costs one `lda abs,x`
whether there are four planes or sixteen.

Writing past `$1F800` does not fault: it fills colour RAM with pixel data, which the VIC-IV reads back as character attributes and which blanks cells all over the display. Bank 1 has room to spare at 160 wide; at 320 it will not, and the colourmap moves to attic RAM (see Performance).

Map coordinates are 8.8 fixed point in a `uint16_t`, so the high byte is the cell index and movement wraps the 256x256 map for free.

## Full-colour display

There is no linear bitmap. The screen is a grid of 8x8 characters, each 64 bytes of 8-bit palette indices, and screen RAM holds a 16-bit character *number* whose data lives at `charnum * 64` — an absolute address, not offset by CHARPTR.

Characters are laid out in **column strips** rather than rows (`screen[row][col] = base/64 + col*FB_ROWS + row`), which makes a vertical span a single pointer stepping by 8 for its whole length:

```
address(x, y) = base + (x >> 3) * FB_STRIDE + (x & 7) + y * 8
```

The renderer relies on this everywhere. Double-buffer flips are just a rewrite of `SCRNPTR` between two prepared screen tables — no pixels move.

The layout also makes the sky nearly free. Every column strip's sky is the *same* `FB_STRIDE` bytes, so `voxel_init` prepares one strip at `FB_SKY` and `voxel_render` DMAs it across the buffer — `FB_COLS` copy jobs, 2.5 ms for the whole screen against about 8 ms when the inner loop drew it a pixel at a time. That prefill is also the frame's clear pass, so the terrain spans just draw over it.

Two register notes: `CHRXSCL` (`$D05A`) is source pixels per output pixel in 120ths, so **60 doubles the width** and 240 halves it; and the hot registers (`$D05D` bit 7) must be turned off first or any write to a legacy VIC-II register makes the VIC-IV recompute the layout and undo the setup.

## The panel

The display is 25 character rows; the framebuffer covers the top 19 and the
bottom six are the information panel. That split is free, because full colour
is per character *number*: `FCLRHI` is set and `FCLRLO` is not, so numbers
above `$FF` are 64-byte full-colour characters (the framebuffer) and numbers
below are ordinary 8x8 text from `CHARPTR`. The panel costs 240 bytes of
screen RAM, no pixel writes, and is not double buffered — `vic4_panel_char`
writes the same cell in both screen tables.

Two things had to be set up for it. `CHARPTR` had never been touched, and
whatever the ROM leaves there draws a horizontal line for a space; the C65
ROM's 8x8 set at `$2D000` is readable where it sits, so the panel needs no
RAM for a font. And **a text character's colour comes from colour RAM, which
is only a four-bit field** even in 16-bit character mode — so panel ink has
to be one of the first sixteen palette entries, which is where the terrain
colours live. `tools/convmap.py` reserves 1 and 2 by moving the terrain
colours that were there out to unused slots; palette 0 is the screen colour
and serves as the paper. Palette entries 240 and 241 stay reserved for a
future pixel-drawn overlay over the 3D view, where a byte per pixel means any
of the 256 entries will do.

## Resources

`tools/convmap.py` turns the 1024x1024 VoxelSpace PNGs in `resources/` into raw `terrain.hgt`, `terrain.col` and `terrain.pal`. The sources need no quantisation: the heightmap is 8-bit greyscale and the colourmap is already a palette image. Palette bytes are nybble-swapped for the `$D100`/`$D200`/`$D300` registers, and the sky gradient goes in indices 224-239, which the colourmap never uses.

`convmap.py` takes the two map sizes as arguments; the Makefile passes
`HGT_SIZE` and `COL_SIZE`.

Height units are a quarter of a map cell — the maps were downsampled 4x and the heights were not rescaled — and `SCALE_H` in `src/voxel.c` folds that in.

The maps are **exomizer-crunched**, which is what lets any resolution above
256 fit a d81. `src/exo_asm.s` is a port of the decruncher in
mega65/ozmoo-z6 (`asm/pictures-mega65.asm`) for the `-P0` stream format,
reading and writing 32-bit addresses so it unpacks straight into attic RAM;
its state lives in `src/exo.c` the way the renderer's `vx_*` block does.
Back-references are read out of the plaintext already written, so there is no
window buffer and `-m` can be as large as the cruncher will take. Each file
carries its own crunched length up front, because EOF cannot be trusted (see
below). `convmap.py` finds exomizer via `$EXOMIZER`, `tools/exomizer`, an
ozmoo-z6 checkout, or `PATH`.

**Reading a SEQ file through the Kernal reports EOF exactly 256 bytes early**, whatever the file's size (verified from 16 K to 64 K, at several chunk sizes). `convmap.py` therefore pads every resource by 512 bytes and `src/loader.c` reads a known length rather than looking for the end of the file. Resources go on the disk as SEQ, not PRG: Calypsi's `_Stub_open` calls Kernal OPEN with the file descriptor as the secondary address, which defaults to SEQ, and SEQ has no load-address header to skip.

`tools/diskutil.rb` (from Fredrik Ramsberg) builds the D81 and refuses to overwrite a file that already exists on the image, so the Makefile deletes and rebuilds the image every time. The name on disk comes from the host file's basename.

## Performance

Roughly 15.5 fps, from 0.74 when the renderer was all C. `src/profile.c` measures
it; `tools/profread.py` formats the results out of a `-dumpmem` image. The FPS
readout in the corner is always on. `make PROFILE=0` compiles out the
per-column instrumentation and the counters in `voxel_asm.s`, which the
Makefile guards with the same flag passed to the assembler; the FPS counter
stays. **That instrumentation costs about 7% of a frame**, so the readout in
the corner of a default build reads low — 10.5 against the 11.3 a `PROFILE=0`
build actually runs at. Quote speed from a `PROFILE=0` run. Switching
`PROFILE` touches a stamp file that forces a rebuild, because otherwise half
the objects disagree with the flag and the counters silently stay off.

The counters are totalled per frame rather than per column: `profile_count`
adds into a 32-bit field, and calling it four times a column instead of four
times a frame cost 9% of the frame on its own.

**`profile_calibrate` must start on a raster line boundary.** Timing sixteen
lines from wherever the raster happens to be loses up to a whole line — 6% —
and that scales every figure `profread` prints. Two runs of identical code
came out 67.0 ms and 64.9 ms from this alone, which is wide enough to invent
or hide a small optimisation. It now waits for a line change before taking
the first timestamp, and repeat runs agree exactly.

**Never time this with `xemu -sleepless`.** It runs the emulator around 19x
faster than a real MEGA65, so wall-clock frame counts measure the host, not the
machine — that mistake put an early reading out by the same factor. The
profiler's clock is calibrated against the raster inside the emulator, so it is
right either way, and a plain `-headless` run agrees with it.

What the measurements found, in the order they mattered:

| | cycles | note |
|---|---|---|
| C 32-bit multiply | 2203 | one per heightmap sample, 64% of the frame |
| the same on the hardware multiplier | 85 | `$D770`, see `mul_shift8` history |
| C 16-bit multiply | 657 | still a library call |
| a scattered map sample | 72 | pointer setup and `lda [ptr],z` |
| one span pixel | 29 | byte write at a stride of 8 |
| DMA copy | 2.6 /byte | fill is 1.6 |

**The memory benchmarks live in `src/bench_asm.s`, not in C, and must stay
there.** They exist to compare chip RAM against attic RAM, and the compiler
cross-calls loop bodies into shared fragments: written in C the two versions
come out as different instruction sequences, and the attic read measured
*faster* than the chip read. In assembly the two differ only by the pointer
the caller sets up, so the ratio means something. Each loop carries the same
counter tail as `bench_empty` so the baseline subtracts exactly — the two
groups also need separate baselines, since the compiler's loop overhead is
not the assembly loop's and mixing them produced negative readings.

**xemu does not model the attic RAM bus**, and reports it at chip RAM speed.
Measured on a real MEGA65 (chip / attic, cycles per operation):

| | chip | attic | |
|---|---|---|---|
| map sample, scattered | 70 | 86 | +16 |
| map read, sequential | 26 | 41 | +15 |
| span pixel, one byte write | 30 | 33 | **+3** |
| DMA copy chip to chip | 2.45 /byte | | |
| DMA copy attic to chip | | 17.80 /byte | **7.3x** |
| DMA fill | 1.22 /byte | | |

So a CPU access to attic RAM costs a flat ~15 cycles more than chip RAM,
whether it is scattered or sequential — the 8-byte cache line buys nothing
worth planning around. **Writes are posted and nearly free at +3.** And the
DMA is the opposite of what everyone assumes: attic to chip runs at 2.3 MB/s
against 16.5 MB/s chip to chip, which rules out any per-frame bulk move out
of attic RAM. Keeping the sky template in chip RAM rather than attic is worth
11 ms a frame on its own.

Both `profread` and the on-screen report print an addressing check
(`$11223344`) alongside these — note that it needs `volatile` far pointers,
or the compiler reorders the writes and reads past each other and the check
fails on correct hardware.

The same run had the real machine at 11.0-11.2 fps against xemu's 11.6, so
xemu's chip RAM timing is about 4% optimistic and can be trusted for
everything that stays in chip RAM.

Real hardware has no `-dumpmem`, so `profile_report` prints the same memory
table to the Kernal's text screen at startup and waits for a key (or 20
seconds, so unattended runs still get on with rendering). It has to run
*before* `vic4_init`, which takes the text screen away. The figures come out
identical to `profread`'s, so either route can be trusted.

Calypsi 5.18 emits a call to `_FillZPQ` — a runtime helper that is in none of
its libraries — when a function call appears inside a 32-bit expression. The
link fails with an undefined symbol. Hoist the call into a variable.

Three things that sound like optimisations and measured slower:
`--no-cross-call` and `--strong-inline` (615 ms a frame against 533 ms), and
using the 45GS02's 32-bit `ADCQ` to step the ray position in one instruction
instead of two 16-bit adds (68.5 ms against 64.7).

**The Q pseudo-register *is* A/X/Y/Z**, which is why: the march keeps its step
index in Y across the whole column, so any Q operation destroys it. Moving
the index to zero page costs a reload plus two `inc`s instead of two `iny`s,
about 9 cycles a sample, and that is more than the Q form saves. The first
attempt at it did not move the index and appeared to run 30% faster — it was
reading garbage from `vx_inv_z` and filling columns early, which the sample
counter showed at once (5612 samples a frame instead of 10240). ADCQ would
also leak a carry from px into py, which is not the once-a-column rounding it
first looks like: with a negative step the carry fires on nearly every step,
drifting y by about a quarter of a cell down the march.

The inner loop is `src/voxel_asm.s`. The C version of the same loop cost 1392
cycles per sample, because the compiler builds it out of `jsr` fragments and
keeps locals on the software stack. The assembly keeps everything in zero page
(the `vx_*` block declared in `voxel.c`), samples the maps with one `lda
[ptr],z` — the maps sit on 64K boundaries so the cell address is just the two
high coordinate bytes dropped into the pointer — and drives the multiplier
directly. Both maps share one address computation because only their bank byte
differs.

Where a 64.7 ms frame goes, measured by stubbing pieces out rather than
guessing (setting `voxel_column_asm` to an immediate `rts` isolates the
per-column C setup; halving `BANDS` separates per-sample from per-column):

| | ms | |
|---|---|---|
| ray march | 46.0 | 10240 samples at **182 cycles** each |
| span fill | 10.0 | 14489 pixels at ~28 |
| span prologue | 3.4 | 3983 spans: the colour read and the length |
| per-column C setup | 2.4 | 607 cycles a column |
| sky DMA | 2.1 | |

Every column takes all 64 samples — none terminates early, because terrain
never fills a column to the top of the screen — so the march is flat in the
scene and scales only with column count. That is the number that governs
whether 320 wide is affordable.

Three things got the march from 302 cycles a sample to 182, all of them
verified by the frame coming out **pixel-identical** (`C_SPAN` and
`C_SPANPIX` unchanged is the cheap check; comparing screenshots of the 3D
view is the real one):

- the write pointer is carried between spans. It always addresses row
  `vx_ybuf`, and a span fills *upwards* from there, so it lands on row
  `vx_ys` — which is the next span's `vx_ybuf`. The whole `fbbase + ys * 8`
  calculation, about 70 cycles a span, simply disappears.
- the ray position's whole-cell bytes live *in* the map pointer. The maps are
  256x256 on a 64K boundary, so those bytes already are the cell address, and
  adding into them in place saves copying them across every sample.
- the height difference is biased positive by `CAM_BIAS`, which drops a
  branch and two register writes per sample. 256 is the one bias that comes
  back out free and exact: `(256 * inv_z) >> 8` is `inv_z`, so the correction
  is a subtraction folded into a per-step horizon table, and the low eight
  bits it discards are zero.

Map resolution is two build knobs, `HGT_SIZE` and `COL_SIZE`, powers of two
from 256 up to the source PNGs' 1024. Measured at 160 wide:

| | disk | frame | |
|---|---|---|---|
| h256 c512 | 169 KB | 64.9 ms, 15.4 fps | |
| h256 c1024 | 575 KB | 64.9 ms, 15.4 fps | colour is **free** |
| **h512 c1024** | **661 KB** | **70.6 ms, 14.2 fps** | the default |
| h1024 c512 | 608 KB | 70.6 ms, 14.2 fps | |
| h1024 c1024 | 1013 KB | — | will not fit a d81 |

**Colourmap resolution is free and heightmap resolution is not.** The colour
is read once per span (~4000 a frame) and the plane lookup is the same work
at any size, so 1024x1024 costs exactly what 512x512 does. The heightmap is
read once per *sample* (10240 a frame) and above 256x256 cannot stay in chip
RAM, so it pays an attic read plus a plane lookup every time: a flat 5.7 ms,
8.8%, the same whether it is 512 or 1024. Which is worth knowing — **h1024
costs no more at runtime than h512**, it just does not leave room for c1024
on one disk.

| | 256x256 colour | 512x512 colour |
|---|---|---|
| 320 wide | 124.8 ms, 8.0 fps | 133.1 ms, 7.5 fps |

Going 320 wide doubles the sample count exactly and costs half the frame
rate, and **the picture barely changes** — the blockiness was the map showing
through, not the pixel grid, so the screen was never the limiting resolution.
Doubling the *colourmap* instead costs 3.6% and is plainly visible. That is
the trade this engine responds to: detail per cycle lives in the map, not in
the raster. The panel going from 20 to 40 columns is the real argument for
320 wide, not the terrain.

Draw distance and cost are traded in the band schedule at the top of
`voxel.c`: `BANDS` x `BAND_STEPS` samples per column, with the step doubling
each band. 4x16 reaches 120 map cells for 64 samples.

Beware `iny` between an `adc` and the branch that tests its result — it
overwrites the flags, and the counters caught it as 160 full-height spans a
frame. `next$` is the one place the march advances Y, because every path that
goes round again passes through it.

## Gotchas

The Makefile deliberately makes every object depend on every header. Without it, changing a layout constant in `vic4.h` leaves stale objects built against the old memory map, and the result looks like a hardware fault rather than a build problem.
