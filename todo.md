# What's next

## Where it is

Two missions, end to end: title, mission list, briefing, flight, debrief —
**and each over its own generated world**, both resident at once on one disk.
Mission one finds the lost hiker on the step pyramid of the island at 46.687N
8.106E — get within ten cells with them on screen and press `SPACE`. Mission
two drops an EpiPen to a pair of hikers by a lake on the plains at 46.522N
8.081E — get within five cells and press `RETURN`. The briefing gives the fix
and the panel gives your own.

A flight can end four ways. The job done; the one EpiPen released in the wrong
place; the drone flown into a hill, which only sport mode allows; or the
battery flat, which takes about four minutes at normal speed. `RUN/STOP`
abandons one at any time. All four are the same debrief page with different
words on it, the same way the two missions are the same flight.

Three things happen to the drone whatever the pilot does: the wind blows it
about and veers every few seconds, the battery runs down faster in sport, and
mission two flies in rain under an overcast sky.

Underneath: a 320x152 3D view over a six-row 40-column text panel with an
overview map, 12.5 fps (`PROFILE=0`, default map sizes; the rain costs 0.68 ms
of that on mission two). The march is 160 rays; each fills the two pixels it
owns.

Build knobs, all in the Makefile:

| | |
|---|---|
| `PROFILE=0` | no per-column instrumentation; use it for timing |
| `FLYNOW=n` | skip the menus and fly mission n; a headless run needs it to render anything at all, since it cannot press a key, and `FLYNOW=2` is the only way one reaches the second map |
| `REPORT=n` | hold the startup benchmark report n seconds instead of 20 |
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

**And the game layer is at a natural stopping point too**, called on 12 Aug
2026: two missions, four ways for a flight to end, wind, battery and weather,
all of it driven from the mission table rather than from branches. The shape
is proven — a third mission is data plus a sprite sheet. What is actually
scarce now is not ideas but *memory*: about 800 bytes of the 32K and 80 of the
low free RAM, with the easy reclaims already spent. Read the Open note on
`cstack` before starting anything large.

- **The keyboard controls are finished.** Called on 13 Aug 2026, flying them:
  WASD, RF, QE and the three speed modes are the drone's and they feel right.
  An earlier entry here wanted a rethink — long push against short push, thrust
  scaled by frame time — and that is **withdrawn**, not deferred. Two things
  survive it, both in Open and both later: a joystick *beside* the keyboard,
  and speed changes that accelerate and brake rather than snapping.
- More of the game. `documentation/vision.md` has the design; the two
  missions are the first pieces of it. A third is a table entry and a sprite
  sheet, and there is now room for it: generated maps under `--shared` leave
  **32 palette entries** free where the hand-drawn one left 12, so two more
  figures fit at fifteen colours each.
- Both missions still start the drone in the middle of the map, which on the
  plains is a long way from the lake and past the draw distance. A launch
  point per mission, or a heading cue, would save a lot of flying on a
  compass — and it is a `mission.bin` field when that exists.
- The sprite draw is C at ~170 cycles a pixel. Harmless at any distance you
  would search from, 9% of a frame nose to nose. Assembly when there is a
  reason, not before.
- One billboard, one depth. Several at different depths need either a clip
  step per sprite (another pass of the march) or a real per-column depth
  buffer. Sorting them and snapshotting at the nearest is probably enough.
- **A third map, or a third mission.** Both are cheap now: a map is a YAML
  file, a line in the Makefile's `MAP_YAMLS` and `MAP_COUNT` in `loader.h`
  (there is a spare 2 MB attic slot and 270 KB spare on the disk), and a
  mission is a table entry naming one. What is *not* cheap is a third figure —
  see the palette budget in Resources.
- **On-device map generation, step 2: the C port.** Step 1 is done —
  `tools/genmap.py` is integers now (`tools/fixed.py` is the arithmetic), so
  what it computes is what a 45GS02 would. Next is a stage-one PRG that
  generates one 512x512 field into attic RAM, times itself with `src/profile.c`
  and prints a checksum the PC tool can match: that one experiment settles the
  dominant term of the estimate and proves the two-stage boot at once.
  `documentation/on-device-maps.md` has the costing, the measured budget it has
  to beat, and the traps the rewrite turned up.
