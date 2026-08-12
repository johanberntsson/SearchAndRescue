#!/usr/bin/env python3
"""Convert the VoxelSpace PNG maps into MEGA65 resources.

    convmap.py <height.png> <colour.png> <sprite.png>[,<sprite.png>...]
               <out-prefix> [hgt-size] [col-size] [--shared maps/palette.yaml]

writes <out-prefix>.hgt, <out-prefix>.col, <out-prefix>.pal,
<out-prefix>.ovr (the panel's overview map) and one billboard file per sprite
sheet: <out-prefix>.spr for the first and <out-prefix>.sp2, .sp3 ... for the
rest, which is the order src/sprite.c numbers its figures in.  The two
sizes default to 512 and 1024 and must be powers of two from 256 up to the
source resolution; they have to match HGT_SIZE and COL_SIZE in the build (the
Makefile passes both).

The sources are already in the right shape: the heightmap is 8-bit greyscale
so its pixel values are heights, and the colourmap is a palette image whose
indices can be used directly as VIC-IV colour indices.  Only the resolution
and the palette encoding need changing.

The sprite sheets are converted here rather than by a tool of their own
because there is only one palette on screen: their colours have to go in slots
the colourmap has left free, which is not knowable without the colourmap --
and every sheet has to draw from that one pool, so they are quantised together
rather than one at a time.

`--shared` is for the generated maps, and it is what lets **more than one map
share a disk**.  Without it the free pool is whatever a particular colourmap
happens not to use, so two maps hand the sprites different palette entries and
a figure changes colour with the mission.  With it, every index the shared
ramp in maps/palette.yaml claims is treated as used whether this map reached it
or not -- and so are 1..15, which the design keeps for the system -- so the
pool starts at the same place for every map and one set of sprite files serves
all of them.  It costs nothing: those entries are spoken for anyway.
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile
from collections import deque

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


# The overview map in the panel: the colourmap scaled right down and laid out
# as sixteen 8x8 full-colour characters, so the loader reads it straight into
# character memory and nothing has to rearrange it on the MEGA65. 32x32 covers
# the whole 256-cell world at one pixel per eight cells.
OVERVIEW_PX = 32
OVERVIEW_CHARS = OVERVIEW_PX // 8


def to_overview(indices):
    """32x32 of the colourmap as OVERVIEW_CHARS^2 tiles of 8x8 bytes."""
    n = indices.shape[0] // OVERVIEW_PX
    # Point sampled, like the colourmap itself: averaging palette indices is
    # meaningless. The middle of each block rather than its corner, which
    # represents the block better at this reduction.
    small = indices[n // 2 :: n, n // 2 :: n][:OVERVIEW_PX, :OVERVIEW_PX]
    tiles = small.reshape(OVERVIEW_CHARS, 8, OVERVIEW_CHARS, 8)
    # (tile row, tile col, pixel row, pixel col): tiles in reading order, each
    # one eight rows of eight bytes, which is what a full-colour character is.
    return tiles.transpose(0, 2, 1, 3).astype(np.uint8).tobytes()


# The billboards, out of the sprite sheets in resources/: a title strip over a
# grid of poses, drawn on a checkerboard rather than with real alpha, so the
# figure has to be cut out. The renderer scales one stored pose to whatever the
# distance calls for; the eight directions are for later, when the figure faces
# the drone.
SPRITE_GRID = (2, 4)   # poses down and across, below the title strip
SPRITE_POSE = 0        # reading order within the grid: 0 is the front view
SPRITE_MAX = 32        # the box a stored figure is scaled to fit, longest side
                       # first, so a scene of two people is as welcome as one
                       # standing figure; both dimensions go in the header and
                       # src/sprite.c sizes its buffer from this
SPRITE_COLOURS = 15    # palette entries each may claim. Sprite pixel value 0
                       # is transparent, so it is not one of them.


def sheet_cell(a, pose):
    """One pose's cell of the sheet, inside its grid lines."""
    # The title strip is separated by a solid dark rule; the poses below it are
    # on an even grid. The last few per cent of each cell are dropped so that a
    # grid line can never be mistaken for part of the figure.
    rule = [i for i, v in enumerate((a.max(2) < 110).mean(1)) if v > 0.5]
    top = rule[0] + 1 if rule else 0
    rows, cols = SPRITE_GRID
    ch, cw = (a.shape[0] - top) // rows, a.shape[1] // cols
    r, c = divmod(pose, cols)
    my, mx = ch // 25, cw // 25
    return a[top + r * ch + my:top + (r + 1) * ch - my, c * cw + mx:(c + 1) * cw - mx]


