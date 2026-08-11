# Procedural map generation — design summary

Status: **stages one and two are built** — `tools/genmap.py` turns a mission
YAML into a height/colour map pair, and `tools/preview.py` flies it on the PC
with the game's own renderer. `maps/` holds the palette definition and three
example missions. `mission.bin` is not written yet, and nothing generated is on
the disk: the build still ships the hand-drawn pair in `resources/`. See "As
built" at the end for what the tools actually do and what was learned making
them.

This covers **build-time map generation only**. Runtime (on-device) generation
is an explicit future possibility, not part of this phase — see "Out of scope"
below.

## Problem

Height/colour maps are large; even compressed, a d81 realistically fits one
map pair. That gives no mission variety. Hand-authoring more maps doesn't
scale. Solution: generate maps from a small YAML description instead of
drawing/sourcing them.

## Pipeline

```
mission.yaml → tools/genmap.py → height map (indices), colour map (indices), mission.bin
                                        │
                              (same file, dev mode)
                                        ▼
                          Python fly/inspect viewer, Linux/Windows,
                          built on the existing VoxelSpace reference code
                          linked from the README
                                        │
                                   (iterate here)
                                        ▼
                     tools/convmap.py (unchanged) → exomizer → d81
                                        │
                                   one xemu pass to confirm
                                   on the real renderer/palette
```

`genmap.py` is new. Everything downstream of it (`convmap.py`, exomizer,
build) is unchanged — the generator just needs to emit the same map shapes
`convmap.py` already consumes.

## Iteration workflow

1. Edit `mission.yaml` (id, seed, type, climate, ruggedness,
   rivers/hills/lakes and their size variants).
2. Run `genmap.py` → regenerates maps, previewer picks them up.
3. Fly around in the Python previewer, keep or reroll the seed.
4. Once terrain is good, fly again and note item coordinates — bind a key
   in the previewer to print/append the current position (grid x,y and/or
   derived lat/lon) so coordinates can be copied straight into the YAML.
   No auto-placement/snapping logic — items go exactly where specified.
5. Add items to the YAML, rerun, fly again with item markers rendered in
   the previewer to confirm placement.
6. Feed to `convmap.py` → build → confirm once in xemu.

No live editing inside the viewer (no in-tool terrain painting/undo/state
sync) — regenerating from YAML each time is fast enough and much simpler.

## YAML schema (draft)

```yaml
general:
  id: 3                 # required. map index — see below
  seed: 12345          # drives every RNG draw; same seed+yaml = same output
  type: island          # island | flatlands | mountains
  climate: temperate     # hot | temperate | cold
  rivers: few            # none | few | many
  river-size: small      # small | large
  hills: few             # none | few | many
  hills-size: small      # small | large
  lakes: few             # none | few | many
  lakes-size: small      # small | large
  ruggedness: rolling    # smooth | rolling | rugged | jagged

items:
  - type: pyramid
    x: 580
    y: 345
  - type: house
    x: 128
    y: 67
```

Coordinates as placed by hand while flying in the previewer — no need to
also support lat/lon in the file, but the previewer should be able to
print/convert both so notes taken while flying don't need manual grid math.

Note the `<feature>` / `<feature>-size` pattern is uniform across all
three: `rivers`/`river-size`, `hills`/`hills-size`, `lakes`/`lakes-size` —
the first controls how many, the second how big each one is. Same shape,
same parsing code, for all three feature types.

### `id` — required

Every `mission.yaml` declares its own map index (e.g. `id: 3`). This one
field does three jobs at once:

- **File naming.** `genmap.py` names its output from `id` —
  `hmap03.png`/`cmap03.png` (matching whatever zero-padding convention
  `convmap.py` already expects) — so filenames are derived, never
  hand-typed or tracked separately from the YAML.
- **Mission reference.** Mission configs refer to a map by this index
  ("use map 3") rather than by filename or seed, giving a stable handle
  that doesn't change if the map is regenerated with a different seed.
- **Attic RAM addressing.** Once maps are resident in attic RAM (whether
  loaded from disk today, or generated in place later per the two-stage
  boot idea above), `id` gives a direct, computable offset — `id * stride`
  — rather than needing a lookup table to find where a given map's data
  lives. Worth choosing a fixed per-map attic RAM stride up front (size of
  the largest map layout you expect) so the arithmetic stays this simple
  even before every slot is used.

Validate at generation time that `id` is unique across all mission YAMLs
in the project — a collision would silently overwrite another map's files
and attic RAM slot.

