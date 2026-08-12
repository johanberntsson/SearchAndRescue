# Generating maps on the MEGA65 — is it feasible?

**Short answer: yes, and it should be faster than loading a map from disk.**
The arithmetic below puts a full 1024x1024 colour map and 512x512 heightmap at
**roughly 20-30 seconds** of generation, against the **~60 seconds** the game
currently spends reading and decrunching 661 KB of them off the d81. Every
figure here is an estimate built from this project's own measured per-operation
costs (see Performance in `CLAUDE.md`), not a measurement of a generator that
exists — call it good to a factor of two, which is enough to answer "feasible",
not enough to promise a number.

The interesting part is that the cost is not where it looks. The generator is
not competing against zero, it is competing against a minute of disk; and it
buys a d81 that holds *every* mission's maps in a few hundred bytes each
instead of one mission's in 661 KB.

## What it has to beat

| | today | generated on device |
|---|---|---|
| disk per map pair | 661 KB crunched | ~100 bytes of mission struct |
| time from boot to flight | ~60 s, nearly all of it loading | ~20-30 s, estimated |
| maps per d81 | one | as many as there are missions |
| exomizer in the loop | yes | no |
| `convmap.py` in the loop | yes | no — the generator writes the planes |

The second row is the one that decides it, and it is not close enough to be
obvious in advance: this is a real experiment, not a formality. But the
downside case — generation lands at 60 s and only breaks even on time — still
wins the other four rows.

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
| percentile stretch | 50 | 1.3 | a 256-bucket histogram pass, then apply |
| ridged fold, island mask | 40 | 1.0 | both are 256-entry lookups |
| hills | — | 0.3 | 16 stamps of a few thousand pixels |
| lakes | — | 1-2 | priority flood, ~1000 cycles a cell taken |
| rivers | — | 2 | the walk is cheap; the flow blur is a pass |
| colourise | 250-350 | 7-9 | gradients, ramp, sun, two dithers, water |
| write the planes to attic | — | 0.5 | 1.3 MB of DMA, once |
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
- **stream by row, keep three rows.** Everything downstream of the field needs
  neighbours: the slope, the sun, the local minima. Three rows in chip RAM is
  6 KB and makes those reads chip-speed.
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
example missions get new terrain from the same seeds), and it is the step where
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
4. `mission.bin` becomes the generator's input rather than the game's — the
   game keeps reading the item block out of it.

Before any of that, **measure DMA from chip RAM to attic RAM**. `src/bench_asm.s`
measures attic-to-chip (17.8 cycles a byte, seven times slower than
chip-to-chip) and the reverse direction has never been measured. The whole
"write the planes" line in the table above assumes it is not another surprise;
it is twenty minutes of work to find out, on a machine that has already been
wrong about attic RAM twice.
