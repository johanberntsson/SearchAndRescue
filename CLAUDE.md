# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

A MEGA65 heightfield voxel flight simulator / drone search-and-rescue game, written in C (Calypsi) with the rendering inner loop in 45GS02 assembly. `documentation/vision.md` holds the full technical and gameplay design; `todo.md` is the authoritative "what's next" and should be updated as work lands.

Currently: two missions, end to end, **each over its own generated world**. A
title screen, a mission list, a briefing, a flight, and a debrief — the lost
hiker on the step pyramid of the island at 46.687N 8.106E to be found and
reported, and an EpiPen to be dropped to a pair of hikers by a lake on the
plains at 46.658N 8.149E. A flight also carries a wind that blows the drone
about, a battery that runs it out, and per-mission weather; it can end four
ways, all of them the same debrief page with different words on it.

**Both maps are generated from a paragraph of YAML and both are resident at
once**, which one hand-drawn map pair could never be: two generated maps are
487 KB crunched against 661 KB for one drawn one, and switching between them
costs 512 bytes of plane table. See Resources.

Underneath is the voxel engine at about 12.5 fps — a 320x152 3D view over a
six-row 40-column text panel, with 512x512 height and 1024x1024 colour maps
unpacked into attic RAM at boot, marching 160 rays and writing each to two
neighbouring pixels; see Performance.

## Build and run

**The disk carries two programs now.** `AUTOBOOT.C65` is *stage one*
(`src/mapgen/`), which prepares attic RAM and then hands the machine to `SAR`,
the game. See The two-stage boot.

```sh
make run          # build build/sar.d81 and boot it in xemu
make prg          # skip the disk and stage one, run the game PRG directly
make PROFILE=0    # without the per-column instrumentation; use this for timing
make FLYNOW=1     # skip the title and menus, launch straight into mission 1
make FLYNOW=2     # ... or mission 2, the only headless way to reach its map
make REPORT=n     # hold the startup benchmark report n seconds, not 20
make HGT_SIZE=1024 COL_SIZE=512     # map resolutions, 256..1024
make release      # PROFILE=0 disk, copied to release/sar-latest.d81
make clean
```

`make release` spells `PROFILE=0 FLYNOW=0` out in a sub-make rather
than leaning on the defaults, so the handed-out disk is the same one however
the tree was last built; the map sizes are left to the defaults, because those
*are* the shipping resolution. It shares `build/`, so it and an interactive
build force a rebuild of each other through the config stamp — that is the
stamp working, not waste.

`make run` launches `xemu-xmega65`, a GUI emulator that blocks until closed. For automated checks, run it headless and screenshot on exit:

```sh
make FLYNOW=1
xemu-xmega65 -besure -headless -sleepless \
    -8 build/sar.d81 -screenshot out.png -dumpmem mem.bin &
sleep 75; kill -INT $!          # xemu writes both files as it exits
python3 tools/profread.py mem.bin
```

**`FLYNOW=1` is what makes a headless run measure anything.** The game now
waits for a keypress on the title screen, then two more through the menus, and
nothing headless can press one; without it the dump has no frames in it and
`profread` says so. **`FLYNOW=2` is the same for mission two**, and the only
way a headless run ever reaches the second map.

Budget about 50 seconds before the first frame: roughly twenty of loading —
both maps, 487 KB crunched — and then the startup benchmark report, which
holds for `REPORT_SECONDS` (20 by default, `make REPORT=n` to change it).
**Kill the run by PID and wait for it**, not with a bare `sleep` in the same
command: a wait that races the emulator reads the *previous* run's screenshot,
which looks exactly like a change that did not take.

`timeout -s INT` looks like the way to end the run and mostly is, but xemu can
take several seconds to act on the signal, so a loop that starts the next run
immediately ends up with two emulators writing screenshots over each other.
Backgrounding it and waiting, as above, is what actually serialises.

`-sleepless` is fine here — see Performance for why it must never be used to time anything from the outside.

`-dumpmem` writes 384 KB of **chip RAM only**, so it cannot see attic RAM and cannot see either map at the default sizes; `mem.bin[0x40000]` is the heightmap only when `HGT_SIZE=256` keeps it in chip RAM. `mem.bin[0x10000]` onwards is framebuffer A. Note that a dump catches whichever buffer is mid-render, so it is no good for judging a finished frame — compare screenshots for that. Reading a framebuffer back and de-swizzling it with the column-strip formula below is the fastest way to tell "the renderer is wrong" apart from "the display is wrong"; both have happened. There are no tests and no linter.

## Toolchain

Calypsi 6502 C compiler 5.18, installed system-wide (`/usr/local/bin/cc6502`, `ln6502`; headers, libraries and linker rules under `/usr/local/lib/calypsi-6502-5.18/`). `--target=mega65` selects the 45GS02 core, puts the MEGA65 SDK headers on the include path (`<mega65.h>` gives `VICIV`, `PALETTE`, `CIA1`, `MATH` …) and links the board support library.

**`ln6502 -o` names the ELF output, not the PRG.** The PRG is written alongside it under the same stem, so the Makefile links to `build/sar.elf` and copies the resulting `build/sar.prg` to `build/autoboot.c65`. Linking straight to `autoboot.c65` silently produces an ELF file that the MEGA65 tries to RUN as BASIC.

`mega65-plain.scm` resolves from the toolchain's `linker-rules/` directory, not this repo. It gives the program `$2001-$9FFF` — 32 KB for code, data, stack and heap — and emits a C65 BASIC stub (`SYS 8206`), which is exactly what `autoboot.c65` needs.

Data above 64 K is reached with Calypsi's `__far` pointers. Their index type is `int16_t`, which cannot span a whole 64 K map; `src/voxel.c` biases the base pointer by 32 K and XORs the offset (`MAP_BIAS`) so a signed index covers the map.

## Memory map

Only banks 1, 4 and 5 are free: `$20000-$3FFFF` holds the C65 ROM, and **colour RAM is aliased into chip RAM at `$1F800`**, so bank 1 effectively ends there.

| Address | Size | Contents |
|---|---|---|
| `$2001-$9FFF` | 32 KB | Program, data, stack, load bounce buffer |
| `$10000-$1BDFF` | 48640 | Framebuffer A (320x152, the 3D view) |
| `$50000-$5BDFF` | 48640 | Framebuffer B (bank 5) |
| `$1C000` | 1216 | One column strip of sky, DMAd across the buffer each frame |
| `$1D000` | 1024 | Overview map: 16 full-colour characters, 64-byte aligned |
| `$1D800` | 1024 | Pristine copy of it, for lifting the crosshair |
| `$1DC00` | 1028 each | Every billboard, one 32x32 slot per figure |
| `$1E800` | 1024 each | Every map's overview tile set; the flight's is DMAd to `$1D000` |
| `$1F800-$1FFFF` | 2 KB | Colour RAM alias — **do not write** |
| `$40000-$4FFFF` | 64 KB | Heightmap, only when it is 256x256 |
| `$8000000-$81FFFFF` | 2 MB | Map slot 0: colourmap planes, then heightmap |
| `$8200000-$83FFFFF` | 2 MB | Map slot 1 — mission two's world |
| `$8400000-$85FFFFF` | 2 MB | Map slot 2, spare |
| `$8600000-` | | Staging for the crunched stream being unpacked |
| `$5C000`, `$5D000` | 2000 each | The two screen tables |
| `$8700000` | 768 each | One palette per map slot, until a flight uploads its own |

