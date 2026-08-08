# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

A MEGA65 heightfield voxel flight simulator / drone search-and-rescue game, written in C (Calypsi) with the rendering inner loop destined for 45GS02 assembly. `vision.md` holds the full technical and gameplay design; `todo.txt` is the authoritative "what's next" and should be updated as work lands.

Currently: a flyable voxel engine at roughly 14 fps, entirely in C.

## Build and run

```sh
make run     # build build/sar.d81 and boot it in xemu
make prg     # skip the disk, run the PRG directly (no resources available)
make clean
```

`make run` launches `xemu-xmega65`, a GUI emulator that blocks until closed. For automated checks, run it headless and screenshot on exit:

```sh
timeout -s INT 100 xemu-xmega65 -besure -headless -sleepless \
    -8 build/sar.d81 -screenshot out.png -dumpmem mem.bin
```

`-dumpmem` writes 384 KB of chip RAM, so `mem.bin[0x10000]` onwards is the framebuffer and `mem.bin[0x40000]` the heightmap — invaluable for telling "the renderer is wrong" apart from "the display is wrong". The emulator runs at real-time speed even with `-sleepless`, so wall-clock timing is meaningful. There are no tests and no linter.

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
| `$10000-$177FF` | 30720 | Framebuffer A (160x192) |
| `$17800-$1EFFF` | 30720 | Framebuffer B |
| `$1F000` | 64 | Blank character for the screen row below the framebuffer |
| `$1F800-$1FFFF` | 2 KB | Colour RAM alias — **do not write** |
| `$40000-$4FFFF` | 64 KB | Heightmap, 256x256 |
| `$50000-$5FFFF` | 64 KB | Colourmap, 256x256 |

Writing past `$1F800` does not fault: it fills colour RAM with pixel data, which the VIC-IV reads back as character attributes and which blanks cells all over the display. That is what forced 192 rows rather than 200 — two full-height buffers need 64000 bytes and only 63488 are available.

Map coordinates are 8.8 fixed point in a `uint16_t`, so the high byte is the cell index and movement wraps the 256x256 map for free.

## Full-colour display

There is no linear bitmap. The screen is a grid of 8x8 characters, each 64 bytes of 8-bit palette indices, and screen RAM holds a 16-bit character *number* whose data lives at `charnum * 64` — an absolute address, not offset by CHARPTR.

Characters are laid out in **column strips** rather than rows (`screen[row][col] = base/64 + col*FB_ROWS + row`), which makes a vertical span a single pointer stepping by 8 for its whole length:

```
address(x, y) = base + (x >> 3) * FB_STRIDE + (x & 7) + y * 8
```

The renderer relies on this everywhere. Double-buffer flips are just a rewrite of `SCRNPTR` between two prepared screen tables — no pixels move. Terrain spans plus the sky fill write every pixel exactly once per frame, so there is no clear pass.

Two register notes: `CHRXSCL` (`$D05A`) is source pixels per output pixel in 120ths, so **60 doubles the width** and 240 halves it; and the hot registers (`$D05D` bit 7) must be turned off first or any write to a legacy VIC-II register makes the VIC-IV recompute the layout and undo the setup.

## Resources

`tools/convmap.py` turns the 1024x1024 VoxelSpace PNGs in `resources/` into raw `terrain.hgt`, `terrain.col` and `terrain.pal`. The sources need no quantisation: the heightmap is 8-bit greyscale and the colourmap is already a palette image. Palette bytes are nybble-swapped for the `$D100`/`$D200`/`$D300` registers, and the sky gradient goes in indices 224-239, which the colourmap never uses.

Height units are a quarter of a map cell — the maps were downsampled 4x and the heights were not rescaled — and `SCALE_H` in `src/voxel.c` folds that in.

**Reading a SEQ file through the Kernal reports EOF exactly 256 bytes early**, whatever the file's size (verified from 16 K to 64 K, at several chunk sizes). `convmap.py` therefore pads every resource by 512 bytes and `src/loader.c` reads a known length rather than looking for the end of the file. Resources go on the disk as SEQ, not PRG: Calypsi's `_Stub_open` calls Kernal OPEN with the file descriptor as the secondary address, which defaults to SEQ, and SEQ has no load-address header to skip.

`diskutil.rb` (from Fredrik Ramsberg) builds the D81 and refuses to overwrite a file that already exists on the image, so the Makefile deletes and rebuilds the image every time. The name on disk comes from the host file's basename.

## Gotchas

The Makefile deliberately makes every object depend on every header. Without it, changing a layout constant in `vic4.h` leaves stale objects built against the old memory map, and the result looks like a hardware fault rather than a build problem.