def cut_figure(cell):
    """Crop to the figure and return it with a mask of which pixels are it.

    The checkerboard behind the figure is light and grey and the pose label is
    black, so the figure is found as the saturated pixels: that excludes both.
    Its outline is black and reaches a pixel or two past them, hence the pad,
    and its dark and grey insides -- hair, backpack, boots -- are recovered by
    filling everything the background cannot reach from the edge.
    """
    ys, xs = np.nonzero((cell.max(2) - cell.min(2)) > 60)
    pad = 4
    sub = cell[max(ys.min() - pad, 0):ys.max() + pad + 1,
               max(xs.min() - pad, 0):xs.max() + pad + 1]

    mx, mn = sub.max(2), sub.min(2)
    solid = ((mx - mn) > 40) | (mx < 120)

    h, w = solid.shape
    outside = np.zeros((h, w), bool)
    queue = deque((y, x) for y in range(h) for x in (0, w - 1) if not solid[y, x])
    queue.extend((y, x) for x in range(w) for y in (0, h - 1) if not solid[y, x])
    for y, x in queue:
        outside[y, x] = True
    while queue:
        y, x = queue.popleft()
        for ny, nx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
            if 0 <= ny < h and 0 <= nx < w and not solid[ny, nx] and not outside[ny, nx]:
                outside[ny, nx] = True
                queue.append((ny, nx))

    mask = ~outside
    ys, xs = np.nonzero(mask)
    box = (slice(ys.min(), ys.max() + 1), slice(xs.min(), xs.max() + 1))
    return sub[box], mask[box]


def shrink_figure(crop, mask, box):
    """Box-average the figure into a `box` square, keeping its proportions.

    The longest side is what gets scaled to `box`: a lone standing figure is
    taller than it is wide and comes out full height, while a scene of two
    people is wider than it is tall and would otherwise overflow the buffer
    src/sprite.c sizes from SPRITE_MAX.

    Averaging rather than point sampling because the sheet is a lossy render:
    every flat area of the pixel art is a cloud of near-identical colours, and
    one sample of it is one sample of the noise. A destination pixel belongs to
    the figure if at least half of the source block did.
    """
    sh, sw, _ = crop.shape
    height = box if sh >= sw else max(1, round(sh * box / sw))
    # Clamped as well as derived: the two roundings are independent and a wide
    # figure can come back out of the second one a pixel over the box.
    width = min(box, max(1, round(sw * height / sh)))
    rgb = np.zeros((height, width, 3))
    opaque = np.zeros((height, width), bool)

    for j in range(height):
        y0, y1 = j * sh // height, max(j * sh // height + 1, (j + 1) * sh // height)
        for i in range(width):
            x0, x1 = i * sw // width, max(i * sw // width + 1, (i + 1) * sw // width)
            block = mask[y0:y1, x0:x1]
            if block.mean() >= 0.5:
                opaque[j, i] = True
                rgb[j, i] = crop[y0:y1, x0:x1][block].mean(0)
    return rgb, opaque


def to_sprite(path, rgb_palette, pool):
    """Convert one sprite sheet; returns the file's bytes.

    Its colours are median cut down to SPRITE_COLOURS and given palette slots
    the colourmap left free -- there are around fifty of those below the sky,
    which is what limits how many figures can be on the one palette. `pool` is
    that free list, shared by every sheet and consumed from the front, so no
    two figures claim the same entry.

    The file is a four byte header (width, height, and two spare) followed by
    the pixels *column by column*, which is the order the renderer draws them
    in: the framebuffer is laid out in column strips, so a vertical run of
    pixels is one pointer stepping by eight.
    """
    sheet = np.asarray(Image.open(path).convert("RGB")).astype(int)
    crop, mask = cut_figure(sheet_cell(sheet, SPRITE_POSE))
    rgb, opaque = shrink_figure(crop, mask, SPRITE_MAX)
    height, width = opaque.shape

    # Median cut over the figure's pixels alone: quantising the whole grid
    # would spend entries on the empty space around it.
    flat = rgb[opaque].astype(np.uint8).reshape(1, -1, 3)
    q = Image.fromarray(flat).quantize(colors=SPRITE_COLOURS, method=Image.MEDIANCUT)
    quantised = q.getpalette()[:SPRITE_COLOURS * 3]

    if len(pool) < SPRITE_COLOURS:
        sys.exit(f"only {len(pool)} free palette entries left for {path}, "
                 f"which needs {SPRITE_COLOURS}")
    mine = [pool.pop(0) for _ in range(SPRITE_COLOURS)]
    for n, entry in enumerate(mine):
        rgb_palette[entry] = tuple(quantised[n * 3:n * 3 + 3])

    pixels = np.zeros((height, width), np.uint8)  # 0 is transparent
    pixels[opaque] = [mine[i] for i in np.asarray(q).reshape(-1)]

    return (bytes((width, height, 0, 0)) + pixels.T.tobytes(),
            width, height, mine)