**The 32 KB is `mega65-plain.scm`'s choice, not the machine's.** The stock
linker script gives `program` `$2001-$9FFF` because that is what is safe with
every ROM mapped in. **Bank BASIC out — `$D030` bit 4 — and `$A000-$BFFF` is
ordinary RAM**, which is what `mega65-sar.scm` gives stage one: its `program`
section went from overflowing to 66% used. Three things make it low risk, and
they are the reasons to copy the arrangement rather than invent another:

- **only BSS goes up there.** A PRG is loaded by the ROM with BASIC still
  mapped, so anything above `$9FFF` that must arrive from disk raises a
  question about whether the write falls through to the RAM underneath. `zdata`
  is not in the file. Code and initialised data stay low, and the question
  never comes up.
- **it is chosen per object.** Naming `zdata` in the memory does not place —
  BSS is 11 KB and the window is 8 — so `HIGH_BSS` marks what moves.
- **the Kernal is left alone**, so `printf` still works and the interrupt
  vectors are still the ROM's. No `SEI`, no handler of our own. And **BASIC
  goes back in before the handover**, beside the timers and the zero page.

The register is written in `__low_level_init`: after the startup has set the C
stack pointer and before anything uses it, since a write up there falls through
to RAM but the read back would come from ROM.

**The game has not been banked yet**, and there is a reason it is not a
copy-paste: a PRG cannot have two *content* areas, and naming `zdata` or
`cstack` in a second memory makes one, because the linker rules already give
those sections a `-bits` half. Stage one gets away with it by owning a section
name of its own. The game needs its big BSS marked `HIGH_BSS` before the memory
can be declared; `mega65-game.scm` and `src/bank.s` are waiting for it.

**What the game did get is its stack measured**, which needed no banking at
all: 144 bytes of the toolchain's 4096, so it builds with 512 and went from
**44 bytes free to 3.5 KB**. The number is the *boot* — loading the resources
is a deeper call chain than the renderer, which measures 120.

**The 32 KB fills up fast.** The survivor sprite took it down to about 360
spare bytes; the game screens needed a couple of kilobytes more. Where the
room came from, in the order it was taken:

- the palette to attic RAM (768 bytes) and the loader's bounce buffer from
  2048 down to 512. The smaller chunk costs nothing measurable — the per-call
  overhead of a Kernal read is small against half a kilobyte of copying, and a
  fixed run rendered 17669 frames at 1024 against 17514 at 512.
- **the screen tables to bank 5** (4000 bytes), which is the big one.
  `SCRNPTR` is a 32-bit register, so screen RAM can live anywhere the VIC-IV
  can read, and bank 5 above framebuffer B is 16K of nothing. Everything that
  writes it is cold: the panel's handful of characters a frame, and building
  the tables once.
- **the second billboard never came near it.** Figures live at `SPRITE_STORE`
  in bank 1, a 1028-byte slot each, and `sprite_select` DMAs the flight's own
  down into the single near buffer that was already there. Adding a figure
  costs a kilobyte of bank 1 and nothing of the 32K.
- **the renderer's three per-ray tables to `$1600-$1EFF`** (800 bytes at 160
  rays, 1600 at 320). See the free space note below.

Next after that would be the 512-byte bounce buffer itself, or the sprite's
1028.

**`$1600-$1EFF` is 2304 bytes of ordinary chip RAM the linker rules hand to a
section called `zpsave`, which nothing in this program uses** — the map said
0.0% before anything was put there with `LOW_FREE` (`loader.h`). It is
near-addressable, so the move costs nothing per access, and it is now the only
place a table can go when the 32K runs out. It holds, of 2304 bytes: the three
per-ray tables (800), the sprite's ramps (304), the rain (96) and the
renderer's plane lookups (1024).

Only things the *renderer* owns belong there. It is below `$2001`, where the
C65 ROM keeps its own buffers, so nothing that has to survive a Kernal disk
call may live in it — everything in it is first written after the last
resource has been read.

**This is what retired `WIDE=1`.** At 320 rays the per-ray tables are 1600
bytes rather than 800 and the plane tables no longer fit — and those are the
ones that cannot move, because the inner loop reads them 10240 times a frame
and they have to stay near. Marching 320 rays was already a settled dead end
(half the frame rate for a picture that barely changes), so the kilobyte is
better spent. The march still has the code; `loader.h` `#error`s on `WIDE=1`
with what would have to move to bring it back, rather than failing as an
unexplained `cstack` placement error.

The framebuffer is 320 wide whatever `WIDE` is set to, so a buffer is 48640
bytes and B lives in bank 5. Bank 4 is free too whenever the heightmap is
above 256x256.

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

Two register notes: `CHRXSCL` (`$D05A`) is source pixels per output pixel in
120ths, so **60 doubles the width** and 240 halves it — it is left at 120 now,
but the renderer used to draw 160 pixels and let this stretch them; and the
hot registers (`$D05D` bit 7) must be turned off first or any write to a
legacy VIC-II register makes the VIC-IV recompute the layout and undo the
setup.

**The VIC-IV latches `SCRNPTR`, `LINESTEP`, `CHRCOUNT` and `CHRXSCL` once a
frame.** Writing any of them part way down the screen changes nothing until
the next frame begins, so whichever value was written last simply wins the
whole of the following frame. A raster split that gave the panel its own
geometry was built and measured against this and cannot work: with the two
`CHRXSCL` values swapped the *entire* display changed together, both halves.
Only per-pixel registers such as the border colour answer mid-frame. The
MEGA65's mechanism for per-row geometry is the Raster Rewrite Buffer, which
this does not use. The experiment is kept at
`documentation/experiments/raster-split.patch`, with its own notes beside it:
the interrupt half of it works and is what to start from whenever the game
wants one. Two things learned on the way:

- the raster **compare** is written to `$D012` in VIC-II line numbers.
  `TEXTYPOS` is 104 *physical* rasters and a VIC-II line is two of them, so
  the character display starts at line 52 and each row is 8 lines. The
  VIC-IV's own `$D079`/`$D07A` compare pair never fired at all.
- the C65 ROM's `$0314` dispatcher is **not** the C64's three-register one at
  `$FF48`. It is the 45GS02-aware one at `$FA23`, which does `PHA / PHX / PHY
  / PHZ / TBA / PHA` and so leaves **five** bytes on the stack: A, X, Y, Z and
  the base page register B. A C64-style three-pull exit RTIs onto a
  mismatched frame and the machine is dead after the first interrupt.
  Chaining to the displaced handler instead avoids needing to know that, but
  the ROM's handler reprograms the raster compare to its own line every time.

## The panel

The display is 25 character rows; the framebuffer covers the top 19 and the
bottom six are the information panel: a message line, ALT/HDG, LAT/LON,
FPS/SPD, and the cargo bay on the last row, with the overview map filling the
right-hand four columns of rows 1 to 4. The cargo line reads `CARGO EMPTY` for
the whole of a mission flown with the camera alone. That split is free, because full colour
is per character *number*: `FCLRHI` is set and `FCLRLO` is not, so numbers
above `$FF` are 64-byte full-colour characters (the framebuffer) and numbers
below are ordinary 8x8 text from `CHARPTR`. The panel costs 480 bytes of
screen RAM, no pixel writes, and is not double buffered — `vic4_panel_char`
writes the same cell in both screen tables.

### The overview map and the coordinates

