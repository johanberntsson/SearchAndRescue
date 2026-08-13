# Generating maps on the MEGA65 — is it feasible?

**Short answer: yes, it will work — but do it for the disk, not for the
clock.** The arithmetic below puts a full 1024x1024 colour map and 512x512
heightmap at **roughly 20-30 seconds** of generation. Every figure is an
estimate built from this project's own measured per-operation costs (see
Performance in `CLAUDE.md`), not a measurement of a generator that exists —
call it good to a factor of two, which is enough to answer "feasible", not
enough to promise a number.

> **Read the estimate as an assembly estimate.** The first pass to be built,
> the terrain noise, was 20x slower than the figure below when it was written
> in C — 1m05s on a real MEGA65 — and is **9.26 seconds** now that its two
> per-pixel loops are assembly, measured identically in xemu and on the machine.
> See steps 2b and 3 at the end. Nothing here was ever wrong about the
> *machine*: the numbers assume an inner loop of the kind `src/voxel_asm.s` is,
> and the C compiler is a factor of 7 or 8 away from that.

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
island at 159 KB and the plains at 334 KB, both resident in attic RAM at once,
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
- **the 32 KB program is the real constraint, and it is what forces the
  two-stage boot.** The link map says the game already fills it: `program` is
  **97.6% used, about 790 bytes free**, and the low RAM at `$1600` has 80 left.
  Noise, a histogram stretch, an island mask, hills, a priority flood with a
  heap, rivers with a blur field, colourise with its tables and a plane writer
  do not go in 790 bytes — for scale, the renderer's whole C half is 2.2 KB and
  the game's screens are 2.4 KB.

  It is worth being clear about what today's single program does, because it
  looks like the same job: it *loads*, it does not generate. Reading the
  crunched maps and unpacking them into attic RAM is `loader.o` plus the
  exomizer decruncher, about **2.2 KB** all in, which is why it fits beside the
  game at all.

  The split costs nothing, because the two are never live at the same moment
  and **attic RAM survives a program load** — stage one fills it, the game is
  loaded over stage one's code, and the maps are simply still there. It pays
  twice, in fact: as stage one the generator owns the whole 32 KB *and* banks
  1, 4 and 5, since no framebuffer or screen table exists yet; and the game
  loses the loader and the decruncher, which is nearly three times the
  headroom it has today. If even that is not enough, the generator splits
  again — terrain, then colour, then chain to the game — for the same reason.

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

**4. Verifying it.** This is the one with no existing answer — though the
handover block below is now a worked example of the shape the answer takes. `-dumpmem` writes
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
above.

**Step 2a, the two-stage boot: done, and it is not the same job as the
generator.** The plan above bundled "prove the boot" with "port `fbm`", on the
grounds that one experiment settles both. Splitting them was worth it: the boot
is a mechanism that either works or does not, and having it proved means the
first real generator pass can be judged on its own numbers rather than on
whether the machine came back at all.

`AUTOBOOT.C65` is stage one now (`src/mapgen/`) and the game is `SAR` on the
same disk. Stage one writes a block into attic RAM and chains; the game reads
it back and reports on its boot screen. Measured: **`STAGE ONE 27756`, all
16384 bytes identical after the program load.** Started on its own the game
says `NO STAGE ONE` and carries on, which is what `make prg` does.

### How the handover is done, and the two routes not taken

**The chain is ozmoo's restart trick** (`z_ins_restart`, `asm/disk.asm`): put
the command in the keyboard queue, return to BASIC, and let the screen editor
read it as though somebody had typed it. On the C65 that queue is **`$02B0`
with the count in zero page at `$D0`** — *not* the C64's `$0277`/`$C6`, which
is what ozmoo's own MEGA65 build uses, because that build runs the machine in
C64 mode and this game runs under BASIC 65. `RUN"SAR"` is nine bytes, so the
whole line fits the sixteen-byte queue and the editor echoes it; ozmoo prints
its command to the screen and queues only the RETURN because its line is longer
than that and needs cursor movement in it.

The screen need not be cleared first. The editor executes the logical line the
cursor is on, and BASIC's `READY` leaves it at the start of a fresh one, so
stage one's report simply scrolls out of the way — which is worth keeping,
since it is the only thing on screen if the handover fails. **A pure-BASIC
stage one would need a `NEW` before the `RUN`**; a PRG chained this way does
not, because `RUN"file"` resets the pointers itself.

Two routes were being built towards before that, and both fight the toolchain
rather than the machine:

- **a second BASIC line in stage one's own stub.** BASIC would run `10 SYS
  8206` and then `20 RUN"SAR"` with no keyboard queue involved, but Calypsi's
  stub is fixed: `mega65-plain.scm` pins `startup` at `$200E`, exactly the
  thirteen bytes `10 SYS 8206` occupies, and the library's `programStart`
  section is `root`, so overriding the `.pubweak __program_root_section` does
  not remove it. It needs a private linker script and a private stub, to buy
  what the keyboard queue gives for nothing.
- **stage one doing the Kernal `LOAD` itself.** The game is 32 KB at `$2001`
  and stage one is at `$2001`, so the loading code has to be somewhere the
  incoming program will not land on — and the only spare chip RAM in the 64 K
  window is `$1600-$1EFF`, which is exactly the region that must not be used
  across a Kernal disk call. The ROM does the whole thing after we are gone
  instead.

### What it cost the 32 KB, and what that says

The game-side check is scaffolding, written to be deleted: no checksum field,
because generating the stream and comparing every byte is both stronger and
smaller, and one format string behind a table rather than a `printf` per case.
Even so **the link failed by 15 bytes** on the first attempt, with `cstack`'s
4096 unable to place. It fits now with `cstack` untouched, but that is the
margin: the measurement `todo.md` asks for — a canary in `cstack` on a program
with no recursion — is on the critical path for the generator, not a
nice-to-have. It is also exactly the pressure the split is meant to relieve, in
the other direction: stage one has the whole 32 KB to itself.

### Step 2b, the noise: done, correct, and twenty times too slow

`src/mapgen/noise.c` is `fbm_octaves` on the machine — `maps/island.yaml`'s
512x512 field, into attic RAM. It is **right**: the device prints `17DFF8E6`
and `python3 tools/fbmcheck.py maps/island.yaml` prints `17DFF8E6`. Byte for
byte, through a sixteen-character report, which is the only channel there is —
`-dumpmem` writes chip RAM only and cannot see the field at all.

It takes **54.38 seconds in xemu and 1m05s on a real MEGA65**. The costing
above assumed 150-400 cycles a pixel; this is about **8400**, or **10000** on
the machine that matters.

**That 20% gap is itself a finding.** xemu's chip RAM timing was measured
against hardware once before, on the renderer, and came out **4%** optimistic
(11.6 fps against 11.0-11.2). On the C generator's instruction mix it is
**19.5%** — so the emulator's optimism is *workload-dependent* and 4% is not a
constant to carry around. The difference is not the attic RAM writes, which are
the obvious suspect and are not big enough: 512 KB of posted writes at +3 cycles
is 1.6M cycles, four hundredths of a second. What is left is the mix itself —
software stack indirection and $D770 accesses, sixteen multiplies a pixel of
them.

**Re-measured after the assembly rewrite, the gap is zero** (step 3 below), which
settles it: xemu models tight zero-page assembly accurately and the compiler's
output badly, so a C measurement taken in the emulator is optimistic by an
unknown amount and an assembly one can be trusted.

On the same real-hardware boot: **about 30 seconds to load the game and its
resources**, against the ~20 seconds measured in xemu. So a full two-stage boot
on hardware was roughly 65 + 30 seconds before the title screen, and the
generator was the larger half of it — which was the whole argument for the next
step. (After step 3 it is 50 seconds all in, and the generator is the *smaller*
half.) The startup benchmark table came out unchanged, so nothing else about the
machine moved, and the game plays as it did.

The measurement is sound, and both halves of that were checked rather than
assumed. The profiler's own clock says 54.38 s, and a real-speed run brackets
it from outside: the report is not up at 45 seconds of wall clock and is up at
62, with about 4 seconds of ROM boot in front. Note the internal figure is
identical under `-sleepless`, which is what makes these experiments cheap —
the profiler's clock is calibrated against the raster inside the emulator, so
it measures emulated seconds either way. (Do not time the *wall clock* under
`-sleepless`, and do not expect the speedup to be a constant: a loop that only
polls the raster runs far faster than one doing work, so a capture timed for
one is nowhere near the other.)

**Where it goes, by stubbing rather than guessing.** With the three `lerp16`s
and the `mulhi` in the inner loop replaced by adds of the same operands, and
everything else untouched:

| | seconds | cycles/px |
|---|---|---|
| the loop, with no arithmetic in it | 23.20 | 3580 |
| the arithmetic on top of it | 31.18 | 4820 |
| **total** | **54.38** | **8400** |

So **even with free multiplies this is 23 seconds**, and the multiplies
themselves are only 57% of it. That is not an algorithm problem. The listing
says what it is: `--list-file` shows `jsr mulhi`, `jsr smoothstep16` and a
swarm of cross-called `?L1xx` fragments, with the loop's locals living on the
software stack at `(_Vsp),y`. `--strong-inline` does not fix it — it made the
`jsr` count *worse*, 62 to 94.

**This is the renderer's history repeating, and it is worth reading that way
rather than as a disappointment.** The voxel march cost 1392 cycles a sample in
C and 182 in assembly; the same 7.6x on the hardware's 65 s is about **8.5
seconds**, and the forward-difference smoothstep (which removes the multiplies
from the inner loop entirely, see above) is a further factor on top of that. The estimate in this
document was implicitly an *assembly* estimate all along. It should be read as
"150-400 cycles a pixel is reachable, in the language the renderer's inner loop
is written in", not as something C was ever going to do.

### Step 3, the rewrite: 1m05s to about 11 seconds

Done, in three steps, with `17DFF8E6` unchanged at every one of them — which is
the whole value of having built the checksum before the optimisation.

| | xemu | note |
|---|---|---|
| straight C | 54.38 s | 1m05s on the real machine |
| keep the two lattice rows | 31.53 s | four multiplies a pixel down to two |
| the blend loop in assembly | 11.26 s | `src/mapgen/noise_asm.s` |
| the store pass in assembly | **9.26 s** | `src/mapgen/store_asm.s` |

**The first of those is not an assembly win and is the one worth remembering.**
`top` and `bot` — the two lattice rows an output row sits between, interpolated
along x — depend on the lattice row, not the pixel row, so they are good for
the `step` output rows that share one; and when the lattice row moves it moves
by exactly one, so the new top is the bot already built and a pointer swap plus
one rebuild covers it. That alone was 1.7x, in C, before a line of assembly.
It paid for its 8 KB by shortening the weight table, which is one lattice cell
per octave rather than one row: the weight is how far across a cell a pixel is,
so it repeats.

The two assembly loops are arranged around the same few facts. 64 pixels a
chunk, because that keeps both index registers eight bits wide — the edge rows
are 16-bit entries so Z walks by twos, `acc` is 32-bit so Y walks by fours, and
64 pixels of `acc` is exactly 256 bytes so its pointer's high byte simply
increments. The multiplier's A high half is zeroed once a call; B's is
rewritten per multiply only because `amp` is ONE on the first octave and does
not fit sixteen bits. The store pass clears `acc` as it reads it, which deleted
a whole pass over the row, and keeps `weight_recip` in the multiplier's B input
for the entire field — the one operand in either loop that never moves.

And the arithmetic trap that survives into assembly: **numpy's `>>` floors**, so
a downward interpolation is `top - hi - (lo != 0)`. The product's low word is
read for no other reason.

**Stage one prints its own total now, and it is not the same as the boot's.**
`STAGE ONE 9.43 SECONDS` against `9.25` for the noise itself: everything else
stage one does -- the handover proof block, the lattices, the weight tables --
is 0.18 s. What is *not* in that figure is the ROM's boot and the report's
pause, and those are what made a stopwatch at the machine read 19 seconds for a
9-second job. **The pause was also mis-timed**: it counted wraps of `$D012`,
which on this machine come about 122 times a second rather than the 61 a
312-line VIC-II frame implies, so `REPORT=20` held for about eight seconds.
It runs off the profiler's clock now -- which stage one still owns at that
point, since the timers go back to the Kernal afterwards -- and it has its own
knob, `make HOLD=n`, defaulting to four seconds because it is on the critical
path of every boot where the game's report is not. `make HOLD=60` to read it at
the machine.

So a boot is roughly 2 s of ROM, 9.4 s of stage one, 4 s of looking at it, and
then the game.

**On the real machine the gap is no longer measurable.** Timed on a MEGA65
against xemu at the same two points -- **by stopwatch, so give or take a few
seconds either way**:

| | xemu | MEGA65 |
|---|---|---|
| boot to the end of stage one | 19 s | 19 s |
| ... to the game's title screen | 42 s | 50 s |

So whatever is left of the emulator's optimism on the generator is inside
stopwatch error, where the C version had the machine 19.5% slower -- and *that*
figure is safe from the same doubt, being 1m05s against 54.38 s, ten seconds
apart. **The way to settle it exactly is on screen**: stage one prints its own
time from the profiler's clock, so reading `STAGE ONE 9.43 SECONDS` off a real
machine is a measurement rather than an estimate. Worth doing next time
somebody is sitting at one. **That is the strongest evidence yet that
xemu's optimism is a property of the instruction mix and not of the emulator**:
what it was mismodelling was the C code's software-stack indirection and its
sixteen `$D770` accesses a pixel, and tight zero-page assembly it gets right —
the same kind of code as the renderer's march, which it was within 4% on.

The 8 seconds that are left are **all disk**: 23 seconds of loading the game and
its resources in xemu against 31 on the machine, a real drive against an
instant one. That is the next thing worth attacking, by crunching the game the
way the maps already are, and it disappears anyway once the generator is real
and the maps stop being files.

**A whole boot on hardware is about 36 seconds**, against roughly 95 before
this work: 12 to the handover -- of which 9.43 is generating, measured, plus 4
of looking at the report and the ROM's own start -- and some 24 more to the
title screen. All of those but the 9.43 are stopwatch figures, and the 24 is a
subtraction of two of them, so treat it as "the disk is about twice the
generator" rather than as a number. The generator is now the smaller part of
the boot, which is the reverse of where this started.

**And the generator only does one map.** The disk carries two, and the game
still loads both off it; when the generator covers both it will be about 19
seconds of generating against the twenty-odd of loading it replaces. That is the
comparison that actually decides whether on-device generation is worth booting
into, and it is close enough that the passes still to be ported -- stretch,
mask, hills, water, colour -- matter to the answer.

What is left is **`edge_build`**, the one loop that runs per lattice row rather
than per pixel and is still C. At 1430 cycles a pixel overall with the blend
loop accounting for perhaps a third, it is the next thing to look at — but the
estimate at the top of this document is now within reach rather than twenty
times away.

### Step 4, the stretch: 1.98 seconds, and right first time

`081B1D88` on the device and from `python3 tools/fbmcheck.py maps/island.yaml
--stage stretch`. The checker takes a `--stage` now, so each pass keeps its own
number to be checked against rather than only the last one.

This is the **measure** of build, measure, paint, and the reason the pipeline
cannot be one streaming pass: nothing can be painted until the whole field has
been looked at. Two more passes over the field, both per-pixel and so both
assembly from the start — which is the rule step 3 bought, applied.

Three things it settled that the next passes inherit:

- **the histogram fits only because it shares the edge caches.** Stage one had
  under 4 KB of its 32 spare and the table is 4100 bytes; the caches are 8 KB
  and are dead the instant the field is finished. A union, and the same bargain
  the loader's staging buffer strikes with the sprite's. **Later passes should
  expect to do this too** — the working set is bigger than the program.
- **a pass over the field costs about a second before any arithmetic**, which
  is what the two passes here measure at 1.98 s together. That is the number to
  hold against any proposal to add another one, and the argument for folding
  per-pixel functions of one value into a single pass.
- **a constant operand belongs in the multiplier's B input for the whole
  pass.** The reciprocal goes in once and never moves, as the store pass's
  weight does. The clip then costs only a test of the product's top four bytes.

Stage one is **11.43 seconds**: 9.26 of noise, 1.98 of stretch, 0.19 of
everything else — the lattices, the weight tables and the handover block, which
are worth knowing are negligible before anybody optimises them.

What that means for the plan:

- ~~the next step is the inner loop in assembly~~ — **done, see step 3 below.**
  The checksum did exactly the job it was built for: `17DFF8E6` through three
  rewrites, the way `C_SPAN` unchanged was the march's cheap check.
- **do not port any more of the pipeline in C first.** The next passes are
  cheaper per pixel than the noise but they are the same shape, and porting
  them in C would mean writing every one of them twice. Write the row cache's
  kind of structural win in C, where it is easy to find, and the per-pixel loop
  in assembly straight away.
- the arrangement around the loop is already what the costing asks for, so
  none of it has to change: octaves summed in a chip RAM row buffer, the field
  touched in attic exactly once on the way out, CPU stores rather than a DMA
  out of a row buffer.

Two smaller things the port turned up:

- **`stretch` cannot be ported without a decision.** It ends with
  `np.clip(..., 0, ONE)`, and ONE is 65536, which does not fit the uint16 a
  field value is stored in — the top half per cent of the map lands exactly
  there. `tools/fixed.py`'s own rule is that ONE is only ever an intermediate,
  so clipping to 65535 on both sides is probably right, but it re-rolls every
  map by up to one part in 65536 and wants the PNGs diffed before and after.
  The note is in `noise.c`, where whoever ports it will be standing.
- **`genmap.py`'s `fbm()` is now `stretch(fbm_octaves())`**, so the half that
  is ported has a name on both sides and the device and the PC quote one
  definition rather than two copies of ten lines.
