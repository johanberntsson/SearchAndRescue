# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

A MEGA65 heightfield voxel flight simulator / drone search-and-rescue game, written in C (Calypsi) with the rendering inner loop in 45GS02 assembly. `documentation/vision.md` holds the full technical and gameplay design; `todo.md` is the authoritative "what's next" and should be updated as work lands.

Currently: three missions, end to end, **each over its own generated world**. A
title screen, a mission list, a briefing, a flight, and a debrief — the lost
hiker on the step pyramid of the island at 46.687N 8.106E to be found and
reported, an EpiPen to be dropped to a pair of hikers by a lake on the
plains at 46.658N 8.149E, and a skier buried by an avalanche at 46.584N
8.177E who cannot be seen at all until the **thermal camera** is armed. A
flight also carries a wind that blows the drone
about, a battery that runs it out, and per-mission weather; it can end four
ways, all of them the same debrief page with different words on it.

**All three maps are generated from a paragraph of YAML on the PC and all
three are resident at once**, which hand-drawn map pairs could never be: one
drawn pair is 661 KB crunched on its own, and switching between them
costs 512 bytes of plane table. See Resources.

Underneath is the voxel engine at about 12.5 fps — a 320x152 3D view over a
six-row instrument panel -- a picture, with its readouts on a plane of
hardware sprites over it -- with 512x512 height and colour maps
unpacked into attic RAM at boot, marching 160 rays and writing each to two
neighbouring pixels; see Performance.

## Build and run

```sh
make run          # build build/sar.d81 and boot it in xemu
make prg          # skip the disk, run the game PRG directly
make PROFILE=0    # without the per-column instrumentation; use this for timing
make FLYNOW=1     # skip the title and menus, launch straight into mission 1
make FLYNOW=n     # ... or mission n, the only headless way to reach its map
make REPORT=n     # hold the startup benchmark report n seconds; 0 by default
make COL_SIZE=1024                  # the finer colourmap; sizes are 256..1024
make release      # PROFILE=0 disk, copied to release/sar-latest.d81
make checkmusic   # both assemblers over the tune, byte for byte; needs acme
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
xemu-xmega65 -skipconfigfile -besure -headless -sleepless \
    -8 build/sar.d81 -screenshot out.png -dumpmem mem.bin &
sleep 75; kill -INT $!          # xemu writes both files as it exits
python3 tools/profread.py mem.bin
```

**`FLYNOW=1` is what makes a headless run measure anything.** The game now
waits for a keypress on the title screen, then two more through the menus, and
nothing headless can press one; without it the dump has no frames in it and
`profread` says so. **`FLYNOW=n` is the same for mission n**, and the only
way a headless run ever reaches that mission's map.

**A headless run cannot press `T` either**, so the thermal camera was checked
by defaulting it on for one build -- the same trick the frame-rate key needed.
See The thermal camera.

Budget about twenty-five seconds before the title in xemu: two of ROM boot and
the game's own 20 KB, then three maps at 446 KB crunched. **That is nearly all
the avalanche map**, which is 268 KB of the 446 -- see Resources. The startup benchmark
report no longer holds the boot up -- `REPORT_SECONDS` is 0, which now means
it does not print either; `make REPORT=120` to read it at the machine.
**Kill the run by PID and wait for it**, not with a bare `sleep` in the same
command: a wait that races the emulator reads the *previous* run's screenshot,
which looks exactly like a change that did not take.

`timeout -s INT` looks like the way to end the run and mostly is, but xemu can
take several seconds to act on the signal, so a loop that starts the next run
immediately ends up with two emulators writing screenshots over each other.
Backgrounding it and waiting, as above, is what actually serialises.

**`-skipconfigfile` must be the first argument, and it is there for a reason.**
xemu rewrites its config template on exit; run several instances at once —
which a timing sweep wants to do — and they race each other's rename and each
one pops up *Cannot save config template* at whoever is sitting at the machine.
Skipping the config file stops that. Better still, **do not run them in
parallel**: the popups aside, contention moves the timings by several seconds
between batches, which is enough to invent a boundary that is not there.

`-sleepless` is fine here — see Performance for why it must never be used to time anything from the outside.

`-dumpmem` writes 384 KB of **chip RAM only**, so it cannot see attic RAM and cannot see either map at the default sizes; `mem.bin[0x40000]` is the heightmap only when `HGT_SIZE=256` keeps it in chip RAM. `mem.bin[0x10000]` onwards is framebuffer A. Note that a dump catches whichever buffer is mid-render, so it is no good for judging a finished frame — compare screenshots for that. Reading a framebuffer back and de-swizzling it with the column-strip formula below is the fastest way to tell "the renderer is wrong" apart from "the display is wrong"; both have happened. There are no tests and no linter.

## Toolchain

Calypsi 6502 C compiler 5.18, installed system-wide (`/usr/local/bin/cc6502`, `ln6502`; headers, libraries and linker rules under `/usr/local/lib/calypsi-6502-5.18/`). `--target=mega65` selects the 45GS02 core, puts the MEGA65 SDK headers on the include path (`<mega65.h>` gives `VICIV`, `PALETTE`, `CIA1`, `MATH` …) and links the board support library.

**`ln6502 -o` names the ELF output, not the PRG.** The PRG is written alongside it under the same stem, so the Makefile links to `build/sar.elf` and copies the resulting `build/sar.prg` to `build/autoboot.c65`. Linking straight to `autoboot.c65` silently produces an ELF file that the MEGA65 tries to RUN as BASIC.

`mega65-plain.scm` resolves from the toolchain's `linker-rules/` directory, not this repo. It gives the program `$2001-$9FFF` — 32 KB for code, data, stack and heap — and emits a C65 BASIC stub (`SYS 8206`), which is exactly what `autoboot.c65` needs.

**ACME is a second assembler in the tree, and it does not build anything.**
The SID player and its tune under `music/` are written for it;
`tools/acme2calypsi.py` translates them into the Calypsi assembler at build
time and needs only Python. ACME itself is used by `make checkmusic`, which
assembles the same source both ways and compares the bytes. See Sound.

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
| `$40000` | 15360 | The panel's background artwork, 40x6 full-colour characters |
| `$43C00` | 16 + 1920 | The panel's text plane: eight sprite pointers, then five 64x48 sprites |
| `$40000-$4FFFF` | 64 KB | Heightmap, only when it is 256x256 — **which no longer builds**, see The panel |
| `$8000000-$81FFFFF` | 2 MB | Map slot 0: colourmap planes, then heightmap |
| `$8200000-$83FFFFF` | 2 MB | Map slot 1 — mission two's world |
| `$8400000-$85FFFFF` | 2 MB | Map slot 2, spare |
| `$8600000-` | | Staging for the crunched stream being unpacked |
| `$5C000`, `$5D000` | 2000 each | The two screen tables |
| `$8700000` | 768 each | One palette per map slot, until a flight uploads its own |

**The 32 KB is `mega65-plain.scm`'s choice, not the machine's.** The stock
linker script gives `program` `$2001-$9FFF` because that is what is safe with
every ROM mapped in. **Bank BASIC out — `$D030` bit 4 — and `$A000-$BFFF` is
ordinary RAM**. The map generator did this and its `program` section went from
overflowing to 66% used; the script is on the `mega65-mapgen` branch. Three things make it low risk, and
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

**The trap in copying it is `(type any)`.** A PRG cannot have two *content*
areas, and a second memory declared `(type any)` becomes one the moment the
linker puts anything initialised there — `zdata`'s data half or `cstack`, both
of which the rules make eligible. The generator's stage one never hit it only
because its `highbss` filled `$A000-$CFFF` to the byte; stage two left a
kilobyte spare and failed at once with *multiple program areas not allowed in prg output*. The fix
is to declare the window the way the stock script declares `freeSpace` — a
name and a section and **no `(type …)` at all** — which takes the named section
and never becomes a content area. `(type ram)` does not work.