def sprite_suffix(n):
    """Figure 0 keeps the original .spr; the rest are numbered from two."""
    return ".spr" if n == 0 else f".sp{n + 1}"


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


def shared_indices(path):
    """Every palette index the shared ramp claims, from maps/palette.yaml.

    Read here rather than imported from genmap.py so that convmap.py stays
    what it has always been: a converter that needs nothing but its inputs.
    The file's shape is the generator's, though, so keep the two in step --
    a band is `base`, `count` steps, and one entry per shade unless it says
    `shaded: false`.
    """
    import yaml
    with open(path) as f:
        pal = yaml.safe_load(f)
    shades = len(pal["shades"])
    out = set()
    for band in pal["bands"].values():
        span = band["count"] * (1 if band.get("shaded") is False else shades)
        out |= set(range(band["base"], band["base"] + span))
    return out


def main():
    argv = list(sys.argv[1:])
    shared = None
    if "--shared" in argv:
        i = argv.index("--shared")
        shared = argv[i + 1]
        del argv[i:i + 2]
    if not 4 <= len(argv) <= 6:
        sys.exit(__doc__)
    height_png, colour_png, sprite_pngs, prefix = argv[:4]
    sprite_pngs = sprite_pngs.split(",")
    hgt_size = int(argv[4]) if len(argv) > 4 else DEFAULT_HGT_SIZE
    col_size = int(argv[5]) if len(argv) > 5 else DEFAULT_COL_SIZE

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
    if shared:
        # Everything the shared ramp claims, plus the system's low sixteen.
        # See --shared in the header: this is what makes one set of sprite
        # files serve every map on the disk.
        reserved |= shared_indices(shared) | set(range(1, 16))
    for entry, colour in PANEL_COLOURS:
        used = reserve(indices, rgb, entry, used, reserved)
        rgb[entry] = colour

    # After the reservations, so the sprites only claim entries that are still
    # genuinely spare -- and before the palette is packed, since they write
    # into it. One pool across all of them: there is a single palette on
    # screen, and every figure in the game has to fit in it at once.
    pool = [i for i in range(1, SKY_BASE) if i not in used and i not in reserved]
    sprites = [to_sprite(png, rgb, pool) for png in sprite_pngs]

    # Three 256-byte planes matching the $D100/$D200/$D300 register banks, so
    # uploading is three straight copies.
    palette = bytes(nybswap(c[channel]) for channel in range(3) for c in rgb)

    # The first figure is .spr and the rest are .sp2, .sp3 ...; src/sprite.c
    # names them in the same order and numbers them from zero.
    sprite_files = [(sprite_suffix(n), s[0]) for n, s in enumerate(sprites)]

    exomizer = find_exomizer()
    raw_sizes = {}
    for suffix, payload in [
        (".hgt", to_planes(heights)),
        (".col", to_planes(indices)),
        (".pal", palette),
        (".ovr", to_overview(indices)),
    ] + sprite_files:
        raw_sizes[suffix] = len(payload)
        # The palette is 768 bytes and is read straight into a buffer before
        # the display is up; crunching it would buy nothing and cost a special
        # case in the loader.
        if suffix in (".hgt", ".col"):
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
    print(f"  overview: {OVERVIEW_PX}x{OVERVIEW_PX} in "
          f"{OVERVIEW_CHARS ** 2} characters, {raw_sizes['.ovr']} bytes")
    for n, (_, w, h, entries) in enumerate(sprites):
        print(f"  sprite {n} ({os.path.basename(sprite_pngs[n])}): {w}x{h}, "
              f"{raw_sizes[sprite_suffix(n)]} bytes, palette "
              f"{min(entries)}..{max(entries)}")
    print(f"  {len(pool)} palette entries still free"
          + ("  (pool shared across maps)" if shared else ""))


if __name__ == "__main__":
    main()
