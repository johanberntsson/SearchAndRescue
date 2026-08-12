#!/usr/bin/env python3
"""Check tools/preview.py still draws what the MEGA65 draws.

    checkview.py [screenshot.png] [--mission maps/island.yaml] [--diff out.png]

With no arguments it uses the reference screenshot in documentation/reference/,
so the check needs no emulator and takes a couple of seconds:

    $ python3 tools/checkview.py
    view: 318x152 of 320 at (82,104), 2x   (PAL clips the last column)
    palette: worst colour 1.0 from an entry
    camera: cell 126,123 angle 0 height 126, sub-cell 0,92 by search
    0 of 48336 palette indices differ -- OK

The previewer is a second implementation of `src/voxel_asm.s`, and a second
implementation is a liability the day somebody changes one and not the other:
it would go on drawing a confident picture of a renderer that no longer exists.
Reading the constants out of the C source covers the numbers but not the
algorithm, so this covers the algorithm -- **run it after touching the
renderer.** A failure means the two disagree; which of them is now wrong is for
you to say.

If the renderer or the *generator* is changed deliberately -- a different map
under the same camera fails this just as a different renderer does -- the
reference screenshot is what goes stale. Taking a new one is now one build,
because the disk is built from the generated maps:

    make FLYNOW=1

then run it headless (`-headless` without `-sleepless`; the report screen
holds for REPORT_SECONDS after loading, so kill it around 50 s -- see
CLAUDE.md), read `LAT`/`LON`/`ALT`/`HDG` off the panel in the screenshot, and
save it as `<name>-x<cell>-y<cell>-a<angle>-h<height>.png` -- this reads the
camera out of that name. The panel gives the cell and not the sub-cell, so the
fraction is recovered here by search.

The reference is of **map 0**, the island, which is what `--mission` defaults
to. A second reference of the plains would want `FLYNOW=2` and
`--mission maps/plains.yaml`; one is enough to catch a renderer that has
drifted, and the second map is checked by flying it.
"""

import argparse
import os
import re
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import preview as P                                       # noqa: E402

REFERENCE = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "documentation", "reference", "island-x126-y123-a0-h126.png")

# A colour in the screenshot should land exactly on the palette entry that put
# it there; xemu's rendering of the VIC-IV palette is a unit or so out, which
# is fine. Much more than that means the screenshot was taken of a different
# map, or with a different palette, and every index below would be a guess.
MAX_COLOUR_ERROR = 8


def camera_from_name(path):
    """`...-x137-y117-a0-h162.png` -- the panel's own readout, in the name."""
    m = re.search(r"-x(\d+)-y(\d+)-a(\d+)-h(-?\d+)", os.path.basename(path))
    if not m:
        sys.exit(f"{path}: no camera in the filename, and no --at given. "
                 f"Name it like island-x126-y123-a0-h126.png, from the panel's "
                 f"LON/LAT/HDG/ALT.")
    return [int(v) for v in m.groups()]