**The game has not been banked yet**, and what it needs first is its big BSS
marked `HIGH_BSS`; `mega65-game.scm` and `src/bank.s` are waiting for it. 

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
- **`printf` itself, 6.5 KB**, which dwarfs every line above it. The boot
  screen was the last thing calling it; now that the loading bar is written
  into screen RAM and the benchmark report only prints when `REPORT_SECONDS`
  asks for it, a default build has no reference to `printf` at all and the
  linker drops the whole formatting machinery. 26728 bytes of PRG to 20219,
  and the program area from 88% used to 68.2%. See The game for what replaced
  it.

**And it was spent again the same day** — 2652 on the tune, 583 on the engine
note and 1449 on the campaign buffer, net of the C mission table it replaced.
That was the point of the reclaim. The thermal camera and its 32-bit key mask
then took about 820 more, so a default build is **92.2% used with 2556 bytes
free**, and a `PROFILE=0` release 91.0% and 2949. The next thing of any size
wants the banking below before it wants anything else.

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
bottom six are the information panel. That split is free, because full colour
is per character *number*: `FCLRHI` is set and `FCLRLO` is not, so numbers
above `$FF` are 64-byte full-colour characters (the framebuffer) and numbers
below are ordinary 8x8 text from `CHARPTR`. The panel costs 480 bytes of
screen RAM, no pixel writes, and is not double buffered — `vic4_panel_char`
writes the same cell in both screen tables.

### The background artwork

**The panel is a picture with text in it**, not six rows of text. 40x6
full-colour characters — 15360 bytes at `PANEL_ART` in bank 4, converted from
`resources/panel.png` by `tools/convmap.py --panel` and read off the disk
crunched (2.4 KB). Drawing it is naming 240 character numbers, so it costs no
pixels and no per-frame work at all: a screen cell costs the same whether it
names a letter or a picture.

Three things make it fit:

- **its palette is free.** Fourteen entries above the sky (242..255) that
  nothing else has ever used — the sprites' free pool stops at `SKY_BASE` —
  so the artwork costs the figures none of their budget and one `.pnl` file
  serves every map. `convmap.py` quantises **after** rounding to the VIC-IV's
  four bits per channel, because the first version spent two entries on box
  greens that are the same colour on the machine. Fourteen entries and
  thirty-two measure identically once those four bits are applied: the
  palette depth is what limits this picture, not the entry count.
- **bank 4 is the only VIC-visible RAM with room.** Bank 1 is full to the
  byte and bank 5 above the screen tables has 10 KB against the 15 this
  wants. Deduplicating the characters does not close that gap either — 209 of
  the 240 are unique, this being a render rather than pixel art — so nothing
  is deduplicated. It does mean `HGT_SIZE=256` no longer builds, since a
  256x256 heightmap wants the same bank; `loader.h` `#error`s on it.
- **the paper is the artwork's own colour.** A text character's background is
  the screen colour and there is no per-cell alternative, so a readout drawn
  over the picture used to punch a black hole in it. `convmap.py` puts the
  artwork's commonest colour — the flat green of its readout boxes — first in
  its entries, at `PANEL_PAPER`, and `vic4_view_mode` makes that the screen
  colour while the view is up (`vic4_text_mode` puts 0 back for the pages,
  which want a black page). The 3D view never draws a pixel of 0, so nothing
  up there notices.

### The text plane

**The readouts are not characters. They are five hardware sprites.** A picture
is not drawn on an 8-pixel grid and a text character is, which was the one
thing the artwork could not live with — the boxes are 14 pixels tall where two
character rows are 16, and the cargo box is 4 pixels off the grid outright.
`src/overlay.c` puts 64x48 sprites side by side over the panel: 5 x 64 is 320,
the width exactly, and 48 is its six rows. See `src/overlay.h` for why not the
alternatives, and the sprite paragraph above for the registers and the proof.

- **1920 bytes of 1bpp bitmap** in bank 4 above the artwork, and eight sprite
  pointers below it. A glyph is eight rows of one shifted byte pair, about
  sixteen writes, against sixty-four for painting it into full-colour pixels.
- **Nothing is restored.** The plane is transparent where it is not drawn, so
  clearing a field is writing zeros and the artwork underneath is never
  touched. That is what makes it cheaper than blitting, not the write count.
- **The plane is in column strips**, exactly as the framebuffer is: the five
  sprites are separate 384-byte blocks, so a byte column is
  `(col >> 3) * 384 + (col & 7)` and stepping down a row is +8 whether or not
  the column crosses into the next sprite.
- **One colour, because a sprite has one.** Labels and values are the same
  green (`PANEL_TEXT`, reserved by `convmap.py` beside the other panel inks
  and sampled off the mockup). Two colours would mean two planes, and the
  mockup wanted one anyway.
- **The panel has a font of its own**, `font/ClairsysOzmoo-Regular-US.fnt`,
  read off the disk into bank 4 and indexed by the character itself. The
  game's *pages* still take the C65 ROM's set at `$2D000` through `CHARPTR` —
  they are text characters and that costs no RAM at all. The panel's text is
  not characters any more, so what it is drawn *from* is only a table
  `overlay.c` reads, and swapping that table costs 768 bytes and nothing else.
  It is in the C64 screen-code layout, in which space through `Z` sit exactly
  where ASCII puts them, so no conversion happens on the way in — doing the
  conversion the pages need drew every letter as its lowercase.
- **Only 96 glyphs ship, from space up**, which is every code the panel can be
  asked for. That is not thrift: at 768 bytes the font fits the staging
  buffer, so it is read the way the palette is. **`load_far` hangs on it** —
  the loader stopped dead partway through a *map*, which is not even where the
  font is read, and booted the moment the call came out. Second file to walk
  into that unexplained fault; see the palette note in `loader.c`.

**Measured, and it is faster than the characters it replaced**: the frame's
"everything else" went 2.46 ms (character text) → 4.1 ms (the first version of
the plane) → **1.87 ms**. What closed it was not drawing a field that has not
changed: at 170 cycles a byte in C, refreshing four readouts every frame is
real money, and most of them do not move every frame — the fix is in
millidegrees, which is one map cell, so it changes only when the camera
crosses a cell.

Two bugs are worth not repeating, both from drawing a field in pieces:

- **the clear has to be masked to the pixel, not rounded out to byte
  columns.** Text at an arbitrary x always shares its first and last byte
  column with whatever is beside it: the wind's bearing ends inside the same
  column its `DEG` begins in, and rounding turned `DEG` into `)EG` on every
  refresh.
- **a box is one string.** The launch message covers the fix and the battery,
  and when the battery was drawn as a separate label plus a number, the
  message wiped `BATT` with nothing left to put it back. Every readout now
  writes its own label with its own value in one call.

The layout, in pixels of the panel's 320x48, measured off `resources/panel.png`:

| | |
|---|---|
| x 0-55 | the compass: the altitude at y 12, the heading at y 28 |
| x 57-188, y 5 | the fix, `46.681N 008.083E` — or a message, which takes both top boxes |
| x 191-270, y 5 | `BATT 100%` |
| x 57-188, y 21 | `WIND 094DEG 4M/S`, sixteen characters, the box to the pixel |
| x 198-270, y 21 | `SPD NRM` |
| x 156-265, y 36 | the cargo bay — its `CARGO` label is painted into the picture |
| x 60-124, y 36 | the frame rate, the one readout with no box of its own |
| x 276-314 | the overview map, still full-colour characters |

