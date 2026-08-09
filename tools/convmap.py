#!/usr/bin/env python3
"""Convert the VoxelSpace PNG maps into MEGA65 resources.

    convmap.py <height.png> <colour.png> <out-prefix> [hgt-size] [col-size]

writes <out-prefix>.hgt, <out-prefix>.col and <out-prefix>.pal.  The two
sizes default to 512 and 1024 and must be powers of two from 256 up to the
source resolution; they have to match HGT_SIZE and COL_SIZE in the build (the
Makefile passes both).

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

# A plane is always CELLS x CELLS, so a whole number of 64K banks.
CELLS = 256

# Both maps ship as (size / CELLS)^2 planes of CELLS x CELLS, one per sub-cell
# corner, rather than as one big array. That keeps every plane on a 64K
# boundary, so the renderer addresses a cell by dropping the two high
# coordinate bytes into a pointer and picks the plane with the bank byte --
# the addressing that makes a sample cheap. One big array would need a shift
# and an OR on every read.
#
# The colourmap is worth more resolution than the heightmap: the blockiness
# the eye notices is the colour, not the silhouette. The heightmap above
# CELLS also has to leave chip RAM, which costs the inner loop an attic read
# and a plane lookup on every sample rather than once per span.
DEFAULT_HGT_SIZE = 512
DEFAULT_COL_SIZE = 1024

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


def load_heightmap(path, size):
    im = Image.open(path)
    if im.mode != "L":
        im = im.convert("L")
    a = np.asarray(im)
    check_size(path, a.shape, size)
    # Box-average each block: heights want to be smooth, and point sampling a
    # downscale turns gentle slopes into staircases.
    n = a.shape[0] // size
    a = a.reshape(size, n, size, n).mean(axis=(1, 3))
    return a.round().astype(np.uint8)


def load_colourmap(path, size):
    im = Image.open(path)
    if im.mode != "P":
        sys.exit(f"{path}: expected a palette image, got mode {im.mode}")
    a = np.asarray(im)
    check_size(path, a.shape, size)
    # Point sample: averaging palette *indices* is meaningless. Copied
    # because a view of the decoded image is read-only, and reserve() below
    # rewrites indices in place.
    n = a.shape[0] // size
    indices = a[::n, ::n].copy()

    pal = im.getpalette()
    rgb = [tuple(pal[i * 3 : i * 3 + 3]) for i in range(256)]
    return indices, rgb


def check_size(path, shape, size):
    if size < CELLS or size & (size - 1):
        sys.exit(f"{size} is not a power of two of at least {CELLS}")
    if shape[0] != shape[1] or shape[0] % size:
        sys.exit(f"{path}: {shape} cannot be sampled down to {size}x{size}")


def to_planes(a):
    """(size/CELLS)^2 planes of CELLS x CELLS, plane (suby * k + subx)."""
    k = a.shape[0] // CELLS
    return b"".join(a[sy::k, sx::k].tobytes() for sy in range(k) for sx in range(k))


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
    if not 4 <= len(sys.argv) <= 6:
        sys.exit(__doc__)
    height_png, colour_png, prefix = sys.argv[1:4]
    hgt_size = int(sys.argv[4]) if len(sys.argv) > 4 else DEFAULT_HGT_SIZE
    col_size = int(sys.argv[5]) if len(sys.argv) > 5 else DEFAULT_COL_SIZE

    heights = load_heightmap(height_png, hgt_size)
    indices, rgb = load_colourmap(colour_png, col_size)
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

    exomizer = find_exomizer()
    raw_sizes = {}
    for suffix, payload in (
        (".hgt", to_planes(heights)),
        (".col", to_planes(indices)),
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

    print(f"heightmap {hgt_size}x{hgt_size} in {(hgt_size // CELLS) ** 2} planes, "
          f"colourmap {col_size}x{col_size} in {(col_size // CELLS) ** 2} planes, "
          f"sky at {SKY_BASE}..{SKY_BASE + SKY_SHADES - 1}, "
          f"panel ink at {PANEL_INK}/{PANEL_LABEL}")
    print("  " + report(".hgt", "hgt"))
    print("  " + report(".col", "col"))


if __name__ == "__main__":
    main()