The right-hand four by four characters of the panel are the colourmap scaled
to 32x32, one pixel per eight map cells. It is **not built on the MEGA65**:
`tools/convmap.py` point-samples it out of the (already remapped) colourmap
and writes `terrain.ovr` as sixteen 8x8 tiles in reading order, which is
exactly the layout of sixteen full-colour characters, so `loader.c` reads it
straight to `$1D000` and `panel_init` does nothing but name the character
numbers. Mixing it in among the text is free: the mode is chosen per character
*number*, so a full-colour tile and a letter cost the same screen RAM.

The crosshair is drawn **into the map's own pixels**, in palette 240 — one of
the two entries `convmap.py` reserves for overlays, and a byte per pixel means
any of the 256 will do. Lifting it again is a 1024-byte DMA from a pristine
copy at `$1D800`, about 60 microseconds, rather than remembering what each
pixel used to be. Restoring from saved pixels was tried and corrupted the
whole map within a few hundred frames.

**A hardware sprite for the crosshair does not work here.** The VIC-IV's
`SPRPTRADR` (`$D06C`-`$D06E`) is ignored by xemu with 8-bit pointers and
*segfaults* it outright with `SPR_PTR16`, so the sprite pointer still comes
from the legacy screen+`$3F8` — which in this screen layout is row 12 column
28, in the middle of the 3D view, where the character number is a framebuffer
tile and cannot be given a useful value. Confirmed by dumping memory: the
bitmap was where we put it and the VIC was fetching from somewhere else. The
sprite itself displays fine, so this is worth retrying on real hardware.

Latitude and longitude come out of the position for free, because **one map
cell is defined as one millidegree**: the world is 256 cells square, so it
spans 0.256 degrees, about 28 km. The cell index *is* the fractional part of
the reading and there is no arithmetic beyond an add. `MAP_LAT_SOUTH` and
`MAP_LON_WEST` in `panel.h` are the whole of the (arbitrary) origin, and
latitude counts down the map so that the overview is north up.

The panel is **40 columns**, because the framebuffer above it is a real 320 pixels
and `CHRXSCL` is 1:1 for the whole display. It was 20 stretched ones for as
long as the renderer drew 160 pixels and let the VIC-IV double them, and the
geometry cannot be changed per raster row (see above), so the way to a
readable panel was to widen the framebuffer rather than to split the screen.

Two things had to be set up for it. `CHARPTR` had never been touched, and
whatever the ROM leaves there draws a horizontal line for a space; the C65
ROM's 8x8 set at `$2D000` is readable where it sits, so the panel needs no
RAM for a font. And **a text character's colour comes from colour RAM, which
is only a four-bit field** even in 16-bit character mode — so panel ink has
to be one of the first sixteen palette entries, which is where the terrain
colours live. `tools/convmap.py` reserves 1 and 2 by moving the terrain
colours that were there out to unused slots; palette 0 is the screen colour
and serves as the paper. Palette entries 240 and 241 are `convmap.py`'s
overlay pair, for things drawn *over* a picture rather than sampled out of a
map — and both are taken: 240 is the overview crosshair and the two together
are the rain's near and far depths. A further overlay needs entries out of the
free pool, not these.

## The game

`src/main.c` is a state machine over four full-screen pages and a flight:
title, mission list, briefing, fly, debrief, back to the list. `src/screens.c`
draws the pages, `src/mission.c` holds what there is to be sent on.

**The two missions are the same flight with different words on it** — and, as
of the two-map disk, over different country. The shape is deliberate: fly to a
figure standing at a fix and press a key. The mission table is what differs,
and the one field the rest hangs off is `cargo`:

- **empty bay** — the job is to look. `SPACE` files a report, it needs the
  figure *on screen* and within ten cells, and getting it wrong costs nothing
  but the time to go round again.
- **something in it** — the job is to deliver. `RETURN` opens the bay, it needs
  only to be within five cells (`sprite_in_range`, which does not care where
  the camera points — you deliver a parcel by flying to the spot, not by
  looking at it), and there is one of whatever it is, so releasing it anywhere
  else ends the flight.

`mission_action_key`, `mission_action_name` and `mission_action_verb` all
derive from that field rather than being stored beside it, so a mission cannot
say one thing in the briefing and do another in the air. The three endings —
done, cargo lost, abandoned — are one `screens_debrief` page with different
strings, for the same reason.

**A mission names its map**, and the fix is a cell of *that* map, so the two
travel together in the table and cannot be edited apart: mission one is flown
over `maps/island.yaml` and stands its hiker on the step pyramid there;
mission two over `maps/plains.yaml`, by the largest lake. See Resources for
what a map slot is and what switching costs.

Adding a third mission is a table entry, a sprite sheet in the Makefile's
`SPRITES`, and nothing else — but see Resources for the palette budget, which
is what actually limits how many figures there can be. A third *map* is a YAML
file, a line in `MAP_YAMLS` and `MAP_COUNT`: there is a spare attic slot for
it.

**The pages cost no pixels.** The display picks text or full colour per
character *number*, so writing numbers below `$100` into the rows the 3D view
normally occupies turns them into ordinary characters; `vic4_text_mode` does
that and `vic4_view_mode` puts the framebuffer tiles back. Nothing moves but
screen RAM.

**The startup order is forced, and not by taste.** Resources must be read
before anything else happens, because two separate things leave the Kernal
unable to open a file at all:

- **`profile_init` takes CIA2's two timers** for its clock, and the Kernal
  needs them to talk to a disk.
- **`vic4_init`** does it too. Which register was not worth isolating —
  `CPU_PORTDDR`, `VFAST` and the sprite enable were each ruled out on their
  own run — because loading has to come first for the timer reason anyway.

So the sequence is: loading, then the benchmarks, then the display. That is
why the loading bar is *printed* on the ROM's text screen rather than drawn on
the game's own, and why it only ever grows one block at a time — printing is
the only tool available on a screen there is no cursor addressing for. The
title screen proper comes after `vic4_init`, in the game's own font and
palette. **Once the display is up, nothing may `printf`**: the Kernal's screen
editor writes colour RAM, and the game is using it. `load_resources` reports
failures through `loader_error()` instead.

Controls, which follow a real drone's (see `documentation/real-drones/`):
`W`/`S` forward and back, `A`/`D` yaw, `R`/`F` climb and descend, `Q`/`E`
gimbal up and down, `1`/`2`/`3` the speed limiter (cinematic, normal, sport),
`SPACE` to file a report, `RETURN` to release the cargo, `RUN/STOP` to abandon
the mission.

**Sport mode has no terrain following, and that is the third way to fail.**
`fly` has always clamped the camera to `GROUND_GAP` above the ground; now the
same test also reports contact, and in sport the flight ends there
(`FLIGHT_CRASHED`). A real drone turns its obstacle sensors off in sport too,
so the fastest mode is the one that will fly you into a hill. The clamp is
still applied on the crash frame, so the last picture is the hillside rather
than a view from inside it, and the check is read at the *bottom* of the loop
with the other exits so that frame reaches the screen first. Arming sport puts
`SPORT: NO TERRAIN FOLLOWING` on the panel, which is the warning — the briefing
deliberately does not carry one, because the page has no spare row and the
panel says it at the moment the pilot chooses.

`src/input.c` scans four matrix rows now — row 0 for `RETURN`, row 7 bit 7 for
`RUN/STOP` — and returns held keys and fresh presses from one scan, because an
edge only means anything against the scan before it and two scans in a frame
would see none.