**The battery has a sprite of its own**, which is the whole reason there are
six and not five: a sprite carries one colour, and the battery is the only
readout that changes colour -- green, then yellow under `PANEL_WARN_AT` (25%),
then red under `PANEL_ALARM_AT` (10%). It is drawn like any other field, at x
`OVERLAY_ALERT` in the same coordinates as the rest, and simply *appears*
somewhere else, over the artwork's battery box. `panel_battery` returns the
level it drew so that `main.c` can decide what it sounds like: the panel says
how it looks, and the sound is not the panel's business. `BAT` rather than
`BATT` because the sprite is 64 pixels and eight characters is all of them.

**A message takes the two top boxes for as long as it is up**, covering the
fix and the battery, and `panel_message(0)` is how one ends: the fix redraws
itself on the next frame and the battery is put back from the last figure it
read. The artwork has five text areas and the game has six things to say, so
an alert borrows the field it needs — which is what an instrument does.

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

**Hardware sprites work here, and the note that said they did not is
retired.** In this layout the legacy sprite pointer comes from
screen+`$3F8` — row 12 column 28, in the middle of the 3D view, where the
character number is a framebuffer tile and cannot be given a useful value — so
for months the crosshair could not be a sprite. `SPRPTRADR`
(`$D06C`-`$D06E`) is the way out and used to be ignored by xemu, with
`SPR_PTR16` *segfaulting* it outright.

**`make sprtest` is the retest**, and on 15 Aug 2026 it came back yes to all
of it. `src/sprtest.c` is a standalone PRG that sets the display up with the
game's own `vic4_init` — the layout being the whole question — and puts four
sprites over the panel's rows. Measured off the screenshot, in display pixels:

| | asked for | measured |
|---|---|---|
| 16-bit pointers (`SPRPTRADR` + `SPR_PTR16`) | any 64-byte boundary in chip RAM | sprite data read from `$44100` in bank 4 |
| `SPRX64EN` (`$D057`) | 64 pixels wide | 64 x 21 |
| `SPRHGTEN`/`SPRHGHT` (`$D055`/`$D056`) | 48 pixels tall | 24 x 48, and 64 x 48 with both |
| priority over full-colour characters | sprites in front | in front, over an opaque FCM tile |
| the C65 ROM font at `$2D000`, read by the CPU | glyphs | legible text, placed 3 pixels down |

So **five 64x48 sprites cover the whole 320-pixel panel** for 1920 bytes of
1bpp bitmap, and a glyph shifted into one is about sixteen writes against
sixty-four for painting it into full-colour pixels — with nothing to restore
afterwards, the plane being transparent where it is not drawn. That is the
route out of the 8-pixel character grid the artwork cannot live with; see
todo.md. **Still to confirm on real hardware**, where the original note came
from.

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
colours that were there out to unused slots; **the paper is palette 0 no
longer** — see the background artwork below, which owns the screen colour for
the duration of a flight. Palette entries 240 and 241 are `convmap.py`'s
overlay pair, for things drawn *over* a picture rather than sampled out of a
map — and both are taken: 240 is the overview crosshair and the two together
are the rain's near and far depths. **242 to 255 are the panel artwork's** and
were free until it existed. **223 is the thermal camera's hot white**, taken
out of the free pool by `convmap.py` rather than from any of these — which is
what a further overlay should do too.

## The game

`src/main.c` is a state machine over four full-screen pages and a flight:
title, mission list, briefing, fly, debrief, back to the list. `src/screens.c`
draws the pages, `src/mission.c` holds what there is to be sent on.

**The three missions are the same flight with different words on it** — and,
as of the several-map disk, over different country. The shape is deliberate:
fly to a figure standing at a fix and press a key. The mission table is what
differs, and the one field the rest hangs off is `cargo`:

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
travel together and cannot be edited apart: mission one is flown over
`maps/island.yaml` and stands its hiker on the step pyramid there; mission two
over `maps/plains.yaml`, by the largest lake; mission three over
`maps/avalance.yaml`, on a snow slope three quarters of the way up a mountain.
See Resources for what a map slot is and what switching costs.

**And a mission can say the figure is not visible at all.** `hidden: thermal`
is what mission three adds, and it is the whole of what makes the second
sensor a thing you need — see The thermal camera.

**Adding a mission is a file in `missions/` and a line in `campaign.yaml`**,
and nothing else at all — see The campaign below. The palette budget in
Resources is what actually limits how many *figures* there can be, and bank 1
holds three; there is a spare attic slot for a third map.

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

So the sequence is: loading, then the benchmarks, then the display. The
loading screen is therefore the ROM's text display and not the game's — but
it is dressed to look exactly like the title screen that follows it, and that
took giving up on printing.

**The boot screen writes screen RAM directly, and nothing in it prints.**
`printf` goes through the ROM's screen editor, which can only add to the
bottom of the screen: it cannot put a word on row 8, cannot centre one, and
cannot take a line away again — so a bar it had drawn could never be cleared.
Writing the screen is not a hard thing to do instead. The C65's is **1000
bytes at `$0800`**, one screen code a cell, with that cell's colour at the
same index into colour RAM (`$FF80000`, which is the `$1F800` alias the game
uses later). `screens.c`'s `boot_cell` is those two stores.

**`VICIV.ctrlb &= ~0x80` puts the boot screen in forty columns**, which is
what makes it the same shape as the title screen: same rows, same columns,
same two lines of text, so `SEARCH AND RESCUE` does not move by a pixel when
the game's own display takes over. It is a VIC-III register the ROM has
already unlocked and it is the **only** display register touched before
loading — the rest of `vic4_init` still has to wait, because something in it
leaves the Kernal unable to open a file. Verified by booting: every map loads
and the title comes up.

The editor goes on believing the display is eighty wide, which costs nothing
while nothing prints. The one thing that still does is the startup benchmark
report, so `screens_boot_restore()` hands the eighty columns back before it
(`main.c`, under `#if REPORT_SECONDS`) — and **the report is not printed at
all when `REPORT_SECONDS` is 0**, because scribbling a table over a title
screen nobody asked to read is worse than not printing it. `profread` takes
the same figures out of memory either way.

That also **freed 6.5 KB of the 32 K**: with nothing in the default build
calling `printf`, the whole formatting machinery drops out at link time and
the PRG went from 26728 bytes to 20219. A `REPORT=120` build pulls it back in
and still fits.

The old note here said the bar's block had to be `#` because *160 prints
nothing* — the shifted space went out through `putchar` for months and drew
thirty invisible characters on every boot. That is still true of Calypsi's
output path, and worth knowing if anything is ever printed again. It stopped
mattering the moment the bar became a store: **160 written into screen RAM is
the solid block it always should have been.**

**Once the game's display is up, nothing may `printf`** whatever the boot did:
the Kernal's screen editor writes colour RAM, and the game is using it.
`load_resources` reports failures through `loader_error()`, and
`screens_load_failed` writes them onto the boot screen where the bar was.

**`vic4_init` leaves the display in text mode, not on the framebuffer.** Bank
1 has never been written when it runs, so showing the 3D view there would put
a screenful of uninitialised RAM between the boot screen and the title.
Everything that wants the view asks for it with `vic4_view_mode`, which
`flight()` already did.

## The campaign

**Nothing about a mission is compiled in.** `campaign.yaml` names the mission
files, each file in `missions/` describes one mission, and each mission names
the world it is flown over and the figure that stands in it.
`tools/campaign.py` turns the lot into two things:

- **`campaign.bin`**, which goes on the disk and is read *first* at boot,
  because it says how many maps and how many figures there are to read after
  it. A header, one 24-byte record per mission, and a pool of strings the
  records point into by offset. `campaign_load()` turns those offsets into
  pointers once, into the `missions[]` array the rest of the game already
  used, so `screens.c` and `main.c` did not change at all.