def find_view(shot, width, height):
    """Where the 3D view sits in a xemu screenshot, and at what scale.

    Palette 0 is black and so is the border, so the picture is the non-black
    bounding box -- its top row is sky and spans the whole width, which is what
    makes the left and right edges trustworthy. Two traps, both of which cost a
    round of wrong answers before this was written:

    - **the PAL visible area clips the last column or two.** The picture is 320
      wide and the screenshot has 318 or 319 of it, so the comparison has to be
      told how many columns it actually got rather than assuming 320.
    - **an even offset passes an alignment test just as well as the right
      one.** Checking that the 2x blocks are uniform says the crop is on the
      pixel grid, not that it is on the *picture*, so the bounding box has to
      supply the origin and the block test only confirms it.
    """
    ink = shot.sum(2) > 0
    rows, cols = np.nonzero(ink.any(1))[0], np.nonzero(ink.any(0))[0]
    if not len(rows) or not len(cols):
        sys.exit("the screenshot is entirely black")
    y0, x0, x1 = int(rows[0]), int(cols[0]), int(cols[-1])

    scale = max(1, int(round((x1 - x0 + 1) / width)))
    got = min(width, (x1 - x0 + 1) // scale)
    if got < width // 2:
        sys.exit(f"found only {got} columns of picture at {scale}x -- this does "
                 f"not look like a screenshot of the game")

    block = shot[y0:y0 + height * scale, x0:x0 + got * scale]
    if scale > 1 and not np.array_equal(block[0::scale, 0::scale],
                                        block[scale - 1::scale, scale - 1::scale]):
        sys.exit(f"the picture at ({x0},{y0}) {scale}x is not on the pixel grid; "
                 f"the screenshot may be scaled by something other than a whole "
                 f"number")
    return x0, y0, scale, got


def to_indices(view, palette):
    """Every screenshot pixel back to the palette entry that drew it."""
    flat = view.reshape(-1, 1, 3)
    d = ((flat - palette[None, :, :]) ** 2).sum(2)
    worst = np.sqrt(d.min(1)).max()
    if worst > MAX_COLOUR_ERROR:
        sys.exit(f"a colour in the screenshot is {worst:.1f} away from any "
                 f"palette entry: this is not a picture of this map")
    return d.argmin(1).reshape(view.shape[:2]), worst


def search(march, cam, ref, cols, coarse):
    """The sub-cell offset the panel cannot report, by coarse-to-fine search.

    The picture is most sensitive to it in the near field -- a quarter of a
    cell moves the bottom rows by a whole sample -- so the coarse pass gets
    close and the fine pass has to be every value, not every fourth.
    """
    cell_x, cell_y = cam.x >> 8, cam.y >> 8

    def score(fx, fy):
        cam.x, cam.y = (cell_x << 8) | fx, (cell_y << 8) | fy
        return int((march.render(cam)[:, :cols] != ref).sum())

    best = min(((score(fx, fy), fx, fy)
                for fy in range(0, 256, coarse) for fx in range(0, 256, coarse)))
    n, bx, by = best
    for fy in range(max(by - coarse, 0), min(by + coarse + 1, 256)):
        for fx in range(max(bx - coarse, 0), min(bx + coarse + 1, 256)):
            got = score(fx, fy)
            if got < n:
                n, bx, by = got, fx, fy
    cam.x, cam.y = (cell_x << 8) | bx, (cell_y << 8) | by
    return n, bx, by


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("screenshot", nargs="?", default=REFERENCE)
    ap.add_argument("--mission", default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "maps", "island.yaml"))
    ap.add_argument("--at", metavar="CELLX,CELLY,ANGLE,ALT",
                    help="the camera, if it is not in the screenshot's name")
    ap.add_argument("--hgt-size", type=int, default=P.DEFAULT_HGT_SIZE)
    ap.add_argument("--col-size", type=int, default=P.DEFAULT_COL_SIZE)
    ap.add_argument("--src", default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src"))
    ap.add_argument("--coarse", type=int, default=8,
                    help="sub-cell search step before the fine pass")
    ap.add_argument("--max-diff", type=int, default=0,
                    help="pixels that may differ and still pass. The two agreed "
                         "exactly when this was written, so the default is 0")
    ap.add_argument("--diff", metavar="FILE", help="write a map of what differs")
    args = ap.parse_args()

    c = P.Constants(args.src)
    spec, _ = P.read_mission(args.mission)
    hgt, col = P.map_files(args.mission, spec)
    maps = P.Maps(hgt, col, args.hgt_size, args.col_size)
    march = P.March(c, maps, P.Sine(c.sin_quarter))

    shot = np.asarray(Image.open(args.screenshot).convert("RGB")).astype(int)
    x0, y0, scale, cols = find_view(shot, c.FB_WIDTH, c.FB_HEIGHT)
    view = shot[y0:y0 + c.FB_HEIGHT * scale, x0:x0 + cols * scale][::scale, ::scale]
    ref, worst = to_indices(view, maps.rgb.astype(int))

    cell_x, cell_y, angle, height = (
        [int(v) for v in args.at.split(",")] if args.at
        else camera_from_name(args.screenshot))
    cam = P.Camera(cell_x << 8, cell_y << 8, angle, height, c.TILT_LEVEL)

    n, fx, fy = search(march, cam, ref, cols, args.coarse)
    total = ref.size

    clipped = "   (PAL clips the last column)" if cols < c.FB_WIDTH else ""
    print(f"view: {cols}x{c.FB_HEIGHT} of {c.FB_WIDTH} at ({x0},{y0}), "
          f"{scale}x{clipped}")
    print(f"palette: worst colour {worst:.1f} from an entry")
    print(f"camera: cell {cell_x},{cell_y} angle {angle} height {height}, "
          f"sub-cell {fx},{fy} by search")
    print(f"{n} of {total} palette indices differ"
          f"{f' ({100.0 * n / total:.3f}%)' if n else ''}"
          f" -- {'OK' if n <= args.max_diff else 'FAILED'}")

    if args.diff:
        d = march.render(cam)[:, :cols] != ref
        Image.fromarray((d * 255).astype(np.uint8)).save(args.diff)
        print(f"{args.diff}: {d.sum()} pixels marked")

    if n > args.max_diff:
        print("\nThe two disagree. In order of likelihood:\n"
              "  - tools/genmap.py changed, so the map under the camera is no "
              "longer the one in the screenshot. A handful of pixels, usually "
              "wherever the generator was touched.\n"
              "  - the renderer in src/ changed and tools/preview.py has not "
              "caught up, or the other way about. Usually a lot of pixels.\n"
              "  - either change was deliberate, and it is the reference "
              "screenshot that is stale.\n"
              "Only the last needs a new screenshot; see this file's header. "
              "--diff writes a map of what moved, which tells the three apart "
              "faster than reasoning does.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