`RUN/STOP` reads the same on the briefing and in the mission list as it does
in the air: this is not the job, take me back.

**The wind is the one thing in the flight model that is not the pilot's.**
`wind_start` picks a direction and a strength at launch from a 16-bit
xorshift seeded off the profiler's clock, `fly` adds the vector every frame
whether the drone is moving or not, and `wind_drift` veers it about eleven
degrees and a step of speed every 96 frames. Three things about it:

- **it is named for where it comes from**, as a weather report and a drone
  controller both are, so the drift is towards `wind_from + 128`. Fly the
  heading the panel says the wind is on and you have a headwind.
- **the m/s readout is a scale, not a conversion.** Sport is 176 cells a frame
  and a cell is a hundred metres, so an honest conversion would print several
  hundred metres a second; `WIND_MPS` maps the internal 3..10 onto 1..5 m/s
  instead. Nothing in this world is to scale — see `SPR_WORLD_H` for the same
  decision about how tall a person is.
- it is applied **before** the ground check, so being blown into a hillside in
  sport mode crashes you exactly as flying into one does.

`voxel_mul_shift8` rather than the C 32-bit multiply the pilot's own motion
uses, because the wind is applied every frame and the compiler's version is
2203 cycles against 85.

**`DEGREES` adds a quarter turn, and that is not decoration.** Angle 0 moves
the camera along +x, which is east, while the overview map is north up — so
printing the raw angle as degrees called east 000 and north 270. The macro
rotates by 64 before converting and masks the wrap in 16 bits. Both readouts
that show a bearing go through it, so the heading and the wind stay in the
same frame as each other: fly the heading the wind is reported on and you have
a headwind.

**The wrap must be a mask, not a cast through `uint8_t`** — see the
sign-extension trap under Performance. Getting that wrong printed a wind of
896 degrees.

**The gimbal is free.** `cam->horizon` was always a field the renderer
rebuilt its per-step horizon table from whenever it moved; tilting is a
frame's worth of table and nothing per pixel.

**The battery is 8.8 percent**, so the figure on the panel is the high byte
and there is no divide in the drain. `battery_drain` is indexed by speed mode
— 7, 10 and 28 a frame, roughly five minutes, four, and a minute and a quarter
at 11.6 fps — because a real drone's sport mode works the props harder
whatever the sticks are doing, not only when you are moving. Flat is
`FLIGHT_FLAT`, tested at the bottom of the loop with the other exits, and the
readout is rewritten only when the figure moves, which at the fastest drain is
every ninth frame.

## The weather

`src/weather.c` owns an overcast sky, the rain, and the one pseudo-random
stream the weather uses (the wind's gusts come out of it too). Which weather a
flight gets is a `weather` field in the mission table, like its cargo and its
figure; mission two rains.

**The sky costs nothing per frame.** `voxel_init` bakes only the *shape* of
the gradient into the template it DMAs across the buffer — `SKY_BASE + y *
SKY_SHADES / FB_HEIGHT` — so the sixteen colours it names are just palette
entries. An overcast sky is sixteen `vic4_set_entry` calls at flight start and
no new palette entries at all.

**A clear sky is restored from the loaded palette, not recomputed.** The blue
comes from `SKY_TOP`/`SKY_HORIZON` in `tools/convmap.py`; a second copy of
those numbers in C would drift from them the first time anybody changed the
sky. `vic4_set_range` puts the sixteen entries back exactly as they shipped —
verified bit-identical against a screenshot from before the weather existed.

**Rain is drawn last, after the billboard, and needs no clearing pass**: the
sky DMA at the top of the next frame repaints every pixel before anything
reads it. 48 drops in four layers, and the layers come from the low bits of
the drop's index rather than from stored fields — speed, length and colour all
from `i & 3`, which cannot get out of step with itself and costs no memory.
Each streak leans one pixel right every second row using the same
strip-crossing step `sprite.c` makes along a figure's width, and spawning is
kept `RAIN_LEAN` columns clear of the right edge because leaning off the last
strip would write past the framebuffer into the sky template at `$1C000`.

State is two bytes a drop in `LOW_FREE` — a ray column and a row. The
framebuffer offset is not stored: `voxel_column_offset` already has it in the
march's own `col_top` table, so a lookup a frame is cheaper than 96 more bytes
of a budget that tight, and it avoids `FB_COLUMN`'s 657-cycle multiply.

**Measured cost, frozen camera, `PROFILE=0`: 0.68 ms a frame, 12.2 fps to
12.1.**

A report counts only if the survivor was **on screen in the frame just
drawn** and within ten map cells (`sprite_reportable`). On screen is half the
test on purpose: a report should mean you looked at them, not that you flew
past with the camera pointed somewhere else.

## The billboards

`src/sprite.c` draws one world-anchored 2D figure into the framebuffer after
the terrain, scaled by distance and clipped against the heightfield. It is the
software sprite `documentation/vision.md` asks for, and the mechanism is meant
to carry the rest of them — campfires, crates, hazards.

There are two figures now and the flight draws one of them. **Every figure is
parked in bank 1 at load time and `sprite_select` DMAs the mission's own down
into the one near buffer**, because the 32K has room for a kilobyte of pixels
and not for two — and a kilobyte of DMA once a flight is nothing, while a far
pointer in the drawing loop would be paid per pixel forever. Adding a figure
means a sheet in the Makefile's `SPRITES`, an entry in `spr_files`, and
`SPRITE_FIGURES`.

**The pictures come off the sprite sheets through `tools/convmap.py`, not a
tool of their own**, because there is only one palette on screen: their
colours have to go in slots the colourmap left free, which is not knowable
without the colourmap — and every sheet has to draw from that same pool, so
they are converted together and consume one shared free list. It takes the
front pose out of each sheet's grid, cuts the figure off its checkerboard (the
background is light and grey and the pose labels are black, so the *saturated*
pixels find the figure and neither of those; the black outline needs a few
pixels of padding and the grey insides come back by filling whatever the
outside cannot reach), box-averages it into a 32x32 box — the sheet is a lossy
render, so every flat area of the pixel art is a cloud of near-identical
colours and point sampling samples the noise — and median cuts it to fifteen
entries. Each file is a four byte header and then the pixels **column by
column**, which is the order they are drawn in. Pixel value 0 is transparent,
so the test is `if (v)`.

**It is the longest side that is scaled to 32, not the height.** A lone
standing hiker is taller than it is wide and comes out 29x32; the pair by the
lake is wider than it is tall and would have come out 33x32, a column past the
buffer. Both dimensions are in the header and the renderer scales from them.

The projection is the renderer's own, and has to be: `TAN_HALF_FOV` and the
`inv_z` scale moved into `voxel.h` so that both use one set of numbers.
Depth along the view axis picks the scale, the lateral offset over that depth
picks the column, and the feet land on `horizon + (camh - ground) * inv_z >> 8`
— the same expression `voxel_asm.s` computes for terrain. `SPR_WORLD_H` is
what decides how big a survivor is, and **nothing about it is to scale**: a
map cell is a hundred metres, so a life-sized figure would be a fraction of a
pixel from anywhere worth flying. It is really the "how far away can one be
spotted" knob, set by eye.

`voxel_ground` had to learn about the map planes for this. It was reading
plane 0 — a point sample of the finer map — which on a peak is a few height
units below what the renderer actually draws, and that is several pixels of
sink or float for something standing on it.

### The depth clip