## Terrain generation notes

- Single seeded RNG stream per map (e.g. `numpy.random.default_rng(seed)`)
  drives noise, river paths, everything — full reproducibility from the
  YAML alone, nothing else needed to regenerate byte-identical output.
- `type` sets the macro elevation function:
  - island: radial falloff mask from centre + fractal noise (coastline)
  - mountains: ridged multifractal noise, higher amplitude/frequency
  - flatlands: low-amplitude, low-frequency noise only
- `hills`/`lakes` layer local perturbations on top of the macro shape
  (bump noise for hills, flood-filled basins at local minima for lakes),
  each sized by its own `-size` field the same way `river-size` scales
  rivers.
- `rivers` trace steepest-descent paths from seeded high points down to
  sea level or a lake; carve the height map and paint the colour map along
  the path, width driven by `river-size`.
- `climate` selects which palette is used (see below) — it does not need
  a different height algorithm, though cold could bias toward exposed
  rock/snow at altitude if wanted later.
- `ruggedness` is a modifier on top of `type`'s base shape, not a
  replacement for it — it controls the noise octave count/amplitude
  layered onto whatever macro shape `type` already produced (a `rugged
  island` is still island-shaped, just with a noisier coastline and
  interior; a `smooth mountains` is still tall, just with gentler slopes
  between peaks). Named steps (smooth/rolling/rugged/jagged) rather than
  a raw number, mapped internally to fixed noise parameters — keeps
  authoring consistent with the other none/few/many-style fields and
  avoids the "what does 0.6 even feel like" problem of a bare float, at
  the cost of only four discrete steps. Worth revisiting as a numeric
  0–1 knob later if four steps turns out too coarse in practice, but
  named is the easier starting point.

## Palette architecture

- Global, fixed, shared across every generated map: up to 256 colours,
  system reserves the first 16 (leave untouched). Roughly 220 indices free
  after existing reservations (HUD, survivor sprite, etc.).
- `genmap.py` writes **palette indices directly** into the colour map — no
  RGB-to-nearest-colour quantization step, since the palette is known
  ahead of time (elevation band / slope / water proximity → index).
- One shared elevation/biome index ramp (e.g. indices 16–31 = elevation
  bands 0–15) reused across climates, rather than splitting the index
  range per climate. The *RGB behind those indices* is climate-dependent.
- All three (or however many) climate palettes preload into attic RAM at
  boot alongside the maps — cheap, attic RAM reads are a flat +16 cycles
  and total palette data is on the order of a couple of KB. At mission
  load, DMA-copy the chosen climate's palette from attic RAM down to chip
  RAM. Cost is negligible against measured numbers (this is a one-time
  per-mission-load copy, not per-frame).
- One shared palette definition file is the source of truth for both
  sides: the Python previewer reads it to render exactly what will appear
  on hardware, and the same file drives whatever gets baked into the d81
  palette resource. No drift between preview and hardware appearance.
- **Watch out:** `CLAUDE.md` already documents that reading the palette
  via `load_far` hung the machine when the survivor sprite's palette was
  loaded from attic RAM, cause unexplained. The attic→chip palette copy
  for climate switching touches the same territory — test this path
  early rather than assuming it's fine.

## mission.bin — pre-parsed struct

Alongside the height/colour maps, `genmap.py` emits a small fixed-width
binary file the game reads instead of hardcoded constants:

- seed
- type / climate (enums)
- item count, then `{item_type, x, y}` per item (actual placed
  coordinates — these came straight from the YAML in this phase, since
  placement is manual, not auto-snapped)
- survivor position, player start position

This is a real win now (data-driven missions instead of hardcoded
coordinates like the current `46.713N 8.110E`), and it's deliberately the
interface a future on-device generator would need to produce — if
generation ever moves onto the MEGA65, only what *writes* `mission.bin`
changes, not what reads it. The MEGA65 side never needs to know or care
whether the struct came from a Python script or on-device generation.

## Keep in mind for future portability

Not building this now, but the build-time tool should be written so it
doesn't foreclose it:

- **Algorithm portability.** The Python generator can use numpy for
  convenience/speed on the PC, but the *algorithms themselves* should stay
  simple enough to hand-port to C or 6502/45GS02 assembly later — integer
  or fixed-point-friendly noise (e.g. hash-based value noise on a lattice
  with simple interpolation) rather than anything that leans on a specific
  library's gradient tables or float-heavy math with no obvious fixed-point
  equivalent. No large external dependency should end up load-bearing for
  the actual generation logic — numpy/Pillow are fine as PC tooling
  convenience, but if a step can only be expressed by calling into a big
  library's internals, that's a step that'll need to be redesigned before
  it can move on-device anyway, so better to keep it simple from the start.
  Keep one Python function mapping to one eventual C/asm routine where
  practical, so porting later is translation, not redesign.

