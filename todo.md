# What's next

## Where it is

Two missions, end to end: title, mission list, briefing, flight, debrief.
Mission one finds the survivor on the pyramid at 46.713N 8.110E — get within
ten cells with them on screen and press `SPACE`. Mission two drops an EpiPen to
a pair of hikers on the lake shore at 46.597N 8.227E — get within five cells
and press `RETURN`, and it fails if the one EpiPen goes down anywhere else.
`RUN/STOP` abandons a flight, and sport mode (`3`) has no terrain following, so
the third way to fail is to fly into a hill. The briefing gives the fix and the
panel gives your own.

Underneath: a 320x152 3D view over a six-row 40-column text panel with an
overview map, 12.5 fps (`PROFILE=0`, default map sizes). The march is 160
rays; each fills the two pixels it owns.

Build knobs, all in the Makefile:

| | |
|---|---|
| `PROFILE=0` | no per-column instrumentation; use it for timing |
| `WIDE=1` | march 320 rays instead of doubling 160 pixels |
| `FLYNOW=1` | skip the menus; a headless run needs it to render anything at all, since it cannot press a key |
| `HGT_SIZE`, `COL_SIZE` | map resolutions, powers of two from 256 to 1024 |
| `make release` | not a knob but a target: the `PROFILE=0` disk, into `release/sar-latest.d81` |

Where a frame goes (the older 160-pixel framebuffer, h256 c512, 64.7 ms; the
shape is the same at other settings):

| | ms | | |
|---|---|---|---|
| ray march | 46.0 | 71% | 10240 samples at 182 cycles |
| terrain span fills | 10.0 | 15% | 14489 pixels at ~28 |
| span prologues | 3.4 | 5% | 3983 colour reads |
| per-column C setup | 2.4 | 4% | 607 cycles a column |
| sky DMA | 2.1 | 3% | |
| panel and everything else | 0.8 | 1% | |

Every column takes all 64 samples — none terminates early — so the march is
flat in the scene and scales only with **column count**. That one fact governs
most of the trades below.

## Next

**The renderer is good enough for a game.** Called on 9 Aug 2026, comparing a
fresh screenshot against the first one: clearly more detail, and faster than
it started. The optimisation ideas left in Open below are there so they are
not rediscovered from scratch, **not** as a queue to work through — the next
work is the game, not another few percent.

- **Flight feel.** The keys are all there now, but thrust is still a per-frame
  constant that jumps to full speed and back. Wanted: long push → big speed
  change, short push → small, accelerating towards a target rather than
  snapping to it, and scaled by the measured frame time, because between 7 and
  15 fps a per-frame constant does not feel the same twice. Joystick too.
- Then more of the game. `documentation/vision.md` has the design; the two
  missions are the first pieces of it. A third is a table entry and a sprite
  sheet — but only **12 palette entries** are left, so the next figure needs
  fewer than fifteen colours or has to share with one that exists.
- Both missions still start the drone in the middle of the map, which for the
  lake is a hundred cells away and past the draw distance. A launch point per
  mission, or a heading cue, would save a lot of flying on a compass.
- The sprite draw is C at ~170 cycles a pixel. Harmless at any distance you
  would search from, 9% of a frame nose to nose. Assembly when there is a
  reason, not before.
- One billboard, one depth. Several at different depths need either a clip
  step per sprite (another pass of the march) or a real per-column depth
  buffer. Sorting them and snapshotting at the nearest is probably enough.

## Decisions already settled

Do not re-litigate these without new measurements.

- **Detail per cycle lives in the map, not in the raster.** Marching 320 rays
  doubles the march and halves the frame rate for a picture that barely
  changes; a finer colourmap is free and plainly better. `WIDE=1` is kept as
  an option, off by default. The 40-column panel used to be its argument and
  is no longer: the framebuffer is 320 wide with a 160-ray march.
- **A raster split cannot give the panel its own geometry.** The VIC-IV
  latches `SCRNPTR`, `LINESTEP`, `CHRCOUNT` *and* `CHRXSCL` once a frame, so a
  mid-screen write does nothing until the next frame and the last value
  written wins all of it — proved by swapping the two `CHRXSCL` values and
  watching the whole display change together. Only per-pixel registers answer
  mid-frame. The interrupt itself works; two traps are written up in
  `CLAUDE.md` (the compare is `$D012` in VIC-II lines, not `$D079`, and the
  C65 ROM's `$0314` dispatcher pushes **five** registers, so a C64-style exit
  kills the machine). The working handler is kept in
  `documentation/experiments/raster-split.patch`.