The march already keeps a y buffer per column: `vx_ybuf`, the topmost row
drawn so far, walking from near to far. Sampled at the sprite's depth it *is*
the clip — every row from there down belongs to terrain nearer than the
sprite. So `voxel_render` converts the sprite's depth to a march step,
`voxel_column_asm` copies `vx_ybuf` into `vx_yclip` at the first span at or
past it, and the sprite skips any pixel at or below `voxel_yclip[column]`.

Taking it at the *first span behind* the sprite rather than testing every
sample is what makes it cheap: spans are ~3000 a frame against 10240 samples,
and the assembly parks `vx_zclip` at 255 — higher than the step index ever
gets — as soon as it has the sample, so the columns that never reach it pay
one `cpy`/`bcc`. **Measured cost of the whole mechanism, always on: 0.8 ms a
frame, 12.7 fps to 12.5.**

If the snapshot never happens — the column filled first, or drew nothing
behind the sprite — the y buffer never moved after that depth, so the final
`vx_ybuf` is the answer, and that is what the C side stores instead.

### What it costs to draw

| | ms/frame | |
|---|---|---|
| nobody in view | 0.03 | the projection, and an early return |
| 20 cells away, ~15 px tall | ~1 | a realistic search distance |
| 6 cells away, ~45 px tall | 7.65 | 9% of the frame, nose to nose |

The drawing loop is in C at about 170 cycles a pixel, which is the usual
Calypsi figure and four times what the terrain span fill costs in assembly.
That is the obvious next optimisation and has not been done: at any distance
you would actually search from it is a fraction of a millisecond, and only
flying right up to somebody makes it visible in the frame time.

## The two-stage boot

The disk holds two programs. `AUTOBOOT.C65` is **stage one**, `src/mapgen/`,
which the ROM boots; it prepares attic RAM and then chains to `SAR`, the game.
They share nothing but the disk and `src/handover.h`, which is the contract for
the block stage one leaves behind.

**Why two programs.** The game already fills the 32 KB at `$2001` — 97.6%, about
790 bytes free — and a map generator is several times the code a *loader* is.
The two are never live at the same moment and **attic RAM survives a program
load**, so stage one can fill the attic and vanish. It pays twice: stage one
owns the whole 32 KB and banks 1, 4 and 5, because no framebuffer or screen
table exists yet, and the game eventually loses the loader and the exomizer
decruncher. `documentation/on-device-maps.md` has the costing.

**The chain is ozmoo's restart trick**: the command goes in the keyboard queue,
`main` returns, BASIC prints `READY` and the screen editor reads the queue as
though somebody had typed `RUN"SAR"`. Four things about it:

- the C65's queue is **`$02B0` with the count at `$D0`**, not the C64's
  `$0277`/`$C6`. ozmoo's own MEGA65 build uses the C64 pair because it runs the
  machine in C64 mode; this game runs under BASIC 65.
- `RUN"SAR"` is nine bytes, so the whole line fits the sixteen-byte queue.
  ozmoo prints its command to the screen and queues only the RETURN, because
  its line is longer than that.
- the screen does **not** have to be cleared first. The editor executes the
  logical line the cursor is on and `READY` leaves it at the start of a fresh
  one, so stage one's report scrolls out of the way by itself — worth keeping,
  since it is all there is to look at if the handover fails.
- **a pure-BASIC stage one would need a `NEW` before the `RUN`.** A PRG chained
  this way does not: `RUN"file"` resets the pointers itself.

The two routes not taken are written up in `on-device-maps.md`, both of which
fight Calypsi rather than the machine: a second BASIC line cannot be squeezed
past the fixed `SYS 8206` stub (`mega65-plain.scm` pins `startup` at `$200E`
and the library's stub section is `root`), and a loader of our own has nowhere
to stand while 32 KB is loaded over it.

**The name on disk is the contract.** `GAME_NAME` in `src/mapgen/mapgen.c` has
to match the Makefile's `$(PRG)` basename, because `diskutil.rb` names a file
on disk after its host file.

**Zero page is the hard part of handing the machine back**, and there are two
separate problems in it. The linker is allowed `$02-$7F` and BASIC lives there
too.

- **The queued line needs a `NEW`.** Calypsi's pseudo registers sit at the
  bottom of that range, on top of BASIC's own pointers, so a C program of any
  size leaves them wrong: `RUN"SAR"` comes back `?FORMULA TOO COMPLEX ERROR`,
  its temporary string stack pointer left past its end. NEW runs a CLR and puts
  those back. It also has to be **its own queued line** — NEW resets the
  interpreter's text pointer, so `NEW:RUN"SAR"` runs the NEW and silently drops
  the rest. Two RETURNs in the queue, which is what ozmoo does.
- **`NEW` is not enough on its own**, because it only resets the pointers a
  program is expected to move. Variables declared `__zpage` are placed *above*
  the pseudo registers and are saved by nobody: stage one's went to `$3A-$5A`
  when the generator's assembly got its parameter block, and with those held
  BASIC never reached READY at all — `main` returned, the queue was full, and
  the machine simply stopped. `zp_preserve`/`zp_restore` in
  `src/mapgen/kernal.s` copy the whole `zzpage` span out at the start of the
  run and put it back beside the CIA timers. **Any program that means to return
  to BASIC needs this**, and the symptom is a machine that dies silently after
  a working program finishes.

Two more things about running stage one under xemu. **Assembly called from C
must leave Z at zero** — Calypsi reaches through pointers with `lda (zp),z` and
keeps Z there, so a routine that leaves it at 128 offsets every later pointer
read. And **xemu headless quits itself about ten seconds into a run** on this
disk, on an `FDC SWAP bit emulation is experimental` dialog that `-besure` does
not cover, so a long boot cannot be watched to the end in one go; shortening
the field is how the handover gets tested.

**Stage one shares `src/profile.c`**, so its figures and the renderer's mean
the same thing, and it hands CIA2's timers back with Kernal IOINIT (`$FF84`,
`src/mapgen/kernal.s`) before the chain — the handover is a disk load, and
`profile_init` is exactly what stops the Kernal doing one.

**Its clock is right under `-sleepless`**, because it is calibrated against the
raster inside the emulator: the same run reports 54.38 seconds either way, and
a real-speed run brackets it from outside. That makes these experiments cheap.
What is *not* constant is the speedup, so a screenshot timed for one build will
miss on another — a loop that only polls the raster runs far faster than one
doing work. `make REPORT=200` holds the report up long enough to catch.

What stage one writes today is a proof block and one generated field, not a
map: a raster-drawn seed
and 16 KB of xorshift, which the game compares byte for byte and reports on its
boot screen (`STAGE ONE <seed>`, `NO STAGE ONE`, or `CORRUPT`) — and, since
step 2b of the port, `maps/island.yaml`'s 512x512 terrain noise, which stage
one reports a checksum and a time for. **`python3 tools/fbmcheck.py
maps/island.yaml` prints the same checksum from the PC** (`17DFF8E6`), and that
comparison is the only way to check a field at all, because `-dumpmem` writes
chip RAM only and cannot see attic RAM. Two details
that are not decoration, because **attic RAM is not cleared by a reset**: the
seed is drawn rather than fixed, so the block cannot pass its own test forever,
and the game clears the magic once it has read it, so a stale block reads as
absent. Note xemu is deterministic and hands out seed 27756 every boot, so the
freshness only really means something on hardware. `src/handover.c` is
scaffolding and goes when a real map proves the handover by being flyable.

## Resources

