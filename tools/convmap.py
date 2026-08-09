#!/usr/bin/env python3
"""Convert the VoxelSpace PNG maps into raw MEGA65 resources.

    convmap.py <height.png> <colour.png> <out-prefix>

writes <out-prefix>.hgt, <out-prefix>.col and <out-prefix>.pal.

The sources are already in the right shape: the heightmap is 8-bit greyscale
so its pixel values are heights, and the colourmap is a palette image whose
indices can be used directly as VIC-IV colour indices.  Only the resolution
and the palette encoding need changing.
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

MAP_SIZE = 256  # engine map is MAP_SIZE x MAP_SIZE, so coords wrap in a byte

# The colourmap is kept at twice the heightmap's resolution, because the
# blockiness the eye notices is the colour, not the silhouette -- and the
# source PNGs have the detail to spare. It ships as four MAP_SIZE x MAP_SIZE
# planes, one per half-cell corner, rather than one 512x512 array: that keeps
# each plane on a 64K boundary, so the renderer still addresses a cell by
# dropping the two high coordinate bytes into a pointer, and picks the plane
# with the bank byte. A real 512x512 array would need a shift and an OR on
# every read.
COL_SUB = 2

# Reading a SEQ file through the Kernal comes up exactly 256 bytes short of the
# end, whatever the file's size (measured across 16K-64K on xemu's ROM), so the
# tail of every resource is unreachable.  Pad past it; src/loader.c reads a
# known length and never looks for EOF.
TAIL_PAD = b"\0" * 512

# The maps are exomizer-crunched, which is what lets resolutions above 256 fit
# a d81 at all.  Flags match the decruncher in src/exo_asm.s, itself a port of
# the one in mega65/ozmoo-z6: raw stream, no load address, crunched forwards,
# exomizer-2 (-P0) layout.  -m is the furthest back a match may reach; the
# decruncher reads back-references straight out of the plaintext it has
# already written, so there is no window buffer to size and this can be as
# large as the cruncher will take.
EXO_FLAGS = ["raw", "-q", "-C", "-P0", "-c", "-m", "65535"]

# Each crunched map carries its own length, so the loader knows how much to
# read without the build having to tell it -- and without relying on EOF,
# which the Kernal reports early.
LENGTH_PREFIX = "<I"


def find_exomizer():
    """$EXOMIZER, then a local copy, then the ozmoo-z6 checkout, then PATH."""
    for c in (os.environ.get("EXOMIZER"),
              "tools/exomizer",
              os.path.expanduser("~/commodore/ozmoo-z6/exomizer/src/exomizer"),
              shutil.which("exomizer")):
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    sys.exit(
        "exomizer not found. Set $EXOMIZER, put a copy or symlink at "
        "tools/exomizer, or check out mega65/ozmoo-z6 beside this project."
    )


def crunch(exomizer, data):
    with tempfile.TemporaryDirectory() as d:
        raw, out = os.path.join(d, "raw"), os.path.join(d, "exo")
        with open(raw, "wb") as f:
            f.write(data)
        r = subprocess.run([exomizer] + EXO_FLAGS + [raw, "-o", out],
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        if r.returncode:
            sys.exit(f"exomizer failed: {r.stderr.decode()[:400]}")
        with open(out, "rb") as f:
            crunched = f.read()
    return struct.pack(LENGTH_PREFIX, len(crunched)) + crunched

# Sky gradient occupies a contiguous run of indices the colourmap never uses,
# top of screen first.  Keep in sync with SKY_BASE/SKY_SHADES in src/voxel.h.
SKY_BASE = 224
SKY_SHADES = 16
SKY_TOP = (40, 70, 140)
SKY_HORIZON = (170, 200, 230)

# Reserved for a pixel-drawn overlay over the 3D view (an artificial horizon
# and the rest of what documentation/vision.md wants). Full-colour characters
# take a whole byte per pixel, so these can sit anywhere.
HUD_INK = 240
HUD_PAPER = 241

# The information panel is ordinary text characters, and a text character
# takes its colour from colour RAM -- which in 16-bit character mode is only
# a four-bit field. So panel ink has to be one of the first sixteen palette
# entries, which is exactly where the terrain colours live. Index 0 is
# already reserved as the screen and border colour and serves as the paper;
# these two are freed by moving whatever terrain colour sits there out to a
# slot the colourmap does not use. Keep in sync with src/panel.h.
PANEL_INK = 1
PANEL_LABEL = 2
PANEL_COLOURS = ((PANEL_INK, (255, 255, 255)), (PANEL_LABEL, (150, 160, 170)))


def nybswap(v):
    """VIC-IV palette registers hold each channel with its nybbles reversed."""
    return ((v & 0x0F) << 4) | (v >> 4)


def load_heightmap(path):
    im = Image.open(path)
    if im.mode != "L":
        im = im.convert("L")
    a = np.asarray(im)
    if a.shape[0] % MAP_SIZE or a.shape[1] % MAP_SIZE:
        sys.exit(f"{path}: {a.shape} is not a whole multiple of {MAP_SIZE}")
    # Box-average each block: heights want to be smooth, and point sampling a
    # 4x downscale turns gentle slopes into staircases.
    n = a.shape[0] // MAP_SIZE
    a = a.reshape(MAP_SIZE, n, MAP_SIZE, n).mean(axis=(1, 3))
    return a.round().astype(np.uint8)


def load_colourmap(path):
    im = Image.open(path)
    if im.mode != "P":
        sys.exit(f"{path}: expected a palette image, got mode {im.mode}")
    a = np.asarray(im)
    if a.shape[0] % MAP_SIZE or a.shape[1] % MAP_SIZE:
        sys.exit(f"{path}: {a.shape} is not a whole multiple of {MAP_SIZE}")
    # Point sample: averaging palette *indices* is meaningless. Copied
    # because a view of the decoded image is read-only, and reserve() below
    # rewrites indices in place.
    n = a.shape[0] // (MAP_SIZE * COL_SUB)
    if n < 1:
        sys.exit(f"{path}: {a.shape} is too small for {MAP_SIZE * COL_SUB} colour cells")
    indices = a[::n, ::n].copy()

    pal = im.getpalette()
    rgb = [tuple(pal[i * 3 : i * 3 + 3]) for i in range(256)]
    return indices, rgb


def add_sky(rgb, used):
    end = SKY_BASE + SKY_SHADES
    clash = sorted(used & set(range(SKY_BASE, end)))
    if clash:
        sys.exit(f"colourmap uses sky palette indices {clash}")
    for i in range(SKY_SHADES):
        t = i / (SKY_SHADES - 1)
        rgb[SKY_BASE + i] = tuple(
            round(a + (b - a) * t) for a, b in zip(SKY_TOP, SKY_HORIZON)
        )


def reserve(indices, rgb, entry, used, reserved):
    """Move any terrain colour off `entry` so the panel can have it.

    The colourmap uses about 170 of the 224 indices below the sky, so there is
    always somewhere to put the displaced colour; only the pixels that used
    `entry` change, and they keep exactly the colour they had.

    `reserved` has to be excluded explicitly: an index this function has just
    emptied is no longer in `used`, so without it the next call would pick the
    slot the previous one just claimed and paint terrain in the ink colour.
    """
    if entry not in used:
        return used

    spare = next(
        (i for i in range(1, SKY_BASE) if i not in used and i not in reserved), None
    )
    if spare is None:
        sys.exit(f"no free palette index to move terrain colour {entry} to")
    indices[indices == entry] = spare
    rgb[spare] = rgb[entry]
    return (used - {entry}) | {spare}


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    height_png, colour_png, prefix = sys.argv[1:]

    heights = load_heightmap(height_png)
    indices, rgb = load_colourmap(colour_png)
    add_sky(rgb, set(np.unique(indices).tolist()))

    # Index 0 is the border and screen colour, and full-colour mode draws it
    # wherever a pixel is 0. The source palette has an unused bright green
    # there, which makes a startling border.
    rgb[0] = (0, 0, 0)

    used = set(np.unique(indices).tolist())
    for entry, colour in ((HUD_INK, (255, 255, 255)), (HUD_PAPER, (0, 0, 0))):
        if entry in used:
            sys.exit(f"colourmap uses HUD palette index {entry}")
        rgb[entry] = colour

    reserved = ({0, HUD_INK, HUD_PAPER}
                | set(range(SKY_BASE, SKY_BASE + SKY_SHADES))
                | {entry for entry, _ in PANEL_COLOURS})
    for entry, colour in PANEL_COLOURS:
        used = reserve(indices, rgb, entry, used, reserved)
        rgb[entry] = colour

    # Three 256-byte planes matching the $D100/$D200/$D300 register banks, so
    # uploading is three straight copies.
    palette = bytes(nybswap(c[channel]) for channel in range(3) for c in rgb)

    # Plane (suby << 1) | subx, matching how the renderer builds the bank
    # byte. Concatenated, so the loader can pull the whole thing up to attic
    # RAM as one run of four 64K-aligned planes.
    planes = b"".join(
        indices[sy::COL_SUB, sx::COL_SUB].tobytes()
        for sy in range(COL_SUB)
        for sx in range(COL_SUB)
    )

    exomizer = find_exomizer()
    raw_sizes = {}
    for suffix, payload in (
        (".hgt", heights.tobytes()),
        (".col", planes),
        (".pal", palette),
    ):
        raw_sizes[suffix] = len(payload)
        # The palette is 768 bytes and is read straight into a buffer before
        # the display is up; crunching it would buy nothing and cost a special
        # case in the loader.
        if suffix != ".pal":
            payload = crunch(exomizer, payload)
        with open(prefix + suffix, "wb") as f:
            f.write(payload)
            f.write(TAIL_PAD)

    def report(suffix, what):
        raw = raw_sizes[suffix]
        packed = os.path.getsize(prefix + suffix) - len(TAIL_PAD)
        return (f"{what}: {raw} -> {packed} bytes "
                f"({raw / packed:.2f}x)" if raw != packed else f"{what}: {raw} bytes")

    print(f"heightmap {MAP_SIZE}x{MAP_SIZE}, "
          f"colourmap {MAP_SIZE * COL_SUB}x{MAP_SIZE * COL_SUB} "
          f"in {COL_SUB * COL_SUB} planes, "
          f"sky at {SKY_BASE}..{SKY_BASE + SKY_SHADES - 1}, "
          f"panel ink at {PANEL_INK}/{PANEL_LABEL}")
    print("  " + report(".hgt", "hgt"))
    print("  " + report(".col", "col"))


if __name__ == "__main__":
    main()