- **`build/campaign.mk`**, which the Makefile includes. The map list, their
  ids and slot numbers, the sprite sheets, the disk's name and the rules that
  generate and convert them all come out of it. Those used to be three
  hand-kept lists in the Makefile that had to agree with `MAP_COUNT` in
  `loader.h` and with the table in `mission.c`, and nothing checked that they
  did. GNU make notices the include is out of date, remakes it and restarts,
  so it bootstraps on a clean tree.

**The maps are collected, not listed.** A mission names its world; two
missions over the same island cost one map slot. Same for figures.

**Everything is checked on the PC**, because the machine has no way to report
anything at that point and no room for the code to do it. The tool uppercases
the text and refuses a character the screen cannot draw, wraps the brief to
the briefing's width and refuses it rather than running off the page, refuses
a fix that is off the map, refuses `cargo` without a `lost` line to go with
it, and refuses a campaign too big for the buffer. The three ceilings —
`MISSION_MAX`, `MAP_SLOTS`, `SPRITE_MAX` — are spelled once in the C and once
in the tool, and a disagreement is caught in the tool.

**The brief is prose and the tool wraps it.** Greedy, at 36 columns, which is
what the briefing draws; the two shipping missions wrap to exactly the three
lines the hand-written table used to hold, which is what made this refactor
checkable.

**`cargo` alone says what kind of mission it is**, as it always did: absent is
a camera mission, present is a delivery. There is no `type:` field, because
two fields that can disagree is exactly what the game's own design avoids.

**`hidden:` is the one other field a mission can carry**, and `thermal` is the
only value beyond `no`: the figure is under the snow and is not drawn until
the thermal camera is armed. It rides in the record's byte 23, which was spare
until it existed. See The thermal camera.

**Bank 1 is full to the byte now.** `SPRITE_MAX` figures at 1028 bytes each
from `$1DC00`, then `MAP_SLOTS` overview maps from `$1EC00`, which end exactly
at the colour RAM alias — `OVERVIEW_STORE` moved up a kilobyte to make room
for a third figure. Both ends are `#error`-checked in `src/sprite.c`.

The old note here said `map.bin` and `mission.bin` were "not written yet".
This is them, under one name.

Controls, which follow a real drone's (see `documentation/real-drones/`):
`W`/`S` forward and back, `A`/`D` yaw, `R`/`F` climb and descend, `Q`/`E`
gimbal up and down, `1`/`2`/`3` the speed limiter (cinematic, normal, sport),
`SPACE` to file a report, `RETURN` to release the cargo, `T` to arm the
thermal camera, `RUN/STOP` to abandon
the mission, and `M` to mute the engine — see Sound, where the same key mutes
the tune on every screen that is not a flight.

**`P` shows the frame rate, and nothing on any screen says so.** It is off
when the game starts and kept for the session like the mute. Deliberately
undocumented in the game: it is a thing to watch while working on the
renderer, not an instrument on a drone, and the briefing has no spare row for
a line about it. It is row 5 of the matrix, which is why `input.c` scans six
rows rather than five. **Confirmed on the machine on 17 Aug 2026**: nothing
headless can press a key, so the emulator could only ever prove the drawing
half of it, which it did by defaulting the flag on for one build.

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

`src/input.c` scans six matrix rows — row 0 for `RETURN`, row 7 bit 7 for
`RUN/STOP` — and returns held keys and fresh presses from one scan, because an
edge only means anything against the scan before it and two scans in a frame
would see none. `T` is row 2 bit 6, a row it already read, so the seventeenth
key cost a probe of nothing — but it did cost the mask its width: see
`keymask` under The thermal camera.

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

