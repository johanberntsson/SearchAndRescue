# Search and Rescue

A 1990s-style heightfield voxel flight simulator for the [MEGA65](https://mega65.org/),
written in C for the [Calypsi](https://github.com/hth313/Calypsi-tool-chains) toolchain,
with the renderer's inner loop in 45GS02 assembly.

![The engine over the pyramid in the VoxelSpace sample maps, with the flight panel and overview map below it](screenshots/screenshot-260810.png)

The eventual game is a post-earthquake drone search-and-rescue simulator: low-altitude
flight over broken terrain, thermal and acoustic sensor modes, payload drops, and
aftershocks that reshape the landscape mid-flight. `documentation/vision.md` has the full design.

## Status

Mission one exists, end to end: a title screen, a mission list, a briefing, a
flight, and a debrief when you find the survivor and file a report on them.

- 320x152 full-colour 3D view, double buffered, over a six-row 40-column text panel
- 512x512 height and 1024x1024 colour maps, exomizer-crunched on the disk and
  unpacked into attic RAM at boot
- Front-to-back ray march with a y-buffer, fixed point throughout, inner loop in
  assembly. It marches 160 rays and each fills the two pixels it owns, which keeps
  the panel's characters a readable 8 pixels wide without paying for twice the march
- About 12.5 frames per second at the default map sizes, up from 0.74 when the
  renderer was all C; a real MEGA65 runs a few percent slower than the emulator
- Altitude, heading, GPS coordinates and frame rate in the panel, with an overview
  map of the whole world and a crosshair showing where you are
- One survivor, waving from the top of the pyramid at 46.713N 8.110E. A software
  billboard drawn over the finished terrain: scaled by distance, and clipped
  against the heightfield with the same y-buffer the ray march already keeps, so
  a ridge in front of them hides their feet
- Drone controls: `W`/`S` forward and back, `A`/`D` yaw, `R`/`F` climb and
  descend, `Q`/`E` gimbal, `1`/`2`/`3` for cinematic, normal and sport speed,
  `SPACE` to file a report once you have them in shot

Getting there meant building a profiler first (`src/profile.c`, read with
`tools/profread.py`) rather than guessing. The compiler's 32-bit multiply turned out
to be 64% of the frame at 2203 cycles a go, against 85 on the hardware multiplier.
See `todo.txt` for what is next.

## Building

You will need:

- [Calypsi 6502 tools](https://github.com/hth313/Calypsi-tool-chains/releases) 5.18 or later
- [Xemu](https://github.com/lgblgblgb/xemu) for `xemu-xmega65`
- Ruby, for the bundled `tools/diskutil.rb`
- Python with Pillow and NumPy, for the map converter
- [exomizer](https://bitbucket.org/magli143/exomizer/wiki/Home), which crunches the
  maps so they fit a D81. `tools/convmap.py` looks for it in `$EXOMIZER`, at
  `tools/exomizer`, in an `ozmoo-z6` checkout beside this project, and on `PATH`

```sh
make run                         # build build/sar.d81 and boot it in the emulator
make PROFILE=0                   # without the instrumentation; use this for timing
make HGT_SIZE=1024 COL_SIZE=512  # map resolutions, powers of two from 256 to 1024
make clean
```

The build produces a D81 with the game as `autoboot.c65`, which the MEGA65 ROM runs at
boot, and the converted maps alongside it as separate files.

## Controls

| Key | |
|---|---|
| `W` / `S` | forward, back |
| `A` / `D` | turn left, right |
| `R` / `F` | climb, descend |

## Layout

    src/            engine: display, DMA, resource loading and decrunching,
                    renderer, input, panel
    tools/          convmap.py builds the map resources, profread.py reads
                    profiles, diskutil.rb builds the disk image
    resources/      source height and colour maps
    documentation/  vision.md, the technical and gameplay design
    release/        a disk image you can just boot
    screenshots/
    CLAUDE.md       memory map, display conventions, hardware notes, measurements
    todo.txt        what is next

## Credits

The sample height and colour maps come from Sebastian Macke's
[VoxelSpace](https://s-macke.github.io/VoxelSpace/), which is also the clearest
write-up of the algorithm.

`tools/diskutil.rb` was written by Fredrik Ramsberg.