- **Procedural maps, stage three.** The generator and the previewer are both
  done, and the items that are terrain are built (see Done). Next is
  **`mission.bin`, and it is a different file from `map.bin`** — a mission has
  a map and is not one, so the map side (seed, shape, terrain items) is what
  the generator reads and the mission side (which map, the target, the figure,
  the cargo, the words) is what the game reads instead of the hardcoded table
  in `src/mission.c`. The previewer's `M` key fills in positions for either,
  and `documentation/procedural-maps.md` has both structs written out. The old
  open question there — sprites taking palette indices below 16 — is settled:
  `convmap.py --shared` reserves them.

## Decisions already settled

Do not re-litigate these without new measurements.

- **Detail per cycle lives in the map, not in the raster.** Marching 320 rays
  doubles the march and halves the frame rate for a picture that barely
  changes; a finer colourmap is free and plainly better. The 40-column panel
  used to be its argument and is no longer: the framebuffer is 320 wide with a
  160-ray march. `WIDE=1` is now **retired** — the memory it wanted went to
  the plane tables instead — and reviving it means finding a kilobyte of near
  RAM first.
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
- **A joystick, in parallel with the keyboard and not instead of it.** The
  stick takes what WASD does — forward, back and yaw — and `FIRE` takes the
  mission's own action, which the code already knows how to name: an empty bay
  means `SPACE` files a report and a full one means `RETURN` opens it, so
  `FIRE` is one more caller of `mission_action_key` rather than a second rule
  about what buttons do. What it does *not* cover is the rest of the panel —
  climb, gimbal, the speed modes and `RUN/STOP` stay on the keys. Both inputs
  live at once, so `input.c` returns the union of the two rather than choosing
  between them; the port reads at `$DC00`/`$DC01`, which is the same CIA1 the
  matrix scan already talks to — which is the trap as well as the convenience:
  the joystick lines *are* the keyboard's, so port 2 sits on the row select
  `input.c` writes and port 1 on the columns it reads. Scan the matrix and the
  stick in one pass, with the rows driven high for the stick read, rather than
  bolting a second reader on beside the first.
- **Speed changes snap.** Picking a speed mode, or pushing and releasing `W`,
  moves the drone to that speed the same frame. Ramping towards the new speed
  and braking off the old would read better — a real drone leans into a
  change. Low priority: the controls themselves are settled, this is polish on
  one number.
- The gimbal shears the picture rather than rotating it, because moving
  `cam->horizon` is a shear. Fine at the tilts flying uses; it will look wrong
  long before straight down.
- The panel is nearly full now: message, ALT/HDG, LAT/LON, FPS/SPD/BATT and
  CARGO/WIND, with the overview map on the right.
- **About 800 bytes of the 32K are left, and the low free RAM at $1600 is
  96.5% full** (80 bytes). The easy reclaims are spent: the staging buffers
  are merged and everything movable is already at $1600. The next lever is
  `cstack`, which is 4096 bytes on a program with no recursion and shallow
  calls — worth measuring with a canary before trusting a smaller one.
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

- **Two maps on one disk, one per mission.** Mission one is flown over
  `maps/island.yaml` and mission two over `maps/plains.yaml`, both generated,
  both resident in attic RAM at once. **This was impossible with a drawn map**:
  the hand-drawn pair is 661 KB crunched and two generated maps are 487 KB, so
  a disk that held one world now holds two with 270 KB spare — and loading
  both takes about twenty seconds, less than the one drawn map took. Switching
  is 512 bytes of plane table, because that is where a map's location lives;
  `map_use()` adds the palette its climate wants and the panel's overview.
  `convmap.py --shared` is what makes it work at all: without it each map hands
  the sprites different palette entries and a figure changes colour with the
  mission. The hand-drawn pair is no longer built into anything.
- **The generator is written in the machine's own arithmetic.** Q0.16 integers,
  reciprocals for divides, tables for sqrt/tanh/gamma, histograms instead of
  sorts, xorshift instead of numpy — `tools/fixed.py`, whose self-test prices
  each routine against the float it replaced (worst 0.007 of a height unit).
  Step 1 of the on-device port, done where a mistake is cheap; the float
  version agreed with it to 0.03 of a height unit and one ramp step. Every map
  re-rolled as a result, so the fixes and the reference screenshot moved.
- **A mission has a map and is not one.** The map files in `maps/` describe
  worlds and nothing else; what is flown over them is the C table in
  `src/mission.c`, and the two will become `map.bin` and `mission.bin` rather
  than the one file the design used to describe.