- Colourmap resolution is free at any size: read once per span, and the plane
  lookup is one `lda abs,x` whether there are 4 planes or 16.
- Heightmap above 256x256 costs a flat 5.7 ms (8.8%) because it leaves chip
  RAM — the **same** at 512 or 1024. So h1024 is no slower than h512; it just
  cannot share a d81 with c1024 (1013 KB against ~793 KB).
- **h512 is the right heightmap and h1024 is wasted.** `Z_STEP0` is half a
  cell, the closest the ray ever samples, so h512 matches the march exactly
  and h1024 is 2x past it. Diffed at a fixed camera: h512→h1024 moves 2.1% of
  view pixels cruising and 8.7% nose to the ground, all of it one-pixel span
  edges with no new feature anywhere. h256→h512 is 4.2%/13.3% and a difference
  in kind — h256 stair-steps the near shoreline into rectangles. Spending on
  h1024 first is 350 KB and ~15 s of loading for jitter.
- Rendering into attic RAM and DMAing the frame back is **dead**: attic-to-chip
  DMA is 17.8 cycles a byte against 2.45 chip-to-chip, so one 320x152 buffer
  would be 21 ms a frame. The writes were never the problem (+3 cycles).
- A pre-rendered sky in attic RAM is dead for the same reason. The chip-RAM
  strip template costs 2.1 ms; from attic it would be 13.5.
- `ADCQ` for the ray position is **slower** (68.5 ms against 64.7). The Q
  pseudo-register is A/X/Y/Z, so it destroys the march's step index in Y.

## Open

- The far edge of each z band pops as you fly. Either blend the bands or
  shorten the step growth.
- The gimbal shears the picture rather than rotating it, because moving
  `cam->horizon` is a shear. Fine at the tilts flying uses; it will look wrong
  long before straight down.
- The panel is nearly full now: message, ALT/HDG, LAT/LON, FPS/SPD/BATT and
  CARGO/WIND, with the overview map on the right.
- **Under 200 bytes of the 32K are left.** The low free RAM at $1600 still has
  about 1.2K spare, but only renderer-owned tables may go there. After that
  the candidates are the loader's 512-byte bounce buffer and the sprite's
  1028-byte drawing buffer.
- No HUD over the 3D view. `vision.md` wants an artificial horizon, battery,
  GPS and signal strength. Palette 240/241 are reserved for it; `hud.c` did
  exactly this kind of pixel drawing and is in git history if wanted back.
- `Z_STEP0`, not `HGT_SIZE`, is the lever for near-field detail. A 0.25-cell
  band in front (5x16: 0.25/0.5/1/2/4) reaches 124 cells for 80 samples
  instead of 64 — about 25% more march, ~11 ms — and it is the only thing that
  would give h1024 something to show. Worth keeping in mind, not queued.
- 1024x1024 for **both** maps needs a second disk, or a packed format better
  than exomizer's 1.9-2.2x on this data. Settled above that it is not worth it
  even if it fit.
- Remaining march ideas, all small: the per-column C setup is 607 cycles and
  two `mul_shift8` calls; the position update is 40 of the 182 cycles a
  sample.

## Done

- Calypsi toolchain, d81 build with `autoboot.c65`, resources as SEQ files.
- Minimal voxel engine, 160x192 double buffered, ASWD/RF to fly.
- Profiler (`src/profile.c`, `tools/profread.py`) with a raster-calibrated
  clock, plus micro-benchmarks in `src/bench_asm.s` and an on-screen report
  for real hardware, which has no `-dumpmem`.
- **0.74 → 10.3 fps**: the compiler's 32-bit multiply was 64% of the frame;
  the hardware multiplier and then an assembly inner loop did the rest.
- Attic RAM measured on real hardware: a read is a flat +16 cycles, a write
  +3, attic-to-chip DMA 7.3x slower than chip-to-chip.
- **Sky by DMA, 10.3 → 11.3 fps.** One strip template, `FB_COLS` copy jobs,
  and it doubles as the frame's clear pass.
- **Split screen, 11.3 → 12.6 fps.** 19 rows of 3D over a 6-row text panel;
  `hud.c` gone. Needed `CHARPTR` set and panel ink moved into the first
  sixteen palette entries, because a text character's colour is a four-bit
  field.
- **Inner loop, 302 → 182 cycles a sample, 12.6 → 15.5 fps**, output
  pixel-identical: carried the write pointer between spans, moved the
  position's whole-cell bytes into the map pointer, biased the height
  difference positive.