- **Two-stage boot for the on-device case.** If/when generation moves to
  the MEGA65, the natural shape is: `autoboot.c65` becomes a first-stage
  generator program — reads `mission.bin`-style parameters, generates
  height/colour/palette data into attic RAM, then loads and runs `game.prg`
  (which is most of what `autoboot.c65` is today). Because the generator
  and the game never need to be resident at the same time, the generator
  stage can afford to be a fairly large program without competing with the
  game binary for space — it does its job once, then gets replaced.
  `mission.bin` already being the interface between "whatever produced the
  maps" and "the game that reads them" (see above) means this later split
  doesn't require rethinking that interface, only adding a program that
  produces attic RAM contents directly instead of reading them from a
  decrunched disk image.

## Out of scope (this phase)

- On-device / runtime procedural generation itself. Possible future step
  if the build-time tool proves out and terrain variety turns out to
  matter for the game — deferred, not designed yet, but see above for
  what to keep in mind now so it stays feasible later.
- Live/interactive map editing inside the Python previewer.
- Automatic item placement/snapping (flat-ground search, biome
  constraints) — items are placed manually by eye while flying.
- Separating world/terrain definition from mission/item definition into
  separate YAML files — starting with one `mission.yaml` per mission;
  revisit only if that seam becomes annoying in practice.

## As built (stage one)

```sh
python3 tools/genmap.py maps/island.yaml        # -> maps/hmap03.png, maps/cmap03.png
python3 tools/convmap.py maps/hmap03.png maps/cmap03.png \
    resources/survivor-sprite.png maps/gen 512 1024
```

`genmap.py` writes exactly the two shapes `convmap.py` already reads — an 8-bit
greyscale PNG of heights and a mode-P PNG of palette indices — so the second
command is the unchanged existing converter, and the only thing between a YAML
file and a disk is the Makefile still naming `resources/D1.png`. The generated
maps are build products and are not in git; the YAML and the palette are.

Everything in the schema above is implemented as written, plus a `--size`
(default 1024, powers of two) and a `--palette`. `items` is parsed and
validated but goes nowhere yet.

### What the numbers mean

Heights come out in 0..120 of the available 255, matching the hand-drawn
heightmap's 0..118, so generated terrain flies with the same altitudes and the
same `GROUND_GAP` the flight model was tuned against.

The palette is `maps/palette.yaml`, and the ramp is the whole of the colour
decision: water at 16..23 shaded by depth, then **one contiguous 40-step land
ramp at 24..63** — shore, lowland, highland, peak — indexed by elevation, plus
a push for slope and a coarse noise dither. Where the band boundaries fall is
a matter of what colours the climate puts there and nothing the code knows
about. Two consequences worth keeping:

- the ramp is normalised to each map's own relief, or a flatland would come out
  one flat colour; that alone would crown *every* map with a bare summit, so
  each `type` also declares a **ceiling** — how far up the ramp its high ground
  is allowed to reach. Mountains earn the peak bands, a plain tops out in the
  greens.
- a generated map uses **48 palette entries against the hand-drawn one's ~170**,
  which leaves `convmap.py` 143 free rather than 12. The palette budget that
  currently limits the game to two figures is a property of the *hand-drawn*
  colourmap, not of the engine.

### Traps found on the way

- **A basin flood will happily flood the ocean.** The priority flood grows into
  its lowest neighbour, so a lake near the coast reaches the sea, finds it the
  lowest thing available, and spends its whole area budget painting ocean at
  the lake's surface level. Standing water is not somewhere a basin can grow.
- **A river arriving at the coast is still well above sea level**, and stamping
  its channel over the sea raises a plateau of water out in the bay. A river
  carries the water mask as it stood when it set out: that is both where it
  stops and where it may not write.
- **Fold the ridged noise once at the end, not per octave.** The textbook
  ridged multifractal creases along every lattice's own mid-contour, and the
  ridges come out boxy and grid-aligned. Folding the finished sum creases along
  a contour of the field instead, and the ridges are long and sinuous. Sampling
  each octave at its own offset matters for the same reason.
