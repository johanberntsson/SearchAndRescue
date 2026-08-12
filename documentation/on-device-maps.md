# Generating maps on the MEGA65 — is it feasible?

**Short answer: yes, it will work — but do it for the disk, not for the
clock.** The arithmetic below puts a full 1024x1024 colour map and 512x512
heightmap at **roughly 20-30 seconds** of generation. Every figure is an
estimate built from this project's own measured per-operation costs (see
Performance in `CLAUDE.md`), not a measurement of a generator that exists —
call it good to a factor of two, which is enough to answer "feasible", not
enough to promise a number.

## What it has to beat — measured, and not what this document first assumed

The first draft of this page compared generation against "~60 seconds of
loading" taken from `CLAUDE.md`, and concluded it would be *faster*. **That was
the wrong comparison**, and measuring it is what showed why: the sixty seconds
belongs to the hand-drawn map, and the thing an on-device generator would
replace is a *generated* one — which crunches four times smaller.

Timed in xemu at real speed, screenshotting a boot at 6, 9, 12, 18 and 24
seconds (`-headless` without `-sleepless`, one run at a time, because four
emulators at once distort each other's timing):

| | crunched | loads in |
|---|---|---|
| generated island, h512 c1024 | **152 KB** | **9-12 s** |
| hand-drawn D1/C1W, same sizes | 661 KB | ~44 s at that rate |

About 15 KB a second, Kernal read and exomizer decrunch together. **Procedural
maps crunch 4.4x better than the hand-drawn pair** — 6.8:1 against 2.2:1 on the
colour map — because they are smooth by construction and spend 130 palette
entries where the hand-drawn one spends 184. The generator has been quietly
paying for itself on disk since the day it worked.

So the honest table is:

| | maps as files today | generated on device |
|---|---|---|
| disk per map | 152-335 KB crunched | ~100 bytes of `map.bin` |
| time before the flight | 20 s for the two on the disk, measured | 20-30 s per map at full size, estimated |
| maps per d81 | **two, built and flown** — perhaps four | as many as you care to write |
| exomizer in the loop | yes | no |
| `convmap.py` in the loop | yes | no — the generator writes the planes |

The first row is now a fact rather than a projection: the disk carries the
island at 152 KB and the plains at 335 KB, both resident in attic RAM at once,
and the game switches between them for 512 bytes of plane table. A generated
map varies more than expected -- the plains are twice the island, being all
small features and dither where the island is mostly flat sea -- so "about
five" was optimistic and four is the honest ceiling.

**Generation loses the time row and wins the disk row**, and the disk row is
the one that was worth having in the first place: the whole point of the YAML
pipeline is more country than a disk can hold as pixels. Four maps is not many —
and note that the *game* already got most of this win without going on-device
at all, since it is generation that shrank a map from 661 KB to 152. What
on-device buys on top is the difference between four worlds and any number.

There is a version that wins both, and it is the resolution trade below:
generate the height field at 512x512 — which is what ships anyway — and paint
the colour map from it. That is four times less noise and four times less
flood, under ten seconds all in, and it beats the disk on both counts. It costs
bit-identity with `genmap.py`, which is a decision to take deliberately.

## Where the time goes

The method: every pass over the field costs at least what this project has
already measured a pass to cost — **26 cycles per sequential byte in chip RAM,
41 in attic** — plus its own arithmetic, at **85 cycles per hardware multiply**
through `$D770` as `voxel_mul_shift8` measures it (call and all; inline in a
loop it should be nearer 60). The CPU is 40.5 MHz. A pixel is 1024x1024 =
1,048,576 of them, so **one cycle per pixel is 26 milliseconds** and a hundred
cycles per pixel is 2.6 seconds. That single conversion is most of what the
estimate is.

| stage | cycles/px | seconds | note |
|---|---|---|---|
| fbm, 5 octaves | 150-400 | 4-10 | the dominant term; see below |
| percentile stretch | 50 | 1.3 | a 1024-bucket histogram pass, then apply |
| ridged fold, island mask | 40 | 1.0 | both are 256-entry lookups |
| hills | — | 0.3 | 16 stamps of a few thousand pixels |
| lakes | — | 1-2 | priority flood, ~1000 cycles a cell taken |
| rivers | — | 2 | the walk is cheap; the flow blur is a pass |
| colourise | 250-350 | 7-9 | gradients, ramp, sun, two dithers, water |
| write the planes to attic | — | 0.32 | 1.31 MB of DMA, once, measured |
| **total** | | **~20-30 s** | |

**The noise is the term that decides it, and it has a factor of three in it.**
Value noise along a row is `A + B * S(fx)` where `A` and `B` are constants for
the lattice cell and `S` is a smoothstep — so the obvious form is one multiply
per octave per pixel (~80 cycles with bookkeeping, 400 for five octaves, 10 s).
But `S` is a *cubic*, so stepping it along a row is three adds and no multiply
at all — the standard forward-difference trick — and the state is only
re-initialised at each lattice boundary. That is ~25 cycles an octave, 150 for
five, under 4 s. Worth writing the multiply version first and the difference
engine second, since the second is where an off-by-one is invisible.

Three structural rules keep those numbers honest:

- **accumulate the octaves in chip RAM, not attic.** A row of 1024 16-bit
  accumulators is 2 KB. Done that way the field is touched in attic once per
  pixel, on the way out; done the naive way it is read and written once per
  octave and the attic penalty alone (+15 a read) costs more than the noise.
- **write it up there with the CPU, not with a DMA.** Measured on the real
  machine (below), a DMA into attic RAM costs 9.54 cycles a byte and a posted
  CPU write costs +3 over a chip one — so a row buffer followed by a DMA is
  three times the price of storing each pixel as it is computed, in a loop
  that is running anyway. The DMA is for moves that cannot be folded into a
  loop; producing a field is not one of them.
- **stream by row, keep three rows.** Everything downstream of the field needs
  neighbours: the slope, the sun, the local minima. Three rows in chip RAM is
  6 KB and makes those reads chip-speed. Reads are the expensive direction —
  16.11 cycles a byte out of attic against 9.54 in — so the asymmetry is
  worth designing around: produce freely, re-read as little as possible.
- **fold passes together.** The percentile apply, the ridged fold and the
  island mask are all per-pixel functions of one value; they are one pass with
  three lookups, not three passes.

### What could be cut, and what it buys

- **generate the height field at 512x512** — which is what ships anyway — and
  interpolate it up when painting the 1024 colour map. Four times less noise,
  four times less flood: the total drops to well under 10 s. The cost is that
  the device's map is then not bit-identical to `genmap.py`'s, which
  box-averages a 1024 field down. That is a decision to take deliberately (see
  Verification), not a saving to take quietly.
- **do the water at 256x256**, one sample per map cell, and upsample the mask.
  Lakes and rivers are macro features and the flood is the fiddliest code here;
  at 256 the visited set is an 8 KB bitmap instead of 128 KB and the heap is
  small enough to keep in chip RAM.
- **drop the meander noise and the mottle dither.** Each is a whole value-noise
  field for a small effect. They are also exactly the two things that would be
  missed, so this is a last resort.

## Memory

Nothing here is short of room, which is worth saying plainly because the game
proper is:

- **the generator is stage one and owns the machine.** No framebuffers, no
  screen tables, no sprites, no resident maps — bank 1, bank 4 and bank 5 are
  about 100 KB of scratch, on top of the usual 32 KB at `$2001`. The heap for
  a priority flood, the three-row window, the histogram and the lattice tables
  all fit several times over.
- **attic RAM holds the working field.** 1024x1024 of 16-bit height is 2 MB of
  the 8, the finished colour map another 1 MB, and rivers need a pristine copy
  of the terrain (`documentation/procedural-maps.md`, the canyon trap) — call
  it 5 MB peak against 8 MB. Tight enough to plan, not tight enough to redesign
  around.
- **the 32 KB program is the real constraint.** Noise, flood, rivers, colour
  and the plane writer in one 32 KB image, in a language whose 16-bit multiply
  is a library call, is not obviously going to fit. The two-stage boot already
  splits generator from game; if it comes to it, the generator splits again —
  terrain, then colour, then chain to the game — because each stage's output is
  in attic RAM and survives the load.

## The four things that are actually hard

**1. Fixed point, and the RNG.** `genmap.py` is written in floats and draws
from numpy's PCG64. Neither can move to the 45GS02. Both have to be replaced
*on the PC first* — 16.16 fixed point throughout, and a 32-bit xorshift of the
kind `src/weather.c` already has — so that the previewer and the device are
running the same arithmetic rather than two dialects of it. This is the bulk of
the work and it is all of the risk: it re-rolls every existing map (the three
example maps get new terrain from the same seeds), and it is the step where
"the same YAML gives the same map" quietly stops being true if it is done
carelessly. Do it as its own change, with the tools still on the PC, and prove
it there.

**2. The global statistics.** Two things in the pipeline look at the whole
field before deciding anything: the percentile stretch of the octave sum, and
the 99th-percentile top of the land ramp. Both become histogram passes — 256
buckets, one add per pixel — which is fine, but it means the field cannot be
generated and painted in one streaming pass. Three passes over the field is the
shape to design for: build, measure, paint.

**3. The flood fills.** The lake flood is a priority queue plus a visited set,
which is the least 6502-shaped code in the generator. At 256x256 it is an 8 KB
bitmap and a heap of a few thousand 4-byte entries, both in chip RAM, and about
a thousand cycles per cell taken. That is affordable. At 1024x1024 it is not
obviously so, which is the strongest argument for doing the water coarse.

**4. Verifying it.** This is the one with no existing answer. `-dumpmem` writes
**chip RAM only**, so it cannot see a map in attic RAM at all, and `checkview`
compares a rendered *picture*, which would catch a broken generator but not
tell you where. The answer that costs nothing: have the generator compute a
checksum per plane as it writes and print it, and have the PC tool print the
same checksums. Identical checksums are a byte-for-byte proof through a
sixteen-character report. Do that from the first day of the port, not after the
first mystery.

## What gets simpler

Three things fall out of the pipeline entirely, and one of them was a
long-standing wart:

- **no exomizer and no crunched resources.** The maps are never a file.
- **no plane packing at load.** `convmap.py` splits each map into 256x256
  planes on 64 K boundaries because that is what `src/voxel_asm.s` addresses;
  a generator writing into attic RAM writes them in that layout to begin with.
- **the palette stops being negotiated.** `convmap.py` currently hands sprites
  whatever indices the colour map left free, which is why the design note about
  sprite indices below 16 is still open. With `maps/palette.yaml` fixed —
  water, the 21x6 land ramp, masonry — the layout is known at build time and
  the sprites can have fixed slots. The two-dimensional ramp bought that.

The overview map goes with it: 32x32 point samples of a colour map the machine
now has in hand is a few thousand cycles, not a file.

## The order to do it in

1. **Rewrite `tools/genmap.py` in fixed point with a portable RNG**, on the PC,
   changing nothing else. Verify with `checkview` and by eye. This is the big
   one, and it is worth doing even if the port stops here — it is what makes
   the algorithm portable at all.
2. **Port `fbm` alone.** A stage-one PRG that generates one 512x512 field into
   attic RAM, times itself with `src/profile.c`, and prints a checksum. That
   single experiment settles the dominant term of the estimate and the whole
   two-stage boot mechanism at once, and it is an evening's work rather than a
   week's.
3. Then the rest in pipeline order, each with its checksum: stretch and mask,
   hills, water, colour, planes.
4. `map.bin` -- the map file pre-parsed into a struct -- becomes the
   generator's input rather than a build product. A stage-one generator reads
   it and never needs to know that missions exist; what the *game* reads is
   `mission.bin`, which is a different file for a different thing. See
   documentation/procedural-maps.md.

## Where the port has got to

**Step 0, the DMA measurement: done, on the real machine.** `P_DMA_TOATTIC` is
a slot in `src/profile.h`, timed beside the other two directions and printed by
both `tools/profread.py` and the on-screen report; `make REPORT=120` holds that
report up long enough to read. Under xemu all three directions read 2.59 cycles
a byte, the emulator not modelling the attic bus at all. On the MEGA65:

| | cycles/byte | vs chip to chip |
|---|---|---|
| DMA copy chip to chip | 2.20 | |
| DMA copy attic to chip | 16.11 | 7.3x |
| **DMA copy chip to attic** | **9.54** | **4.3x** |
| DMA fill, chip | 1.12 | |

**The bus is asymmetric, and in the direction a generator wants**: writing into
attic RAM is 1.7x faster than reading out of it, the same story the CPU figures
tell with a posted write at +3 against a read at +15. Two things follow, and
the second is a correction to the plan above:

- **the planes are free to write.** The finished 1024x1024 colour map and
  512x512 heightmap are 1.31 MB, which is 0.32 seconds of DMA. It stays a
  rounding error in the estimate.
- **do not buffer a row and DMA it out.** At 9.54 cycles a byte that is three
  times what a posted CPU write costs in a loop that already has the value in
  a register. Compute straight into attic RAM.

Reads are the direction to be careful with: a full pass over a 2 MB working
field is 0.83 seconds before any arithmetic, which is what makes "build,
measure, paint" three passes rather than ten.

**Step 1, fixed point: done. `tools/genmap.py` is integers now.**

- `tools/fixed.py` is the arithmetic: Q0.16, one multiply-and-shift, 257-entry
  tables for sqrt/tanh/gamma, an exact digit-by-digit `isqrt`, and xorshift32
  with the three draws the generator makes. `python3 tools/fixed.py` checks
  each routine against the float it replaces and prints the error in height
  units and colour steps: **worst 0.007 height units, and sqrt exact to 0.003**.
- The pipeline was ported beside `genmap.py` as `tools/genfixed.py`, compared
  against the float version stage by stage, and then **swapped in**: the float
  path is gone and so is the second file, because two generators is not a
  state to keep. What the comparison said before the swap, over all three
  example maps:
  - **terrain**, given the same draws: worst **0.01-0.03 of one height unit in
    120**, and the island's coastline moving on 0.001% of its pixels.
  - **colour**, given the same terrain and water: 4-15% of land pixels land on
    a different palette entry, **none by more than one ramp step or one sun
    shade**. Worth its proportion — the generator dithers the ramp by ±1.4
    steps on purpose, so the port disagreed by less than the noise the map is
    drawn with.
  - **water** cannot be compared pixel by pixel, because the *choosing*
    differs and not the arithmetic: a 45GS02 cannot argsort tens of thousands
    of local minima, so the candidates come from a histogram median instead.
    It is compared as water — how much, in how many bodies, how big.

Six things that cost a debugging round each, and would have cost more in C:

- **the comparison has to feed both sides the same draws.** PCG64 and
  xorshift32 build different maps however good the arithmetic is; the first
  comparison reported 8 height units of "error" that was nothing but the seeds
  re-rolling. The harness answered the float generator's stream interface out
  of the xorshift instead, which left the arithmetic as the only difference.
- **`isqrt`'s domain is 32 bits and the first caller blew straight past it.**
  Asking `hypot` for sixteen fractional bits hands the root 2^45; it silently
  returns the root of what fits under 2^30, every disc profile came out as
  garbage, and the hills grew twenty height units past the map's range. Disc
  distances are measured at 8 fractional bits now — a 256th of a pixel — and
  the function raises rather than lying.
- **the percentile stretch's bucket width lands on the map's heights.** A
  256-bucket histogram put the mountains map a systematic 0.77 height units
  below the float version — a bias, not noise, because both ends of the stretch
  move and everything scales between them. 1024 buckets costs 4 KB and a fifth
  of the error; interpolating *inside* the bucket — one divide, at setup, from
  counts already in hand — takes it to 0.01. Twice, that one: the first
  attempt returned the bucket *below* the one the value is in and made
  everything worse, which is the sort of off-by-one a histogram hides well.
- **the ramp's gamma table has to reach past 1.0.** `t` is a height over the
  map's own 99th percentile, so the top one per cent of the terrain is above
  1.0 by construction. A table over 0..1 saturates exactly the pixels that
  should be reaching the top of the ramp, and a flatland's hilltops came out
  the colour of its middle slopes. The domain is 0..2 and the read is a shift.
- **the comparison measured the stream a second time.** `colourise` draws two
  noise fields for its dithers, and by then the two pipelines had consumed
  different numbers of draws — the lakes see to that — so the dithers were
  different fields entirely and *70%* of pixels "differed" by a step of
  dither. Re-seeded identically it is 4-15% at one step. Every comparison in
  this port had to be told what it was holding constant, and twice it was not.

**What the swap cost, and it was not nothing: every map re-rolled.** xorshift
draws different offsets from the same seed, so the island is a different island
— an equally good one, flown and looked at, but not the same one. Both
consequences landed as predicted. The pyramid at 422,481 came out under a lake
and the generator refused to build it, exactly as it should; it moved to
426,274, a headland where it reads better than it did before. And the reference
screenshot was of a map that no longer exists, so it was retaken.

**Step 1 is done.** What is left is the machine itself: `fixed.py` and then the
pipeline into C, one routine at a time, each with the checksum verification
above — starting with `fbm`, the experiment that settles the dominant term of
the estimate and proves the two-stage boot at the same time.