`tools/convmap.py` turns a 1024x1024 heightmap/colourmap PNG pair into `<stem>.hgt`, `<stem>.col`, `<stem>.pal`, `<stem>.ovr` and one billboard file per sprite sheet — the two maps crunched, the palette raw, and the figures cut out of the sheets (see The billboards). The sources need no quantisation: the heightmap is 8-bit greyscale and the colourmap is already a palette image. Palette bytes are nybble-swapped for the `$D100`/`$D200`/`$D300` registers, and the sky gradient goes in indices 224-239, which the colourmap never uses.

`convmap.py` takes the two map sizes as arguments; the Makefile passes
`HGT_SIZE` and `COL_SIZE`. The sprite sheets are one **comma-separated**
argument, from the Makefile's `SPRITES`, and the first one's output is
`<stem>.spr` while the rest are `.sp2`, `.sp3` and so on — the order
`src/sprite.c` numbers its figures in. Every map's conversion writes its own
copy of them and they are identical under `--shared`, so the Makefile takes
slot 0's and puts them on the disk as `terrain.spr`/`terrain.sp2`.

**The palette is what limits how many figures there can be**, and generating
the maps loosened it. Each figure claims fifteen entries, and what is left
over depends on the colourmap: the hand-drawn one uses about 170 of the 224
indices below the sky and left **12 free** after two figures, while a
generated map under `--shared` reserves the ramp's 150 and the system's low
sixteen and leaves **32** — two more figures' worth. `convmap.py` prints the
figure at the end of every run, and refuses rather than quietly painting
terrain in a sprite colour.

**Maps can also be generated rather than drawn.** `tools/genmap.py` turns a
map file in `maps/` into `hmapNN.png`/`cmapNN.png` — the same two shapes
`convmap.py` reads — from one seeded xorshift stream, so a seed and a YAML file
reproduce a map byte for byte.

**Its arithmetic is the MEGA65's, not Python's.** Every value is a Q0.16
integer, every divide is a reciprocal, sqrt/tanh/gamma are 257-entry tables,
and a histogram stands in for every percentile, median and sort — all of it in
`tools/fixed.py`, whose self-test prices each routine against the float it
replaced (worst 0.007 of a height unit). numpy is there to do a megapixel at a
time and nothing else: read `>>` as a shift and `np.where` as a branch and it
is the C. That is not tidiness, it is the on-device port
(`documentation/on-device-maps.md`) done in the place where a mistake is cheap;
the float version it replaced agreed with it to 0.03 of a height unit and one
ramp step, which is less than the dither the ramp already carries. `maps/palette.yaml` is the shared index ramp:
water at 16..23 by depth, then a land ramp at 24..149 that is **21 elevation
steps of 6 sun shades each**, with the RGB behind those indices chosen by the
mission's `climate`. About 130 entries against the hand-drawn map's 184, so
`convmap.py` still has around 60 free for figures.

Two knobs of that are worth knowing before touching the generator. **The sun is
due west and on the horizon**, because that is what the hand-drawn map measures
as (luminance correlates 0.69 with the east-west gradient and 0.04 with the
north-south one); shading is how fast the ground falls *towards* it, not a
Lambert term against a normal, which was tried and could not be tuned to suit
mountains and plains at once. And **`scale: near|medium|distant` is how big the
country is, not how much of it there is** — the world is always 256 cells
across, and this says how many landforms fit in it. It moves every length in
the generator, including the steepness references the rock colouring and the
sun are judged against, because doubling a landform's width halves its slopes.
**An `items:` entry can be terrain.** A `pyramid` is terraformed straight into
both maps — heights to roof height, colours to the palette's `masonry` band,
which is lit by the same sun as the ground — so the renderer never learns that
anything unusual is there and it costs nothing per frame. `size` is
`small|medium|large`, cut around the one in `C1W.png`. The rule to know before
changing its shape: **a terrace narrower than a map cell is not there.** The
heightmap ships box-averaged to 512 and the march samples half a cell at best,
so the first version's pixel-and-a-half steps came out of the air as a smooth
grey mound; terraces are two cells now, a cell of riser and a cell of tread,
and the riser wears the lighter course because a stepped face seen from the air
is nearly edge on. Item types genmap cannot build are refused rather than
dropped, and the previewer pins only the ones it does not build.

**The disk is generated maps now, and there are two of them.** The Makefile's
`MAP_YAMLS` names one mission file per map slot; each is run through
`genmap.py` and then `convmap.py` into `build/map0.*` and `build/map1.*`, and
`src/mission.c` says which slot a mission is flown over. The hand-drawn pair in
`resources/` is no longer built into anything — it stays as the reference the
sun was measured against and the pyramid copied from.

**A map file describes a world and nothing else** — `maps/island.yaml` is its
seed, its shape and the things built into its terrain. What is flown over it is
a *mission*, which has a map and is not one: two rescues could be flown over
the same island. The game holds its missions as a C table in `src/mission.c`;
neither `map.bin` (that table's map-side counterpart, and what an on-device
generator would read) nor `mission.bin` is written yet.

**Several maps fit only because they are generated.** Two of them are 487 KB
crunched against 661 KB for the one hand-drawn pair, so a disk that used to
hold one world now holds two with 270 KB spare, and loading both takes about
twenty seconds — less than the single drawn map took.

**Switching between them is 512 bytes of table.** A map's whole location lives
in the renderer's plane lookups: the march reads a bank byte out of
`vx_hplane_y`/`vx_cplane_y` on every sample, so `voxel_set_map(slot)` rebuilds
those and the march is looking at another world. `map_use(slot)` wraps that
with the two other things a map is — **its palette, because a climate *is* a
palette** (the shared ramp means both maps hold the same indices and put
different colours behind them), and the panel's overview — and a flight calls
it once at launch. No pixels move and nothing reloads: everything the game
will ever need is resident after the one load at boot, which is what lets the
Kernal be unusable from `vic4_init` onwards.

Three things to know before adding a third map:

- **`convmap.py --shared maps/palette.yaml` is what makes it work at all.**
  Without it each map hands the sprites whatever palette entries its own
  colours left free, so a figure changes colour with the mission. With it every
  map reserves the whole shared ramp *and* the low sixteen, so the pool starts
  in the same place for all of them and the sprite files come out
  byte-identical — one set serves every map. It also settles the old question
  about sprites taking indices below 16: they no longer can.
- attic RAM is cut into **three** 2 MB slots (`MAP_SLOT` in `loader.h`), so
  there is room for one more without moving anything. `MAP_COUNT` in
  `loader.h` and `MAP_YAMLS` in the Makefile have to agree.
- **`FLYNOW=n` launches straight into mission n**, and it is the only way a
  headless run reaches the second map — nothing else can press a key. Both
  maps were flown that way before the arrangement was believed.

`documentation/procedural-maps.md` has the design, what is built, and the traps
found building it. The one to know before touching the water: **a river
measured against the live map digs itself a canyon** — each disc flattens the
ground ahead of the walk, so the walk reads its own channel floor and cuts
again, and thirty steps later it is below sea level, flattened to the sea plane
and painted in the darkest water there is. It looks like a black line ruled
across the map. Measure against a pristine copy. The second one to know is
that **water is only ever cut into the ground, never built up out of it**: a
lake whose flood stopped on its area budget took its level from the highest
cell it had swallowed while the frontier still held lower ones, so it stood
several units above the beach beside it — a row of dark blocks out of the sea
at the waterline, invisible in plan view and obvious from the air. A river disc
crossing a slope did the same to its downhill half. The level is cut back to
the lowest cell left on the frontier, and a channel only wets ground already at
or above its run. The others: a lake flood will flood the ocean; a river
arriving at the coast will raise the sea; meander noise may only choose between
neighbours that already run downhill, never decide whether the river goes on;
fold ridged noise once at the end, not per octave.