`src/weather.c` owns an overcast sky, the rain, the snow, and the one
pseudo-random stream the weather uses (the wind's gusts come out of it too).
Which weather a flight gets is a `weather` field in the mission table, like its
cargo and its figure; mission two rains and mission three snows.

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

**Snow is that same loop with different constants**, which is all it was ever
meant to be: the same forty-eight drops, the same four layers, the same two
bytes of state each, the same drawing loop. Two 4-entry tables hold the whole
difference — snow falls at about a third of the rate and a flake is a dot
where a raindrop is a streak — and they are indexed by `weather - 1`, which is
why the falling kinds have to stay contiguous above `WEATHER_CLEAR`. Its cost
over rain's is one multiply a flake a frame, about 4000 cycles or a tenth of a
millisecond; that is arithmetic rather than a measurement.

Three things are not in a table:

- **snow is blown sideways and rain is not.** A raindrop falls too fast to be
  carried anywhere and keeps the fixed one-pixel-per-two-rows lean it always
  had. A flake is carried `drift` columns for every row it has fallen, so its
  column is a function of its row and needs **no state of its own** — and at a
  length of one or two pixels there is nothing to lean *within*, which is why
  the two mechanisms do not collide. The column is folded back into range
  rather than dropped at the edges, because a snowfall blown off one side
  leaves that side of the picture empty within seconds.
- **the drift is the wind across the *view*, not the wind.** The flight works
  it out every frame in `fly`, because turning the drone moves it quite as
  much as the wind veering does: the lateral axis is `(-sin, cos)` of the
  heading, exactly as `src/sprite.c` projects a figure, so a wind blowing
  towards `to` contributes `sin(to - angle)`. Fly into the wind and the snow
  comes straight down; turn across it and it streaks off to one side.
  `SNOW_DRIFT` in `main.c` is the whole scale, and at 7 a flake crossing the
  full picture is carried about forty of the hundred and sixty columns.
- **snow has to know about the thermal camera**, which rain never did. Left in
  its own near-white it is the brightest thing on a screen whose entire point
  is that a body is the brightest thing on it — so `weather_thermal` recolours
  the pair cold while the camera is armed. This is the one place the two
  features touch, and it is exactly the mission that has both.

**Rain and snow share palette entries 240/241**, which settles the old question
about spending two more on snow: a flight has one weather, so there is never a
frame with both kinds on it. A clear flight still leaves the pair to the
overview crosshair.

A report counts only if the survivor was **on screen in the frame just
drawn** and within ten map cells (`sprite_reportable`). On screen is half the
test on purpose: a report should mean you looked at them, not that you flew
past with the camera pointed somewhere else.

## The thermal camera

`T` arms the drone's second sensor and `src/thermal.c` is the whole of it.
Nothing about the picture changes: the march samples the same maps, the
billboard is projected the same way, the framebuffer holds the same bytes.
What changes is what the palette entries *behind* those bytes are. It costs
`vic4_set_entry` at the toggle and **not one cycle a frame**.

That is only affordable because the shared ramp gave terrain and figures
separate indices (see Resources). A map's colours are 16..173 and every
figure's fifteen are above them, so the two recolour apart without the
renderer learning there is a second mode at all.

- **the ground becomes a cold monochrome of its own luminance**, not a flat
  silhouette. The sun's shading is the shape a pilot flies by, and there is
  nothing to fly at in a silhouette -- taking the luminance carries all of it
  across. It is read back out of the loaded palette rather than computed from
  `maps/palette.yaml`'s band boundaries, so a new band needs no change here;
  that is the same reasoning that has the clear sky restored from the palette
  rather than recomputed. Nine or ten distinct colours survive the VIC-IV's
  four bits per channel, blue nybble 1..7 on all three maps.
- **the figure is not tinted, it is replaced.** Every non-zero pixel of the
  drawing buffer becomes one reserved hot index, so what is drawn is a heat
  signature and not a person in a coat. A kilobyte at the toggle against a
  test per pixel in a loop that already costs 170 cycles each -- and
  `sprite_draw` never learns the mode exists. Putting it back is the DMA out
  of bank 1 that `sprite_select` already does, so nothing remembers what any
  pixel used to be.
- **`THERMAL_HOT` has to be left out of the sweep, and this is the trap.** It
  sits at the top of the same run of entries, so the first version painted it
  cold along with the ground: the figure was projected correctly, clipped
  correctly, drawn correctly -- and came out **the same blue as the hillside
  it was lying on**. Leaving it alone is also what makes the restore a single
  `vic4_set_range`.
- **the sky goes nearly black**, and the sky belongs to the weather rather
  than to the palette, so stowing the camera calls `weather_sky()` -- a rainy
  flight is overcast and the file it was loaded from is not.
- **the panel is not something the camera is pointed at**, so its artwork
  (242..255), its ink (1..5) and the precipitation pair (240/241) are all
  outside the sweep and stay readable. **The overview map does go cold**,
  because it is drawn in the colourmap's own indices; that is a consequence
  and not a decision.
- **falling weather is recoloured by the weather and not here.** The pair is
  outside the sweep on purpose — 240 is the overview crosshair on a clear
  flight and must stay white — so `thermal_set` tells `weather_thermal`
  instead, and snow goes cold only when there is snow. See The weather.

**The briefing names `T` on every mission**, in the `CONTROLS` list it already
drew `1 2 3` on — a control the game does not name is a control nobody has,
and mission three cannot be finished without this one. Mission three's brief
says it a second time in its own words. Arming it in the air puts
`THERMAL CAMERA ON` on the panel's message row, the way the mute and sport
mode do; there is no persistent readout for it and it needs none, since the
whole picture going cold says it better than a box could.

**The briefing page starts on row 0 now**, which no other screen does. It was
already full — four labelled blocks, each with a blank row before it, and the
controls ending on the row above the prompt — so the seventeenth key had
nowhere to go but the top margin, and the display's own border is the margin.
The separators are rows 1, 5, 9 and 12, and there are no others.

**A figure can be hidden from the optical camera**, which is what makes the
sensor a thing you need rather than a thing you can look through.
`hidden: thermal` in a mission file turns `sprite_show` off at launch, and it
is asked in `sprite_prepare` rather than in `sprite_draw` **so that a buried
figure is not "on screen" either** -- `sprite_reportable` is false, and no
report can be filed on somebody the pilot has not been shown. Mission three
is the one that uses it.

**It cost the key mask its sixteenth bit.** All sixteen were taken, so
`keymask` in `input.h` is `uint32_t` now. `T` is row 2 bit 6, a row `input.c`
already scanned, so the scan itself did not grow. The whole feature is about
820 bytes of the 32K, which is what took a default build from 89.7% of the
program area to 92.2%.

**Verified by diffing frames**, since nothing headless can press a key: a
flight that armed the camera and stowed it again renders the 3D view
**pixel-identical** to one that never touched it. Only the wind readout
differed between the two runs, and that is seeded off the clock.

## Sound

Two things make a noise and never at the same time: a **tune**, which belongs
to the pages, and an **engine note**, which belongs to the flight. Every page
has the tune under it and only the air is without it. Both hang off one
interrupt.

**`M` mutes whatever the place you are in sounds like** — the tune on a page,
the motors in the air. Two settings and not one, because wanting a quiet
flight and wanting quiet menus are different wants; both live in `main.c` and
are kept for the whole session, so muting once is enough. The menus carry the
state on a line under the prompt (`screens_music`); the flight has no room for
one and says it on the panel's message row at the moment the key is pressed,
the way arming sport does. `M` is why `input.c` scans a fifth matrix row —
row 4, `$EF`, bit 4.

**The interrupt chains rather than taking the vector.** `src/audio_irq.s`
saves `$0314` and jumps to it when it is done, so the ROM's raster compare,
keyboard scan and jiffy clock go on exactly as before and nothing here has to
know which line the ROM asked for. Two things about that vector, both from the
raster-split experiment written up under Full-colour display: the C65's
dispatcher at `$FA23` has already pushed A, X, Y, Z **and the base page
register** by the time it jumps through `$0314`, so a handler reached from
there may use every register freely; and taking `$FFFE` instead means knowing
that the ROM's exit pulls five bytes and not the C64's three. The handler sets
the base page to 0 around both callees, because zero page is wherever B says
it is and B is the ROM's business. `audio_begin()` installs it once, at boot.

**Nothing may interrupt a measurement, and the sound is what made that
matter.** `profile_calibrate` times sixteen raster lines — about a
millisecond — and one interrupt inside that window scales every figure the
profiler prints for the rest of the run, the frame rate on the panel included.
At 50 Hz that is a real chance on any given boot. `profile_irq_off`/`_on` in
`bench_asm.s` are an `sei`/`cli` pair, wrapped around the calibration and the
benchmarks; neither wants anything from an interrupt. The ROM's own handler
was always a smaller version of the same risk.

**Both SIDs are written throughout.** The MEGA65 has one per stereo channel,
`$D400` left and `$D420` right, so everything that makes a sound writes both.
On a C64 `$D420` is a partly decoded mirror of `$D400` and the second write
lands back on the same registers, which is what keeps `music/`'s standalone
program working there. If it ever comes out of one speaker on real hardware,
`SID2_BASE` in `audio.h` and `SID2` in `music/player.asm` are the two
constants to move.

### The tune

A three voice SID tune plays under **every page** — the loading screen, the
title, the mission list, the briefing and the debrief. `src/music.c` is the
whole of the game's side of it: `music_set(0|1)` around the flight. Turning it
on again rewinds rather than resumes — it is a title screen, not a radio — and
`music_set` is idempotent, so a page that is already musical can say so again,
which is what lets the tune run unbroken from a debrief into the next
briefing.

**The flight is the one quiet place, and that is a decision about the game.**
The air has the motors, the wind and nothing else: a search is meant to sound
like weather. It costs nothing to have it either way — the flight never calls
the player — so this is taste, and the pair of calls either side of `flight()`
in `main.c` is where to change it.

**The tune is written in ACME and the rest of this is Calypsi.** `music/`
holds `player.asm` (the engine: patterns, instruments, arpeggios, vibrato,
pulse sweep) and `music.asm` (the tune), plus a `main.asm` that makes the pair
a standalone C64 program. `tools/acme2calypsi.py` translates them into
`build/music_asm.s` at build time — generated, not checked in, so `music/` is
the only copy of the tune there is. The converter needs Python and nothing
else; **ACME is needed only to check it**.

- **the check is what makes the converter trustworthy**, and it is a command:
  `make checkmusic` assembles the same source with both assemblers at the same
  origin, with the linker's zero page pinned to the addresses the ACME source
  picked, and compares. 2473 bytes, byte for byte. A translator between two
  assemblers is either exactly right or quietly playing a different tune, and
  this is the difference. **Run it after touching either file.**
- **what the converter does not translate is `--zp`.** The player picks its
  two zero page pointers by hand at `$fb`, which a C64 program may do and a
  program sharing zero page with a C compiler and a live Kernal may not.
  Those two constants are dropped and the names put in a `zzpage` bss section
  for the linker to place — `zeroPage` went from 88.9% to 92.1% and has ten
  bytes left. Everything else is a translation and is proved to be one.
- it stops rather than guessing. Anything in the ACME source it does not
  recognise is an error, not a line passed through to be mis-assembled.

**It costs 2652 bytes of the 32 K** — player, tune and all — which is what the
`printf` reclaim was for. See the 32 KB note under Memory map for where the
rest of that reclaim went.

**The music is running while the resources load**, which is the one place it
touches something timing-sensitive. It is fine in the emulator and should be
fine on the machine — the D81 comes off the SD card through the F011
controller rather than the serial bus — but if a load ever fails on hardware
and nothing else explains it, moving `audio_begin()` below `load_resources` is
the thing to try first.

### The engine note

**There is no player, no patterns and no rhythm.** Three voices are gated on
at launch and never gated off until the flight ends; the only thing that ever
changes is their pitch. `src/engine.c` decides what the pitch should be and
`src/engine_asm.s` walks it there from the interrupt, twenty-four frequency
units at a time, fifty times a second — so opening the throttle spools the
motors up over about two thirds of a second instead of stepping. A launch
starts the motors cold at 30 Hz and they are heard to come up.

- **it starts by pulling the gates down, and that is not a formality.** The
  first version of this made exactly one click at launch and then no sound at
  all for the whole flight. Two SID rules were behind it, either of which is
  enough on its own:
  - **an envelope only triggers on a 0 → 1 edge of the gate bit.** The tune
    leaves all three of its voices gated on — read straight out of the
    player's own record of what it last wrote — and `music_set(0)` took only
    the master volume away. So the gates were already high, and writing a
    waveform with the gate bit set changes the tone without ever starting a
    note.
  - **raising the sustain level during the sustain phase drains the envelope
    to zero.** That phase holds only while the counter *equals* the sustain
    register; anything else keeps it falling. The tune's bass and lead sit at
    sustain 10 and 11, the engine asks for 14 and 12, and with no gate edge to
    start a fresh attack both voices simply drained away. The click was the
    master volume coming back up over envelopes on their way to nothing.

  `gate_low()` clears every register on both SIDs and holds them low for a
  frame — the same hard restart the tune's own player does before every note —
  which also puts `$D417` back, where a voice routed into a filter with no
  filter mode selected in `$D418` is a third way to write a note and hear
  nothing. `music_set(0)` drops its gates now too; the engine does not rely on
  that, but leaving them up was the wrong thing for *stop* to mean.
- **it is written in assembly because it runs in an interrupt.** A C function
  called from one would use the same zero page scratch and software stack as
  whatever the main program was in the middle of, and the renderer is in the
  middle of something for nearly the whole frame. The player gets away with it
  by being assembly with its own state; the engine's tick is assembly for the
  same reason, and the decisions stay in C where they read.
- **the note follows what is being asked of the props, not what the drone is
  doing.** The speed mode moves it on its own, the way the battery drain does:
  sport works the motors harder whatever the sticks are doing. The wind moves
  the drone and does not move the note at all.
- **three voices: a sawtooth, a pulse sixty units above it, and a triangle an
  octave down.** The detune is the whole trick — the two beat against each
  other about four times a second, and that throb is most of what makes it
  read as rotors rather than as an organ. Sustain is the only per-voice volume
  a SID has, so the balance between the three is three numbers in
  `engine_start`.
- **the numbers were never listened to while they were written**, which is
  why every one of them is named and gathered at the top of `engine.c`:
  `idle_hz`, `move_hz`, `CLIMB_HZ`, `DESCEND_HZ`, `DETUNE` and `RATE` are the
  whole of what a flight sounds like. `RATE` and `DETUNE` are spelled twice,
  in the C and in the assembly; keep them in step. **`ENGINE_VOLUME` is 7 of
  the SID's 15**, halved on 15 Aug 2026 after hearing the first version: full
  was too loud to fly under. The tune keeps its own level, since the two never
  sound at once and each sets what it wants as it starts.
- **the battery warning borrows voice 3**, and it is the only thing in a
  flight that is not the motors: one note when the pack goes yellow at 25%
  and a higher, longer one when it goes red at 10%. **Heard on the machine on
  17 Aug 2026 and it is right** -- which is the only way it could be checked,
  since nothing about a SID can be read back from a headless run. The colours
  were confirmed with it. Voice 3 is the triangle
  an octave down, the quietest of the three and the least missed for the
  third of a second this takes; the other two carry the note straight
  through. `engine_beep_left` is shared with `engine_asm.s`, which leaves
  voice 3's *pitch* alone while it is set — otherwise the interrupt would
  write the note over the beep fifty times a second — and it is counted down
  by the flight loop rather than by the interrupt, so every SID write in a
  flight stays on one side of the fence. It gates down before it gates up,
  for the reason two bullets above, and hands the voice back with the same
  registers `engine_arm` gave it. A muted flight makes no sound at all,
  warning included: `M` means quiet.
- **muting mid-flight resumes where the throttle left it**, not from cold.
  `engine_start(heard)` is the launch, and a muted one never touches the SID
  at all rather than making a click and falling silent; `engine_set` is the
  key.
- `engine_target` is written in two halves by the flight loop while the
  interrupt may be reading it. A torn read costs one tick of walking the wrong
  way — a fifth of a semitone — and the next tick corrects it. An `SEI` every
  frame to prevent that would cost more than the fault.

**Measured cost: none.** Flown twice over the same ground with the wind seed
pinned, once with the engine armed and once without: 11.7 fps both times, and
the panel readouts identical. The arithmetic agrees — the tick is about 300
cycles fifty times a second, four hundredths of one per cent of the CPU.
(Frame rates between *unpinned* runs differ by a couple of tenths because the
wind blows the camera somewhere else; that is the noise floor, and it is what
made pinning the seed necessary to say anything at all.)

**Nothing here can be heard from a headless run, and one readback that looks
like it would help does not.** `$D41B` and `$D41C` are the only SID registers
that read back — the voice 3 oscillator and its envelope — and **xemu returns
garbage for both**: three reads a few microseconds apart came back `1d 7b 8a`,
which no oscillator at 50 Hz can do. An hour went into an A/B test built on
those numbers before that showed up. What *is* trustworthy is ordinary RAM:
`engine_freq` walking to its target proves the interrupt half is running, and
the tune's `v_wave` proves what state it left the gates in. Reason from those
and from the SID's documented behaviour; do not trust a register readback in
the emulator.

**So the last word on anything audible is Johan's.** Both things written here
by reasoning alone -- the engine's gate handling and the battery warning
borrowing voice 3 -- came back right the first time when he listened to them,
which is what reasoning from the documented behaviour is worth. It is still
worth saying plainly which half of a sound change has been seen and which has
only been argued.

## The billboards

`src/sprite.c` draws one world-anchored 2D figure into the framebuffer after
the terrain, scaled by distance and clipped against the heightfield. It is the
software sprite `documentation/vision.md` asks for, and the mechanism is meant
to carry the rest of them — campfires, crates, hazards.

There are three figures now and the flight draws one of them — or none, if
the mission says the ground is over it; see The thermal camera. **Every figure is
parked in bank 1 at load time and `sprite_select` DMAs the mission's own down
into the one near buffer**, because the 32K has room for a kilobyte of pixels
and not for two — and a kilobyte of DMA once a flight is nothing, while a far
pointer in the drawing loop would be paid per pixel forever. Adding a figure
is a `figure:` line in a mission file: the campaign collects the sheets, names
them in order, and `sprite_load` reads as many as there are. `SPRITE_MAX` is
the ceiling and it is bank 1's, not a choice — see The campaign.

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

## The map generator that is not here

**Generating the maps on the MEGA65 was built, measured and removed.** It
worked: the whole of `tools/genmap.py` ran on the machine, all eleven passes
bit-exact against the PC, the per-pixel work in 45GS02 assembly, 64 seconds a
map. But that is a 512² pipeline, and the game ships a 1024² colourmap
computed from a 1024² field — four times the work at every pass, so **~255
seconds a map** against 66 to load both off a floppy and 31 off SD.

The code is on the **`mega65-mapgen`** branch and
`documentation/on-device-maps-experiment.md` is the write-up, with the verdict
at the top. Read that before proposing it again. What it leaves behind here is
the toolchain and hardware knowledge under Performance and Gotchas, all of
which was learned doing it and none of which is about maps.

**Two capabilities went with it, and both are written up on the branch** if
they are ever wanted: chaining from one program to the next by typing into the
C65's keyboard queue (`$02B0`, count at `$D0`), which works because attic RAM
survives a program load; and banking BASIC out to put a program's BSS at
`$A000-$CFFF`. The second is what the game still has not done — see the Memory
map notes.

## Resources

`tools/convmap.py` turns a 1024x1024 heightmap/colourmap PNG pair into `<stem>.hgt`, `<stem>.col`, `<stem>.pal`, `<stem>.ovr`, `<stem>.pnl` (the panel artwork, with `--panel`; see The panel) and one billboard file per sprite sheet — the two maps crunched, the palette raw, and the figures cut out of the sheets (see The billboards). The sources need no quantisation: the heightmap is 8-bit greyscale and the colourmap is already a palette image. Palette bytes are nybble-swapped for the `$D100`/`$D200`/`$D300` registers, and the sky gradient goes in indices 224-239, which the colourmap never uses.

`convmap.py` takes the two map sizes as arguments; the Makefile passes
`HGT_SIZE` and `COL_SIZE`. The sprite sheets are one **comma-separated**
argument, which `build/campaign.mk` fills in from the sheets the missions
name, and the first one's output is `<stem>.spr` while the rest are `.sp2`,
`.sp3` and so on — the order the campaign numbers its figures in. Every map's
conversion writes its own copy of them and they are identical under
`--shared`, so slot 0's are the ones copied to the disk.

**The palette is what limits how many figures there can be**, and generating
the maps loosened it. Each figure claims fifteen entries, and what is left
over depends on the colourmap: the hand-drawn one uses about 170 of the 224
indices below the sky and left **12 free** after two figures, while a
generated map under `--shared` reserves the ramp's 150 and the system's low
sixteen. Three figures and the thermal camera's one reserved hot entry leave
**4 free**, which `convmap.py` prints at the end of every run; it refuses
rather than quietly painting terrain in a sprite colour. A fourth figure needs
eleven entries that are not there.

**The sprite sheets are not all the same shape**, and `sheet_grid` counts the
rows off the sheet rather than assuming them: a cell is square, so the row
count is what makes it so. The skier's sheet is one row of four poses and the
other two are two of four. It finds the title strip by the solid rule under
it — three or so rows dark across the *whole* width, which the title's own
lettering never is however dense a row of it looks — and skips a band starting
at row 0, which is a sheet's own border and not a separator. **The old version
of this got the survivor and medical sheets wrong and it did not show**: it
took the first row that was merely half dark, which put the cell window
thirty pixels high and clipped the medical figure's feet off. Fixing the
detection is what changed that figure from 32x31 to 30x32.

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
is the C. It was written that way for a port to the machine that has since
been measured and abandoned (`documentation/on-device-maps-experiment.md`),
and it stays: integers are what makes a map reproducible byte for byte, which
is what `tools/checkview.py` leans on;
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

**There are three kinds now: `pyramid`, `house` and `road`.** A material is a
band in `maps/palette.yaml` and a `BUILT_*` code in the `built` field, looked
up in one table in `colourise` — so a fourth is those two lines and a builder.
`maps/template.yaml` is one of each, and `maps/testitems.yaml` is a map that
exists only to be flown over and looked at.

- **a `house` is a flat-topped rectangle**: a ring of wall one map cell wide
  around a dark `roof` band. There is no roof *shape*, because from a drone
  what you see is the plan and a pitched roof at this scale is two pixels of
  slope the march samples over — what says roof is the colour. `medium` is
  6 by 4 cells and stands 12 height units, which is what the survivor
  billboard is; nothing here is to scale, for the same reason `SPR_WORLD_H`
  is not.
- **a `road` is `from:` and `to:` and a width, and it finds its own way.** It
  is **paint and not terrain** — the only built thing that does not move the
  ground — which is what keeps it clear of the trap that a river measured
  against the map it has already cut digs itself a canyon. There is no
  feedback here to get wrong.

**How a road chooses its route**, which is the only part of the generator that
searches rather than computes: a Dijkstra over a coarse grid of two map cells
a node, then the corners taken off. The cost is `ROAD_*` at the top of
`genmap.py` — a flat node costs 10, height above sea costs up to 40 at the
ceiling, every height unit of rise between one node and the next costs 3, and
a little noise keeps a road over flat country from being a ruled line.

**The ceiling is a price and not a wall**, and it took a real map to see why.
Above `ROAD_CEILING` — 55% of the way up that map's own land — a node costs
`ROAD_ABOVE`, forty flat nodes, so a road will go eighty map cells out of its
way rather than cross one node of high ground. That is the "go round the
mountain" rule. It was a hard refusal first, and the first hilltop landmark
with a road to it proved that wrong: the author had asked for a road to a
place and been told the place was unsuitable. A price says what was meant —
climb when climbing is the only way to get where you were sent, go round when
it is not.

**Water is still a wall**, there being no bridges, and it is the only one: a
road whose ends water has separated says so and stops rather than quietly
drawing a line through a lake.

**The order items are listed in does not matter**, and two things make that
true. A road is routed against the ground *as the generator left it* rather
than against whatever has been built on it so far — the rule the river taught,
in the one other place this file measures against a map it is also changing —
so a road to a pyramid does not have to climb the pyramid, and no route
depends on where it sat in the file. And a road never paints over something
already built, so it stops at a wall instead of driving through it. A building
does paint over a road, which is the right way round.

A priority queue is not the kind of code the rest of that file is. The port to
the machine it was written for was measured and abandoned (see below), and
what the integers are still for is reproducibility: every number in the search
is one and ties break on coordinates, so a seed and a map file give the same
road every time.

**The disk is generated maps now, and there are two of them.** The map list
comes out of the campaign — each mission names its world and
`tools/campaign.py` collects them, so the Makefile's `MAP_YAMLS` is generated
rather than kept by hand. Each is run through `genmap.py` and then
`convmap.py` into `build/map0.*` and `build/map1.*`, and which slot a mission
flies is the order its map was first named in. The hand-drawn pair in
`resources/` is no longer built into anything — it stays as the reference the
sun was measured against and the pyramid copied from.

**A map file describes a world and nothing else** — `maps/island.yaml` is its
seed, its shape and the things built into its terrain. What is flown over it is
a *mission*, which has a map and is not one: two rescues could be flown over
the same island, and the map file would not change. The missions live in
`missions/`, one YAML file each, and `campaign.yaml` lists them -- see The
campaign.

**Several maps fit only because they are generated.** The hand-drawn pair was
661 KB crunched on its own; the three generated ones are 446 KB together, so a
disk that used to hold one world now holds three with **1246 blocks free**.
Two thirds of that reduction is `COL_SIZE`: see the note under Performance
about why 512 is the default.

**How well a map crunches is a fact about the country, not about the
generator**, and the spread is wide enough to plan around:

| | crunched | |
|---|---|---|
| `island.yaml` | 60 KB | flat sea and gentle land |
| `plains.yaml` | 117 KB | |
| **`avalance.yaml`** | **268 KB** | a mountain range with no water in it |

That one map is 60% of the disk and 60% of the boot — about twenty-five
seconds in xemu now against eight for two maps. Ruggedness is part of it and
not the whole: the same map at `ruggedness: rolling` comes to 187 KB, a saving
of 80 KB, and is still three times the island. **What actually crunches is
flat ground**, and a mountain range has none. If the boot ever has to come
back down, that is the lever — a map with water or plains in it, not a knob.

**Switching between them is 512 bytes of table.** A map's whole location lives
in the renderer's plane lookups: the march reads a bank byte out of
`vx_hplane_y`/`vx_cplane_y` on every sample, so `voxel_set_map(slot)` rebuilds
those and the march is looking at another world. `map_use(slot)` wraps that
with the two other things a map is — **its palette, because a climate *is* a
palette** (the shared ramp means every map holds the same indices and puts
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
- attic RAM is cut into **three** 2 MB slots (`MAP_SLOT` in `loader.h`), and
  the avalanche map took the third. **All three are full now**, as are all
  three figure slots in bank 1, so a fourth of either needs the ceiling raised
  rather than a spare filled. Nothing has to be kept in step by hand:
  `MAP_SLOTS` is the ceiling, the count comes off the disk with the campaign,
  and `tools/campaign.py` refuses a fourth.
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
`documentation/on-device-maps-experiment.md`). Two consequences worth carrying:

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
(Those figures are the generator's, measured before it was removed; the middle
two rows are the reason the last one matters.)

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
table to the Kernal's text screen at startup and waits for a key. **The wait is
`REPORT_SECONDS` and it defaults to 0 — and at 0 the report is not printed at
all now**, because the boot screen is a title screen a table would be scribbled
over, and because not calling it is what lets `printf` drop out of the link
entirely (6.5 KB; see the 32 KB note). `make REPORT=120` at the machine, where
that table is the only way to read the attic RAM figures: it restores the ROM's
eighty columns first, prints, and waits. It is boxed in on both sides: after
the resources, because `profile_init` takes the timers the Kernal reads a disk
with, and before `vic4_init`, which takes the text screen away. The figures
come out identical to `profread`'s, so either route can be trusted.

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

**Two byte reads combined into a word in one expression: one of them is
dropped.** `(uint16_t)p[at] | (uint16_t)p[at + 1] << 8`, with `at` a `uint8_t`,
compiles to a *single* indirect load — the high byte — and fills the low byte
with the high byte of the address it just worked out. Every string offset and
both fixes in the campaign came back with the right high byte and a low byte
of `$82`, which is simply where the buffer sits. Hoisting the two halves into
locals fixes it, and so does widening the index to `uint16_t`; the listing
shows it in one look, because there is no second `lda (ptr),z`. See
`word()` in `src/mission.c`.

**A call with no prototype in scope returns `int`, and `int` is sixteen bits.**
`rnd_state()` returned a `uint32_t`; its caller in the map generator did not
include the header that declared it, so it took the low half and left garbage above it — and the
map generator handed its second stage the wrong random state, which changed
every colour on a map that still looked entirely plausible. **This is the one
in the family that the compiler does warn about**, as `implicit declaration of
function`, and it links without complaint because the symbol is real. Read the
warnings; a 32-bit return that arrives half right is this and not the codegen.

That one cost five emulator runs, and the reason is worth more than the bug.
The word came out wrong at the far end of a handover, so three rewrites went
into the *move* — byte stores, a far store, a DMA — and all three moved the
same wrong word. **When a value is wrong after a move, print it at the source
before touching the move.** One run would have done it.

Calypsi 5.18 emits a call to `_FillZPQ` — a runtime helper that is in none of
its libraries — when a function call appears inside a 32-bit expression. The
link fails with an undefined symbol. Hoist the call into a variable.

Three things that sound like optimisations and measured slower **for the
renderer**: `--no-cross-call` and `--strong-inline` (615 ms a frame against
533 ms), and using the 45GS02's 32-bit `ADCQ` to step the ray position in one
instruction instead of two 16-bit adds (68.5 ms against 64.7).

**Both of those flags are per-directory decisions, not global ones.** The map
generator wanted the opposite answer on one of them, and the measurements are
worth keeping because the next hot loop written in C will too:

- **`--no-cross-call` was worth 16%** (43.0 seconds of colour to 36.2).
  Calypsi shares common instruction runs between functions by turning them
  into `jsr` fragments, and `mulhi32` came out as a chain of *six* of them with
  `pha/phx/phy/phz` around each. That is a good trade where the hot loop is
  already assembly and a bad one where it is still C. It cannot change what is
  computed — it is a speed-for-size flag and nothing else.
- **`--strong-inline` produces the wrong answer.** It was worth 9% and it
  generated a different map: colour came out `C24E6E26` against the PC's
  `D69C51D9`. `MATH` is volatile, so the write-write-read of one multiplier
  wrapper is safe; but three `lerp16`s in one expression are three *calls*,
  and C only promises those are indeterminately sequenced, not that their
  bodies stay unbroken once inlined. Merged into a single basic block they
  trample each other's operands. **The `jsr` per multiply is the price of the
  multiplier being one piece of shared hardware addressed through memory; the
  way out is assembly, not an inliner.**

**Read the product as words and longwords, not out of `multout[]`.** The
product's eight bytes at `$D778` are consecutive and the 45GS02 has 32-bit
loads, so `(a * b) >> 16` is one `ldq` from `$D77A` — but written as
`multout[2] | (multout[3] << 8) | …` it compiles to four loads, three shifts
and three ors, about twenty instructions. `fixed.h` names the four useful
windows as words and longwords of their own is the whole difference; doing so
took the generator's colour pass from 47.2 seconds to 43.0 and changed no
checksum anywhere.

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
| **h512 c512** | | | the default now: see below |
| h256 c512 | 169 KB | 64.9 ms, 15.4 fps | |
| h256 c1024 | 575 KB | 64.9 ms, 15.4 fps | colour is **free** |
| **h512 c1024** | **661 KB** | **70.6 ms, 14.2 fps** | was the default until 14 Aug 2026 |
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
chunky in the foreground. So h512 earns its place and the next step up does
not.

**c1024 was the default and is not any more, and that is a boot-time decision
rather than a rendering one.** The finer colourmap costs nothing per frame --
that is the whole point of the row above -- but it costs 316 KB of disk, and
with the map generator gone the resources are almost the entire boot: 502 KB
is 66 seconds off a floppy against 186 KB at 24.5. Flown side by side on the
machine the difference is slight, called on 14 Aug 2026 after playing both, so
the default moved to c512 and `make COL_SIZE=1024` still builds the other.
**`tools/checkview.py`'s reference screenshot is of whichever is the
default**, and `tools/convmap.py` reads both sizes out of the Makefile so the
previewer cannot check a resolution the disk is not built at -- which it
silently did once, on the day this changed. The residual h512→h1024 difference is lateral, not along the ray —
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

**A SID that is silent has usually not been gated.** An envelope triggers only
on a 0 → 1 edge of the gate bit, and raising the sustain level while a voice
is already in its sustain phase drains that voice to zero rather than lifting
it. Anything that takes the SID over has to pull the gates down and hold them
there for a frame first, whatever the last owner left behind — see the engine
note under Sound, where getting this wrong cost a flight's worth of silence
and a click. `$D41B`/`$D41C` look like the way to check and are not: xemu
returns garbage for both.

**A measurement that gets interrupted is not a measurement.**
`profile_calibrate` times sixteen raster lines and `profile_bench` times tight
loops; both now run under `sei` (`profile_irq_off` in `bench_asm.s`), because
the title music fires fifty times a second and one hit inside the calibration
window scales everything the profiler reports. If a timing figure ever moves
for no reason, check that whatever was added to the interrupt is outside these
two.

The Makefile deliberately makes every object depend on every header. Without it, changing a layout constant in `vic4.h` leaves stale objects built against the old memory map, and the result looks like a hardware fault rather than a build problem.