- **The previewer, `tools/preview.py`.** Flies a generated map on the PC with
  the game's own renderer — the same march, projection, map sampling, sky and
  flight model, at the same 12.5 fps, because every rate in the flight model is
  per frame. Its constants are **read out of the C source** at startup rather
  than copied, so it cannot drift; checked against a xemu screenshot at the
  same camera, **every one of the 48336 palette indices is identical**, and
  `tools/checkview.py` is that check as a four-second command with no emulator
  in it — **run it after touching the renderer**. `M` marks a position in the form
  the map file wants, which is where item coordinates will come from; items
  already in the file are drawn as pins clipped against the march's own y
  buffer. tkinter and Pillow, no new dependency. Nothing in `src/` changed.
- **Landmarks are terrain.** A `pyramid` in a mission's `items` is terraformed
  into both maps by `genmap.py` — a square stack of terraces, the site levelled
  to the median of what it covers, heights to roof height and colours to the
  palette's `masonry` band, lit by the same sun as the ground it stands on. It
  costs the renderer nothing, because there is nothing there but ground.
  `size` is `small|medium|large`, cut around the one in `C1W.png`; the island
  has a medium one at 422,481. What governs the shape is that **a terrace
  narrower than a map cell is not there** — the heightmap ships averaged to 512
  and the march samples a cell at most ranges — so terraces are two cells, one
  of riser and one of tread, and the riser wears the lighter course because a
  stepped face from the air is nearly edge on.
- **Sunlight and scale in the generator.** Generated terrain is lit by a sun
  due west and on the horizon, which is what `resources/C1W.png` measures as —
  its luminance correlates 0.69 with the east-west gradient and 0.04 with the
  north-south one. The land ramp is two dimensional now, 21 elevation steps of
  6 shades, and a shade is how fast the ground falls *towards* the sun rather
  than a Lambert term against a normal: that was tried first and could not suit
  a mountain range and a plain with one constant. `scale: near|medium|distant`
  is the second half of it — how big the country is, not how much of it there
  is — and it moves every length the generator knows, including the steepness
  the rock colouring and the sun are judged against, since doubling a
  landform's width halves its slopes. A generated map now spends about 130
  palette entries, leaving around 60 for figures.
- **Procedural map generation, stage one.** `tools/genmap.py` turns a mission
  YAML — type, climate, ruggedness, rivers/hills/lakes and their sizes — into
  the same height and colour PNGs `convmap.py` already eats, reproducibly from
  one seeded stream, with `maps/palette.yaml` as the shared index ramp both
  sides will read. Nothing generated is on the disk yet; the build still ships
  the hand-drawn pair. Rivers took a second pass after the first flight over
  one: measured against the live map a channel digs itself a canyon, ends up
  below sea level and comes out as a black line — they are cut against a
  pristine copy now, meander by choosing between neighbours that already run
  downhill, and end in a pool where the slope pits out. Worth knowing: a
  generated map uses 48 palette entries
  against the hand-drawn one's ~170, which leaves 143 free rather than 12 —
  the budget that limits the game to two figures is a property of that
  colourmap, not of the engine.
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
- 320-wide rendering behind `WIDE=1`, since retired — see Weather below.
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
- **Weather**, per mission: an overcast sky and rain over the 3D view, mission
  two. The sky is sixteen palette entries and costs nothing per frame; the
  rain is 48 slanted streaks drawn after the billboard, 0.68 ms. A clear sky
  is restored out of the loaded palette rather than recomputed, so it is
  bit-identical to what shipped.
- **`WIDE=1` retired** to pay for it: the renderer's plane lookups moved into
  the low free RAM at $1600, which only has room for them at 160 rays. The
  march still has the code and `loader.h` `#error`s with what would have to
  move to bring it back. 320 rays was already a settled dead end below.
- **A 768-byte read into a 512-byte buffer** in the loader, live since the
  bounce buffer was cut from 2048. Fixed by merging it with the sprite's
  drawing buffer, which the two never need at the same time -- and that
  reclaimed 512 bytes as well.
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
  chip RAM at `$1600-$1EFF` that the linker rules hand to an unused section.
  That was the first thing to go there; the weather entry above spent the
  rest.
- **Mission one**: title, mission list, briefing, flight, debrief, and the
  drone controls a real one has (WASD, RF, QE gimbal, 123 speed, `SPACE` to
  report). Full-screen text pages cost no pixels — the display picks text or
  full colour per character *number*, so a page is a rewrite of screen RAM.
  The screen tables moved to bank 5 to pay for the code. **Watch the startup
  order:** resources must load before `profile_init` (it takes CIA2's timers,
  which the Kernal needs for the disk) and before `vic4_init` (same effect,
  cause not isolated), which is why the loading bar is printed on the ROM's
  screen and why nothing may `printf` once the display is up.