**`tools/preview.py` flies a generated map on the PC, and it is this renderer
rather than a lookalike.** The band schedule, the 8.8 position update with its
16-bit wrap, the biased horizon, the y buffer, the map sampling, the sky, the
flight model and the panel readouts are all the ones in `src/`; the hardware
walks a column at a time and the previewer walks a *step* at a time across all
160 rays, which is the same order with the loops exchanged. Three things about
it:

- **it reads the renderer's constants out of the C source** (`C_DEFINES` names
  each `#define` and its file, and the sine table and speed limits are parsed
  too). A constant that moves or becomes an expression stops the tool with a
  message instead of quietly flying a different game. Maps go through
  `convmap.py`'s own loaders for the same reason.
- **verified against the machine, not by eye — and the check is a command.**
  The panel reports the camera exactly, so a xemu screenshot can be reproduced;
  at the same camera every one of the 48336 palette indices is identical.
  `python3 tools/checkview.py` is that comparison against a reference
  screenshot in `documentation/reference/` (4 seconds, no emulator; it needs
  `genmap.py` to have written the maps, which are not in git). **Run it after
  touching the renderer.** A failure means the two implementations disagree —
  either `preview.py` has not caught up, or the change was deliberate and the
  reference screenshot is stale. Proved to catch both kinds of drift by
  breaking each on purpose.
- it runs at **12.5 fps on purpose** — every rate in the flight model is per
  frame, so a faster preview is a faster drone. No wind and no crash: it is an
  inspection tool, and `M` marks a position in the form the map file wants.

Height units are a quarter of a map cell — the source is 4x the renderer's 256-cell grid and the heights were not rescaled — and `SCALE_H` in `src/voxel.c` folds that in. `HGT_SIZE` does not change this: a finer heightmap subdivides each cell rather than widening the world, so the world stays 256 cells across whatever the map resolution.

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

**Reading a SEQ file through the Kernal reports EOF exactly 256 bytes early**, whatever the file's size (verified from 16 K to 64 K, at several chunk sizes). `convmap.py` therefore pads every resource by 512 bytes and `src/loader.c` reads a known length rather than looking for the end of the file.

**`load_far` hangs on the palette file and nothing explains it.** Reading the
768-byte palette through `load_far` — open, read into the bounce buffer, DMA
it up, close — never returns from the open or the read that follows it. The
same 768 bytes of the same file into the same buffer through `load_small` is
fine, and `load_far` reads the overview two lines later without complaint.
Ruled out, each with its own run: the DMA destination (bank 1 at `$1C800` and
`$1E000`, attic RAM at `$8300000` — all hang), the chunk size, short Kernal
reads (`read_exact` loops now and it still hung), and `--no-cross-call`. The
argument values reaching `read_far` were printed and are correct. It behaves
like something layout-sensitive rather than a logic error, so **if the loader
starts hanging after an unrelated change, suspect this and not the change** —
the reproducer is one line, swapping the palette back to `load_far`. Resources go on the disk as SEQ, not PRG: Calypsi's `_Stub_open` calls Kernal OPEN with the file descriptor as the secondary address, which defaults to SEQ, and SEQ has no load-address header to skip.

`tools/diskutil.rb` (from Fredrik Ramsberg) builds the D81 and refuses to overwrite a file that already exists on the image, so the Makefile deletes and rebuilds the image every time. The name on disk comes from the host file's basename.

## Performance

Roughly 12.5 fps at the default map sizes, from 0.74 when the renderer was
all C. `src/profile.c` measures
it; `tools/profread.py` formats the results out of a `-dumpmem` image. Its
`TIMES` list and `BENCH0` are positional, so **adding a `P_` slot to
`profile.h` means editing both**. The FPS
readout in the corner is always on. `make PROFILE=0` compiles out the
per-column instrumentation and the counters in `voxel_asm.s`, which the
Makefile guards with the same flag passed to the assembler; the FPS counter
stays. **That instrumentation costs about 7% of a frame**, so the FPS readout
in a default build reads low against what the machine actually does. Quote
speed from a `PROFILE=0` run — and note `profread`'s per-frame counters need
the opposite, a `PROFILE=1` build. Two runs, not one. Switching
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
| DMA copy chip to chip | 2.20 /byte | | |
| DMA copy attic to chip | | 16.11 /byte | **7.3x** |
| DMA copy chip to attic | | 9.54 /byte | **4.3x** |
| DMA fill | 1.12 /byte | | |

So a CPU access to attic RAM costs a flat ~15 cycles more than chip RAM,
whether it is scattered or sequential — the 8-byte cache line buys nothing
worth planning around. **Writes are posted and nearly free at +3.** And the
DMA is the opposite of what everyone assumes: attic to chip runs at 2.5 MB/s
against 18 MB/s chip to chip, which rules out any per-frame bulk move out
of attic RAM. Keeping the sky template in chip RAM rather than attic is worth
11 ms a frame on its own.

**The attic bus is asymmetric, and in the useful direction.** Writing *into*
attic RAM runs at 9.54 cycles a byte against 16.11 coming out — 1.7x faster —
which is the same story the CPU figures tell, where a posted write costs +3
and a read +15. Anything that produces a lot of data once and reads it back
rarely is therefore much cheaper than the attic-to-chip figure suggests, which
is exactly the shape of a map generator (see
`documentation/on-device-maps.md`). Two consequences worth carrying:

- **a DMA out of a chip RAM row buffer is not the cheap way to fill attic
  RAM.** 9.54 cycles a byte is more than the +3 a posted CPU write costs in a
  loop that is computing the value anyway, so a generator should write its
  pixels straight up there and keep the DMA for moves it cannot fold into a
  loop it is already running.
- the three DMA rows above were re-measured with this one and came out about
  10% faster than the figures recorded here before (2.45 / 17.80 / 1.22). The
  ratios are unchanged to two figures, so nothing that was concluded from them
  moves; treat the absolute numbers as ±10% between core versions.

Both `profread` and the on-screen report print an addressing check
(`$11223344`) alongside these — note that it needs `volatile` far pointers,
or the compiler reorders the writes and reads past each other and the check
fails on correct hardware.

The same run had the real machine at 11.0-11.2 fps against xemu's 11.6, so
xemu's chip RAM timing is about 4% optimistic for the *renderer's* instruction
mix.

**That 4% is not a constant — it is a property of the code being run**, and
three measurements now bracket it:

| | xemu | MEGA65 | |
|---|---|---|---|
| the renderer's march (assembly) | 11.6 fps | 11.0-11.2 | 4% optimistic |
| the map generator's noise, in C | 54.38 s | 1m05s | **19.5%** |
| the same, rewritten in assembly | 9.26 s | no measurable difference | |

The last row is a stopwatch comparison of whole boots and only says the gap is
inside a few seconds; the 19.5% is safe from that doubt, being ten seconds wide.
Stage one prints its own time from the profiler's clock, so reading it off a
real machine would settle the last row properly.

So **xemu models tight zero-page assembly accurately and the compiler's output
badly** — what it gets wrong is software-stack indirection and heavy `$D770`
traffic, not chip RAM. A C timing taken in the emulator is optimistic by an
unknown amount; an assembly one can be trusted. (The attic writes are not the
explanation: 512 KB of posted writes at +3 cycles is four hundredths of a
second.)

