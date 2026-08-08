#!/usr/bin/env python3
"""Convert the VoxelSpace PNG maps into raw MEGA65 resources.

    convmap.py <height.png> <colour.png> <out-prefix>

writes <out-prefix>.hgt, <out-prefix>.col and <out-prefix>.pal.

The sources are already in the right shape: the heightmap is 8-bit greyscale
so its pixel values are heights, and the colourmap is a palette image whose
indices can be used directly as VIC-IV colour indices.  Only the resolution
and the palette encoding need changing.
"""

import sys

import numpy as np
from PIL import Image

MAP_SIZE = 256  # engine map is MAP_SIZE x MAP_SIZE, so coords wrap in a byte

# Reading a SEQ file through the Kernal comes up exactly 256 bytes short of the
# end, whatever the file's size (measured across 16K-64K on xemu's ROM), so the
# tail of every resource is unreachable.  Pad past it; src/loader.c reads a
# known length and never looks for EOF.
TAIL_PAD = b"\0" * 512

# Sky gradient occupies a contiguous run of indices the colourmap never uses,
# top of screen first.  Keep in sync with SKY_BASE/SKY_SHADES in src/voxel.h.
SKY_BASE = 224
SKY_SHADES = 16
SKY_TOP = (40, 70, 140)
SKY_HORIZON = (170, 200, 230)


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
    # Point sample: averaging palette *indices* is meaningless.
    n = a.shape[0] // MAP_SIZE
    indices = a[::n, ::n]

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

    # Three 256-byte planes matching the $D100/$D200/$D300 register banks, so
    # uploading is three straight copies.
    palette = bytes(nybswap(c[channel]) for channel in range(3) for c in rgb)

    for suffix, payload in (
        (".hgt", heights.tobytes()),
        (".col", indices.tobytes()),
        (".pal", palette),
    ):
        with open(prefix + suffix, "wb") as f:
            f.write(payload)
            f.write(TAIL_PAD)

    print(
        f"{prefix}.hgt/.col {MAP_SIZE}x{MAP_SIZE}, "
        f"{prefix}.pal {len(palette)} bytes, "
        f"sky at {SKY_BASE}..{SKY_BASE + SKY_SHADES - 1}"
    )


if __name__ == "__main__":
    main()