- **Stretch the octave sum to 0..1 before using it.** A sum of octaves is a
  bell around the middle: a `range` of 0.8 delivers about half of that, and
  almost every pixel sits near 0.5 — which is exactly where the fold above puts
  its crease, so an unstretched ridged map is nearly all ridge.
- Steepest descent down a smooth field is a **straight line**. Rivers follow a
  blurred copy of the terrain (the raw surface has a noise pit every few pixels
  and stops a walk immediately) with a little noise on top, which bends the
  path where the ground is flat and leaves it alone where it is steep.
- Colour dither at one-pixel frequency is not texture, it is **salt and pepper
  along every band boundary**. A lattice cell every four map cells reads as
  patches of vegetation instead.

## As built (stage two): the previewer

```sh
python3 tools/preview.py maps/island.yaml          # fly it
python3 tools/preview.py maps/island.yaml --shot out.png --at 137,117,0,162
```

`W`/`S`/`A`/`D`, `R`/`F`, `Q`/`E` and `1`/`2`/`3` are the game's controls;
`M` marks the current position, `L` reloads after a rerun of `genmap.py`, `ESC`
quits. tkinter and Pillow, no new dependency beyond `python3-tkinter`.

**It is the game's renderer, not a lookalike.** The band schedule, the position
update in 8.8 with its 16-bit wrap, the biased horizon, the y buffer, the map
sampling, the sky, the flight model and the panel readouts are all the ones in
`src/` — the hardware walks a column at a time and this walks a *step* at a
time across all 160 rays, which is the same front-to-back order with the loops
exchanged. Maps are read through `convmap.py`'s own loaders, so what is flown
is what the disk would carry, down to the box-averaged heightmap and the sky
gradient's palette entries.

**The constants are read out of the C source at startup, not copied.**
`C_DEFINES` names the `#define`s and the file each lives in, and the sine table
and the speed limits are parsed out too; if one moves or becomes an expression
the previewer exits saying so rather than quietly flying a different game. That
is the whole difference between a tool that stays honest and one that drifts a
release later.

**Checked against the machine, not by eye, and the check is checked in.** The
panel gives the camera exactly — `LAT`/`LON` are the cell, `ALT` the height,
`HDG` the angle — so a screenshot from xemu can be reproduced here, and at the
same camera **every one of the 48336 palette indices is identical**. The
sub-cell fraction the panel cannot report is recovered by search.

```sh
python3 tools/genmap.py maps/island.yaml    # the maps are not in git
python3 tools/checkview.py                  # 4 seconds, no emulator
```

`tools/checkview.py` is that comparison as a command, against a reference
screenshot in `documentation/reference/` whose filename carries the camera.
**Run it after touching the renderer.** The previewer is a second
implementation of `src/voxel_asm.s`, and a second implementation is a liability
the day somebody changes one and not the other — it would go on drawing a
confident picture of a renderer that no longer exists. Reading the constants
out of the C source covers the numbers; this covers the algorithm. Both were
tested by breaking them on purpose: `SCALE_H` 25→26 moves 13% of the pixels and
a half-step error in the march moves 2.9%, and the check fails on either.

A deliberate change to the renderer makes the *reference* stale rather than the
previewer wrong. `checkview.py`'s header has the three commands for taking a
new one, since there is still no Makefile knob for building a disk from a
generated map.

Worth knowing:

- **it runs at 12.5 frames a second on purpose.** Every rate in the flight
  model is per *frame*, so a preview at 60 fps would be a drone flying five
  times as fast. The frame's own cost comes off the wait. Rendering is 4 ms.
- **no wind and nothing crashes.** The wind would blow you off a spot while you
  were writing it down; the ground clamp is there but sport mode's crash is
  not. This is an inspection tool.
- `M` prints the position as a YAML item block and appends it to
  `<mission>.marks.yaml`, so nothing is lost to a scrolled terminal. Item
  coordinates are **map pixels at the generated size** — a quarter of a cell at
  1024, which converts to the game's 8.8 position exactly.
- items already in the YAML are drawn as pins, projected with `sprite.c`'s own
  numbers and clipped against the march's y buffer at their depth — so the
  question "can this be seen from the air" is answered by the same arithmetic
  that will answer it on the MEGA65.

### Next, in order

1. `mission.bin`, and with it the item coordinates the previewer collects.
2. A Makefile knob for which map pair the disk is built from. Note that
   `convmap.py` hands sprites any free index including 3..15, which the design
   reserves; worth settling before a generated map ships.