The one place the machine is still slower is **the disk**: the game and its
resources take roughly twice as long to load as the generator takes to run, and
longer on hardware than under xemu -- a real drive against an instant one.

Real hardware has no `-dumpmem`, so `profile_report` prints the same memory
table to the Kernal's text screen at startup and waits for a key (or 20
seconds, so unattended runs still get on with rendering). It is boxed in on
both sides: after the resources, because `profile_init` takes the timers the
Kernal reads a disk with, and before `vic4_init`, which takes the text screen
away. The figures come out identical to `profread`'s, so either route can be
trusted.

**`int` is 16 bits, and a constant expression will overflow it silently.**
`(uint16_t)heading * 360 / 256` in the panel read 192 degrees as 14, and
`FB_WIDTH / 2 * 256` in a macro folded to a negative number. Reassociate
(`* 45 / 32`) or force the width (`256L`); the compiler warns about the second
kind but not the first.

**A narrowing cast to `uint8_t` inside a wider expression is SIGN-extended.**
`(uint16_t)(uint8_t)(angle + 64)` compiles to `adc #64 / ora #127 / bmi / lda
#0` — the byte is widened as though it were signed, so every value from 128 up
comes out negative and the arithmetic after it is nonsense. It cost a wind
bearing of 896 degrees, and it is invisible at small inputs: the heading
readout that shares the macro looked right for a whole commit because the
camera launches at angle 0. Never let a value pass through a byte type in the
middle of an expression — widen first and mask (`((uint16_t)a + 64) & 0xFF`),
which never puts it in a byte at all. **Check the `--list-file` listing when a
number comes out wrong and the arithmetic looks unarguable**; that is what
found this in one look.

**Coordinates through `int8_t`, and small `static const` arrays inside a
function, miscompile.** The crosshair was first written with `int8_t` x and y
and the four arms in `static const int8_t arm_x[8]`; it stored nothing at all
— no writes reached memory, while the surrounding code ran and a canary
proved the function was entered. Rewritten with `int16_t` coordinates and the
eight calls spelled out, the same logic works. Suspect this shape before
suspecting the hardware.

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

Where a frame goes (the older 160-pixel framebuffer, h256 c512, 64.7 ms),
measured by stubbing
pieces out rather than guessing (setting `voxel_column_asm` to an immediate `rts` isolates the
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

**`WIDE=1` is retired** — see the low free RAM note under Memory map. The
figures below are the measurements that were taken while it still built.

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

**h512 is the finest heightmap the march can resolve, so h1024 buys nothing.**
`Z_STEP0` is 128 in 8.8 — half a cell — and that is the *closest* spacing the
ray ever samples at: band 0 steps 0.5 cells for 8 cells, then the bands step
1, 2 and 4 out to 120. h512 matches that rate exactly; h1024 is two times past
it along the ray and can only alias. Measured by diffing screenshots of the
same fixed camera at c512, over the 3D view only:

| | cruise (alt 79) | hugging terrain (alt 31) |
|---|---|---|
| h256 → h512 | 4.2% of pixels differ | 13.3% |
| h512 → h1024 | 2.1%, mean 0.30/255 | 8.7%, mean 1.58 |

The h512→h1024 diff is scattered one-pixel slivers at span edges — silhouettes
moving a row — with no terrain feature appearing anywhere; the two pictures are
hard to tell apart even nose to the ground. h256→h512 is a real difference in
kind: at low altitude h256 stair-steps the shoreline into rectangles and goes
chunky in the foreground. So the default earns its place and the next step up
does not. The residual h512→h1024 difference is lateral, not along the ray —
near-field columns are packed closer together than the ray step — which is why
the low-altitude figure beats the cruise one and why it is still only jitter.

That also says where to spend if the near field ever needs more detail: **the
lever is `Z_STEP0`, not `HGT_SIZE`.** Prepending a 0.25-cell band (5x16:
0.25/0.5/1/2/4) reaches 124 cells for 80 samples instead of 64, about 25% more
march, ~11 ms — and only then would h1024 have something to show. Raising
`HGT_SIZE` first pays 350 KB of disk and ~15 s of loading for nothing.

| | 256x256 colour | 512x512 colour |
|---|---|---|
| 320 rays | 124.8 ms, 8.0 fps | 133.1 ms, 7.5 fps |

Marching 320 rays doubles the sample count exactly and costs half the frame
rate, and **the picture barely changes** — the blockiness was the map showing
through, not the pixel grid, so the screen was never the limiting resolution.
Doubling the *colourmap* instead is free and plainly visible. That is the
trade this engine responds to: **detail per cycle lives in the map, not in
the raster.** The panel was the real argument for a 320-pixel screen, and it
did not need the extra rays to get one.

**The framebuffer is 320 wide and the march is still 160 rays.** Each ray
fills the two neighbouring pixels it owns, which is one `inz` / `sta` / `dez`
added to the span fill: 2n and 2n+1 always share a character, since 2n is
even and the pair can never straddle the eight-byte boundary, so the second
pixel is simply the next byte. Measured at h512 c1024, `PROFILE=0`:

| | frame | |
|---|---|---|
| 160-pixel buffer, VIC-IV stretch | 71.9 ms, 13.9 fps | the old arrangement |
| **320-pixel buffer, 160 rays** | **77.5 ms, 12.9 fps** | the default |
| 320-pixel buffer, 320 rays | — | `WIDE=1`, halves it again |

7% for a panel that is legible, and **the 3D view is pixel-identical** — the
doubled pixels are exactly what the hardware stretch was producing. The cost
is the second byte write per span pixel plus the sky DMA covering twice the
buffer; the march, two thirds of the frame, does not move.

(An earlier note here priced the finer colourmap at 3.6%. That was the
raster-calibration jitter above, not a real cost; re-measured with the fix it
is zero. Small differences measured before that fix are worth re-taking.)

Draw distance and cost are traded in the band schedule at the top of
`voxel.c`: `BANDS` x `BAND_STEPS` samples per column, with the step doubling
each band. 4x16 reaches 120 map cells for 64 samples.

Beware `iny` between an `adc` and the branch that tests its result — it
overwrites the flags, and the counters caught it as 160 full-height spans a
frame. `next$` is the one place the march advances Y, because every path that
goes round again passes through it.

**The assembler does not see C headers.** It gets `-DPROFILE_DETAIL`,
`-DWIDE`, `-DHGT_SIZE` and `-DCOL_SIZE` from the Makefile and nothing else,
so an `#if` on anything derived in a header — `COL_AXIS`, say — is silently
false and assembles the guarded code away. That happened to the plane lookup:
every sample read plane 0, which is a coarse point-sample of the finer map,
so the picture came out *worse* than the map it replaced while the frame time
and the silhouette check both looked plausible. Only a zoomed A/B caught it.

## Gotchas

**The startup order is not a style choice.** Resources have to be read before
`profile_init` and before `vic4_init`, either of which leaves the Kernal
unable to open a file; and nothing may `printf` once the display is up,
because the ROM's screen editor writes the colour RAM the game is using. Both
are written up under The game, and both look like a corrupt disk rather than
an ordering mistake.

The Makefile deliberately makes every object depend on every header. Without it, changing a layout constant in `vic4.h` leaves stale objects built against the old memory map, and the result looks like a hardware fault rather than a build problem.