- 320-wide rendering behind `WIDE=1`.
- **Overview map and GPS readout, 12.9 → 12.7 fps.** `convmap.py` ships the
  colourmap scaled to 32x32 as sixteen full-colour character tiles, so the
  MEGA65 only has to name them; the crosshair is drawn into the map and lifted
  with a 1024-byte DMA from a pristine copy. A hardware sprite for it does
  **not** work — `SPRPTRADR` is ignored by xemu and `SPR_PTR16` segfaults it,
  so the pointer comes from screen+`$3F8`, which lands inside the 3D view.
  Retry on real hardware if it matters.
- **40-column panel, 13.9 → 12.9 fps.** The framebuffer went to 320 pixels and
  the span fill writes each of the 160 rays to two neighbouring bytes; the 3D
  view came out pixel-identical, because that is exactly what the VIC-IV
  stretch had been doing.
- Exomizer compression (`src/exo_asm.s`, ported from mega65/ozmoo-z6)
  unpacking into attic RAM, and both map resolutions as build knobs up to the
  source 1024x1024.
- **Survivor billboard, 12.7 → 12.5 fps.** `convmap.py` cuts the front pose
  off the sprite sheet's checkerboard and gives it fifteen palette entries the
  colourmap left free; `src/sprite.c` projects it with the renderer's own
  numbers and clips it against the march's y buffer, sampled at the first span
  behind it. The clip is the whole cost, 0.8 ms a frame; drawing is 0.03 ms
  unless somebody is actually in view. Room for it came out of the 32K: the
  palette moved to attic RAM and the loader's bounce buffer went from 2048
  bytes to 512. **Watch out:** reading the palette with `load_far` hangs the
  machine and no run explained why — see `CLAUDE.md` before touching
  `src/loader.c`.
- **The heading readout reads against the map.** `DEGREES` rotates by a
  quarter turn before converting, because angle 0 flies east while the
  overview is north up; it used to call east 000 and north 270. Both bearings
  on the panel go through the one macro, so heading and wind stay in the same
  frame as each other.
- **Battery**, 8.8 percent on the panel, drained per frame by speed mode so
  sport empties it in about a quarter of the time. Flat ends the flight. Found
  a Calypsi sign-extension trap on the way — see `DEGREES` in `panel.c`.
- **Wind.** A direction and strength picked at launch from a 16-bit xorshift,
  added to the position every frame, veering a little every eight seconds, and
  reported on the panel by the direction it comes from. It is applied before
  the ground check, so it can blow you into a hill.
- **Crashing.** Sport mode has no terrain following: the ground clamp in `fly`
  now also reports contact, and in mode 3 that ends the flight with the drone
  destroyed. Modes 1 and 2 hold you off the hill as they always did. Arming
  sport says so on the panel.
- **`make release`**: a `PROFILE=0 WIDE=0 FLYNOW=0` disk copied to
  `release/sar-latest.d81`, so a handout is one command and always the same
  build.
- **Mission two, "First Aid"**, and with it the shape the rest of the missions
  take: they are the same flight with different words on it. The mission table
  carries a `cargo` field and everything else follows from it — an empty bay
  means `SPACE` files a report and a miss costs nothing; a full one means
  `RETURN` opens it, needs five cells rather than ten and no camera at all, and
  a miss ends the flight. Three endings, one debrief page. `RUN/STOP` abandons
  a flight or backs out of a briefing, and the mission list moves on `W`/`S`.
  The second figure came out of the same `convmap.py` pipeline; both sheets are
  now quantised together against one shared pool of free palette entries, of
  which **12 are left**, and that is what limits a third. Figures live in bank
  1 and `sprite_select` DMAs the flight's own into the near buffer. To pay for
  the code, the renderer's three per-ray tables moved to the 2304 bytes of
  chip RAM at `$1600-$1EFF` that the linker rules hand to an unused section —
  `WIDE=1` does not link without it.
- **Mission one**: title, mission list, briefing, flight, debrief, and the
  drone controls a real one has (WASD, RF, QE gimbal, 123 speed, `SPACE` to
  report). Full-screen text pages cost no pixels — the display picks text or
  full colour per character *number*, so a page is a rewrite of screen RAM.
  The screen tables moved to bank 5 to pay for the code. **Watch the startup
  order:** resources must load before `profile_init` (it takes CIA2's timers,
  which the Kernal needs for the disk) and before `vic4_init` (same effect,
  cause not isolated), which is why the loading bar is printed on the ROM's
  screen and why nothing may `printf` once the display is up.
