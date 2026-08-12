#!/usr/bin/env python3
"""Generate a height and colour map pair from a map file.

    genmap.py <map.yaml> [-o outdir] [--size N] [--palette FILE]

writes hmapNN.png (8-bit greyscale heights) and cmapNN.png (a palette image of
colour indices) beside the YAML, where NN is the `general.id` field.  Those are
exactly the two shapes tools/convmap.py already consumes, so nothing downstream
of here changes: convmap.py samples them down, crunches them and the build puts
them on the disk the same way it does the hand-drawn pair in resources/.

See documentation/procedural-maps.md for the design and the YAML schema.

**A map file describes a world and nothing else** -- its seed and shape, and
the things built into its terrain.  Which mission is flown over it, and where
anybody stands, is not in here: a mission *has* a map, and the two are not one
to one, since two rescues could be flown over the same island.  The game's
mission table is in src/mission.c and names the map slot it wants.  Items that
are terrain -- a pyramid -- are built into the two maps here; a survivor is a
mission object and will come from the mission side when that exists.

Everything is drawn from one seeded xorshift stream, so the same YAML gives
byte identical output on any machine, and the seed in the file is the whole of
the state needed to get a map back.

**The arithmetic is the MEGA65's.**  Not "portable in principle": every value
in this file is a Q0.16 integer, every multiply is one the 45GS02's $D770 does
in sixteen cycles, every divide is a reciprocal, and sqrt, tanh and the ramp's
gamma are 257-entry tables -- all of it in tools/fixed.py, whose self-test
prices each routine against the float it replaced.  numpy is here to do a
megapixel at a time and for no other reason: read `>>` as a shift and
`np.where` as a branch and this is the C.

Two things follow that are easy to lose.  A histogram stands in for every
percentile, median and sort, because the machine has no room to sort a
megapixel -- and the bucket width lands directly on the map's heights, so it
interpolates inside the bucket (see percentile()).  And the whole file must
stay free of numpy semantics that have no scalar meaning: fancy indexing over
a whole array is fine, a library that solves the problem for you is not.

documentation/on-device-maps.md costs the rest of the journey: what the
generator would take to run on the machine itself, what it has to beat, and
the six traps this rewrite turned up.
"""

import argparse
import heapq
import os
import sys

import numpy as np
import yaml
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fixed as F                                         # noqa: E402

ONE = F.ONE

# The stretch's histogram: 1024 buckets of the field, see percentile().
BUCKETSHIFT = 6
BUCKETS = ONE >> BUCKETSHIFT

# The ramp's gamma, over 0..2 rather than 0..1: `t` is a height over the map's
# own 99th percentile and the top one per cent of the terrain is above 1.0 by
# construction. See fixed.gamma_table.
GAMMA_TOP = 2

# The source PNGs are 1024x1024 and convmap.py samples them down to HGT_SIZE
# and COL_SIZE, so generating at anything less would cap what the build can
# ask for. 1024 px over a 256-cell world is four pixels to a map cell.
DEFAULT_SIZE = 1024
CELLS = 256

# Heights are quarter-cell units (SCALE_H in src/voxel.c folds that in) and the
# hand-drawn heightmap peaks at 118 of the available 255. Matching its range
# keeps generated terrain flyable with the same camera altitudes and the same
# GROUND_GAP as the map the flight model was tuned against.
HEIGHT_MAX = 120

# The coarsest noise lattice at `scale: medium`: LATTICE cells across the whole
# map, so a feature at this octave spans a quarter of the world. Powers of two
# only -- every octave doubles it and the lattice has to divide the map exactly
# for the corners to land on pixel boundaries.
LATTICE = 4

# `scale` is how big the country is, not how much of it there is: the world is
# always 256 cells across, and this decides how many features that holds. It is
# the knob the hand-drawn map needs and does not have -- resources/C1W.png is
# plainly a "near" map, one huge pyramid and a couple of lakes, and nothing in
# the YAML could ask for that before.
#
#   lattice  the coarsest octave, so half the world at near and an eighth at
#            distant.
#   octaves  added to whatever `ruggedness` asked for, to hold the *finest*
#            octave where it was: base << (octaves - 1) is the same number in
#            all three rows. Without it, scale and ruggedness would fight --
#            zooming in would smooth the surface texture out as well as
#            enlarging the landforms, which is not what either word means.
#   feature  a length multiplier for everything measured in pixels: hill and
#            river radii, the flow blur, lake areas (as its square), and the
#            steepness both the rock colouring and the sun are judged against.
#            That last one matters: doubling a landform's width halves its
#            slopes, so without it a "near" map would come out uniformly green
#            and unshaded however dramatic its shape.
SCALE = {
    "near":    {"lattice": 2, "octaves": +1, "feature": 2.0},
    "medium":  {"lattice": 4, "octaves":  0, "feature": 1.0},
    "distant": {"lattice": 8, "octaves": -1, "feature": 0.5},
}

# ruggedness -> how many octaves are layered on the macro shape, and how fast
# their amplitude falls away. A modifier on top of `type`, never a replacement:
# a rugged island is still island-shaped, just noisier.
RUGGEDNESS = {
    "smooth":  (3, 0.35),
    "rolling": (4, 0.45),
    "rugged":  (5, 0.55),
    "jagged":  (6, 0.65),
}

# none/few/many, for all three feature types.
COUNTS = {"none": 0, "few": 3, "many": 8}
HILL_COUNTS = {"none": 0, "few": 6, "many": 16}

# The -size fields. Radii and areas are in pixels at DEFAULT_SIZE and scale
# with the map, so --size changes the resolution of a map and not its shape.
# A hill's height is a fraction of its terrain's own range rather than an
# absolute: the same number has to mean "a hill" on a plain whose whole relief
# is a few height units and on a mountain range that fills the map.
HILL_SIZE = {"small": (24, 0.25), "large": (56, 0.50)}   # radius, height
HILL_ROUGH = 0.4         # how far a dome's own noise may vary its height
LAKE_AREA = {"small": 900, "large": 6000}                # pixels of surface
RIVER_SIZE = {"small": 3, "large": 7}                    # radius

# The macro elevation function, per type: the noise's floor and range, and the
# sea level, all as a fraction of HEIGHT_MAX. Islands sit low with a lot of
# range because the radial mask pulls their edges under water; flatlands have
# barely any range at all and a sea level just below their floor, so water
# happens only where a lake or a river puts it.
#
# `ceiling` is how far up the land ramp that type is allowed to reach. The ramp
# is normalised to each map's own relief -- it has to be, or a flatland would
# come out one flat colour -- and without a ceiling that would give every map a
# bare rock summit, including one whose entire relief is six map cells. So the
# type says how high its high ground counts as: mountains earn the peak bands
# and a plain tops out in the middle of the lowland greens.
TYPES = {
    "island":    {"floor": 0.20, "range": 0.80, "sea": 0.26, "ridged": False,
                  "ceiling": 0.92},
    "mountains": {"floor": 0.15, "range": 0.85, "sea": 0.10, "ridged": True,
                  "ceiling": 1.00},
    "flatlands": {"floor": 0.20, "range": 0.22, "sea": 0.14, "ridged": False,
                  "ceiling": 0.50},
}

# The island mask: full height inside EDGE - FADE of the centre, nothing past
# EDGE, as a fraction of half the map. The coastline is the mask's edge pushed
# about by low-frequency noise, which is what stops it being a circle.
ISLAND_EDGE = 0.86
ISLAND_FADE = 0.46
ISLAND_WOBBLE = 0.13

# Colour. Slope pushes a pixel up the land ramp, so a cliff wears the rock
# colours of ground well above it and a beach only forms where the shore is
# actually flat; the mottle is a dither that breaks the ramp's contour lines up.
# The push is a ramp fraction and the mottle is in *steps*, because the ramp
# lost half its steps to the shading and a dither that means anything has to be
# about a step and a half wherever the ramp is re-cut. The mottle's lattice is
# deliberately coarse -- a cell every four map cells -- because at one pixel it
# is not texture but salt and pepper along every band boundary.
#
# SLOPE_REF is height per *map cell* and not per pixel, so it means the same
# thing at any --size. Measured over the three example maps, the steepest one
# per cent of an island is around 0.027 and of a mountain range around 0.056:
# 0.05 is therefore a push that mountains collect and gentler country does not.
SLOPE_PUSH = 0.22
SLOPE_REF = 0.07
MOTTLE = 1.4
DEPTH_REF = 0.10         # how deep water has to be to reach the darkest shade

# The sun. It is what makes a heightfield read as terrain from the air rather
# than as a coloured contour map: the hand-drawn resources/C1W.png has it baked
# in, and its luminance correlates 0.69 with the east-west height gradient and
# 0.04 with the north-south one -- a light due west, on the horizon as far as
# the shading is concerned. This keeps the west and puts it a little way up.
#
# SUN_FROM is a compass bearing in the panel's own frame (0 north, 90 east), so
# 270 is a sun in the west and the east faces of the hills are the dark ones.
# Note y counts *south* down the map (see MAP_LAT_SOUTH in src/panel.h), which
# is why the bearing's y component is negated when it becomes a vector.
SUN_FROM = 270.0

# How much the ground has to rise towards the sun, per map cell, to be lit a
# shade and a half above flat -- the same kind of reference SLOPE_REF is, and
# tuned the same way: at 0.02 the three example maps spend 2 to 6 per cent of
# their land in each of the end shades and the rest spread across the middle,
# which is about what resources/C1W.png does.
#
# It is scaled by the map's `feature` size, because doubling a landform's width
# halves its slopes: without that a near map would come out nearly unshaded and
# a distant one soot black, which is scale deciding how *lit* the map is rather
# than how big it is.
SUN_REF = 0.02
# A dither on the shade, in shades, for the same reason MOTTLE dithers the
# elevation ramp: six shades across a smooth dome is six visible bands.
SUN_MOTTLE = 0.55

# The bands the code asks the palette file for by name: the land ramp in
# ascending order, then the two that are not elevation at all.
LAND_BANDS = ("shore", "lowland", "highland", "peak")
BANDS = LAND_BANDS + ("water", "masonry")

# The land ramp is walked with a gamma, so its top bands are the top few per
# cent of the relief rather than an even slice of it. Linear put snow on a
# fifth of every map: the ramp is a set of bands with names, and "peak" has to
# mean the peaks.
RAMP_GAMMA = 1.8
GAMMA = F.gamma_table(RAMP_GAMMA, GAMMA_TOP)

# Rivers descend on a blurred copy of the terrain. On the raw surface a
# steepest-descent walk falls into the first noise pit it meets and stops after
# a few pixels; the blur is what gives it the macro slope to follow.
FLOW_BLUR = 16
RIVER_DEPTH = 0.012      # how far the channel is cut below its own banks

# The meander. It is added to the flow field only to *choose* between the
# neighbours that already run downhill, never to decide whether the river goes
# on at all: noise strong enough to bend a path is also strong enough to dig a
# pit in front of it, and a river that stops at the first one is a river three
# pixels long. Choosing among descenders cannot stall and cannot loop, because
# the blurred field falls at every step.
MEANDER = 0.006
# A bend every sixteen pixels, four map cells, at `scale: medium` -- it is taken
# off the scale's own lattice, so a near map's rivers bend on the same scale as
# its landforms rather than fidgeting across them.
MEANDER_OCTAVES = 4

# Where a river ends when the macro slope pits out before it reaches any water:
# the pool it would make, rather than nothing at all.
RIVER_POOL = 400
RIVER_POOL_RISE = 0.02

# A lake stops growing when its surface has risen this far above the basin it
# started in -- otherwise a shallow basin on a plain floods half the map before
# it reaches its area budget.
LAKE_RISE = 0.05


# --- noise --------------------------------------------------------------

def hash32(x, y, salt):
    """The lattice hash, unchanged in shape and moved to uint32.

    genmap.py already does this in integers -- it is the one part of the
    generator that was portable from the start -- so this is the same multiply
    and shift chain with numpy's uint64 masking replaced by the uint32 the
    45GS02 would use.
    """
    m = np.uint64(0xFFFFFFFF)
    h = ((x.astype(np.uint64) * np.uint64(0x1F1F1F1F)) & m) ^ \
        ((y.astype(np.uint64) * np.uint64(0x2545F491)) & m) ^ np.uint64(salt)
    h = (h ^ (h >> np.uint64(13))) & m
    h = (h * np.uint64(0x5BD1E995)) & m
    h = (h ^ (h >> np.uint64(15))) & m
    return h


def lattice_values(period, salt):
    """A period x period grid of hashed Q0.16 values.

    The float version divides the low sixteen bits by 65535 to reach 0..1;
    in Q0.16 the low sixteen bits *are* the value, which is one mask instead
    of a divide and the reason this representation was chosen.
    """
    y, x = np.meshgrid(np.arange(period), np.arange(period), indexing="ij")
    return (hash32(x, y, salt) & np.uint64(0xFFFF)).astype(np.int64)




def value_noise(size, period, salt):
    """One octave: bilinear over a hashed lattice, smoothstepped, in Q0.16.

    The weights are exact. `step` is a power of two, so the fraction of the
    way across a lattice cell is the low bits of the pixel index shifted up
    to Q0.16 -- no division, no rounding, and the same number the C version
    would compute from a mask and a shift.
    """
    if period > size:
        period = size
    step = size // period
    corner = lattice_values(period, salt)

    i0 = np.arange(size) // step
    i1 = (i0 + 1) % period
    shift = F.FRACBITS - (step.bit_length() - 1)
    w = F.smoothstep((np.arange(size) % step) << shift)

    top = F.lerp(corner[np.ix_(i0, i0)], corner[np.ix_(i0, i1)], w)
    bot = F.lerp(corner[np.ix_(i1, i0)], corner[np.ix_(i1, i1)], w)
    return F.lerp(top, bot, w[:, None])


def percentile(field, num, den):
    """The value at num/den of the way up `field`, by histogram, in Q0.16.

    A bucket is 1/1024 of full scale, and the interpolation inside it is not
    a nicety: this number scales the whole land ramp, so a bucket of error
    moves every pixel a fiftieth of a step and lands one in eight of them on
    the other side of a boundary. One divide, at setup, to place the cut
    within its bucket -- the counts either side of it are already in hand.
    """
    counts = np.bincount((field >> BUCKETSHIFT).ravel(), minlength=BUCKETS + 1)
    cum = np.cumsum(counts)
    want = int(field.size) * num // den
    b = int(np.searchsorted(cum, want))
    below = int(cum[b - 1]) if b else 0
    inside = int(counts[b])
    frac = ((want - below) << BUCKETSHIFT) // inside if inside else 0
    return (b << BUCKETSHIFT) + frac


def stretch(total):
    """The 0.5/99.5 percentile stretch, by histogram instead of by sort.

    numpy sorts a megapixel to find a percentile. The MEGA65 cannot, and does
    not need to: a histogram gives both cut points in one pass, and the divide
    by (hi - lo) is hoisted into a reciprocal so the second pass is a
    multiply.

    **The bucket width lands directly on the map's heights**, which is why it
    is 1024 buckets and not 256. Both ends of the stretch move by up to a
    bucket, and the whole field is scaled between them, so a 256-bucket
    histogram put the mountains map a systematic 0.77 height units below the
    float version -- not noise, a bias, visible as every summit being slightly
    lower. At 1024 it is a fifth of that and the cost is 4 KB of counters
    instead of 1 KB, which is nothing against the map itself.
    """
    lo = percentile(total, 5, 1000)
    hi = percentile(total, 995, 1000)
    span = max(hi - lo, 1)
    recip = (ONE * ONE) // span            # one divide, at setup
    return np.clip(F.scale(np.clip(total - lo, 0, None), recip), 0, ONE)


def fbm(size, octaves, gain, stream, ridged=False, base=LATTICE):
    """Octaves of value noise, in Q0.16. The float version's shape exactly.

    The weighted sum is accumulated at Q8.16 -- an octave sum reaches several
    times ONE before it is normalised -- and divided by the total weight
    through a reciprocal, which is the only place this differs from the float
    in more than rounding.
    """
    total = np.zeros((size, size), np.int64)
    weight = 0
    amp = ONE
    for o in range(octaves):
        period = base << o
        if period > size:
            break
        n = value_noise(size, period, stream.salt())
        oy, ox = stream.below(size), stream.below(size)
        n = np.roll(np.roll(n, oy, 0), ox, 1)
        total += F.scale(n, amp)
        weight += amp
        amp = F.mul(amp, gain)

    total = F.scale(total, (ONE * ONE) // weight)
    total = stretch(total)
    if ridged:
        total = ONE - np.abs(2 * total - ONE)
    return total


# --- macro shape --------------------------------------------------------

def island_mask(size, spec, stream):
    """Radial falloff with a noisy coastline, in Q0.16.

    The radius is the one per-pixel square root in the generator, which is
    what `fixed.sqrt`'s normalisation was written for: at the centre of the
    map the unnormalised table read is wrong by two height units, and the
    error is a visible ring.
    """
    axis = ((np.arange(size) * 2 - size) * ONE) // size      # -1..1 in Q0.16
    r2 = (axis[:, None] ** 2 + axis[None, :] ** 2) >> F.FRACBITS
    # Past the corners r2 exceeds 1.0, where sqrt's table saturates. Those
    # pixels are half a map outside the coast and mask to nothing anyway, so
    # the clip costs nothing and keeps the root inside its domain.
    r = F.sqrt(np.clip(r2, 0, ONE))

    wobble = F.scale(2 * value_noise(size, spec["lattice"], stream.salt()) - ONE,
                     int(ISLAND_WOBBLE * ONE))
    r = r + wobble
    recip = (ONE * ONE) // int(ISLAND_FADE * ONE)
    t = np.clip(F.scale(int(ISLAND_EDGE * ONE) - r, recip), 0, ONE)
    return F.smoothstep(t)


def base_terrain(size, spec, stream):
    """`type`, `ruggedness` and `scale`: the surface before any feature."""
    octaves, gain = RUGGEDNESS[spec["ruggedness"]]
    shape = TYPES[spec["type"]]

    n = fbm(size, max(2, octaves + SCALE[spec["scale"]]["octaves"]),
            int(gain * ONE), stream, ridged=shape["ridged"],
            base=spec["lattice"])
    h = int(shape["floor"] * ONE) + F.scale(n, int(shape["range"] * ONE))
    if spec["type"] == "island":
        h = F.scale(h, island_mask(size, spec, stream))
    return h


# --- features -----------------------------------------------------------

def add_hills(h, spec, stream, sea):
    """Bumps of the given size dropped on dry land, in Q0.16."""
    count = HILL_COUNTS[spec["hills"]]
    if not count:
        return
    radius, height = HILL_SIZE[spec["hills-size"]]
    radius = max(2, round(radius * spec["feature"] * h.shape[0] / DEFAULT_SIZE))
    height = int(height * TYPES[spec["type"]]["range"] * ONE)
    texture = value_noise(h.shape[0], spec["lattice"] << 3, stream.salt())

    placed = 0
    for _ in range(count * 8):
        if placed == count:
            break
        cy, cx = stream.below(h.shape[0]), stream.below(h.shape[0])
        if h[cy, cx] <= sea:
            continue
        stamp(h, cy, cx, radius,
              lambda d: F.scale(F.smoothstep(ONE - d), height), texture)
        placed += 1


def stamp(h, cy, cx, radius, profile, texture=None):
    """Add a radial profile at (cy, cx), wrapping. Q0.16 throughout.

    `profile` takes the distance from the centre as Q0.16 and returns Q0.16 to
    add. The distance is exact: `fixed.hypot` is the digit-by-digit root,
    because this is where a rounding error becomes a stair on a hillside.
    """
    size = h.shape[0]
    span = np.arange(-radius, radius + 1)
    # Measured at DISCBITS and shifted up to Q0.16 afterwards, so isqrt keeps
    # its 32-bit domain -- see fixed.hypot.
    d = (F.hypot(span[:, None], span[None, :], F.DISCBITS)
         << (F.FRACBITS - F.DISCBITS)) // radius
    inside = d <= ONE
    ys = (cy + span) % size
    xs = (cx + span) % size
    box = np.ix_(ys, xs)
    patch = np.where(inside, profile(np.clip(d, 0, ONE)), 0)
    if texture is not None:
        rough = int(HILL_ROUGH * ONE)
        patch = F.scale(patch, ONE - rough + 2 * F.scale(texture[box], rough))
    h[box] += patch


def box_blur(a, radius):
    """Separable moving average, wrapping, in Q0.16.

    The float version divides the window sum by its width. The width is
    2r + 1, never a power of two, so the divide is hoisted into a reciprocal
    and the running sum is multiplied by it -- one $D770 multiply per pixel
    instead of a division the machine does not have. The sum itself is at most
    33 values of 16 bits, which is why it can stay in 32 bits.
    """
    n = 2 * radius + 1
    recip = ((1 << 32) + n - 1) // n
    for axis in (0, 1):
        a = np.moveaxis(a, axis, 0)
        pad = np.concatenate([a[-radius:], a, a[:radius]], axis=0)
        c = np.cumsum(pad.astype(np.int64), axis=0)
        c = np.concatenate([np.zeros((1,) + a.shape[1:], np.int64), c], axis=0)
        a = ((c[n:] - c[:-n]) * recip) >> 32
        a = np.moveaxis(a, 0, axis)
    return a


def local_minima(h, water):
    """Cells no higher than any of their eight neighbours, and not already wet."""
    low = np.ones_like(h, bool)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            if dy or dx:
                low &= h <= np.roll(np.roll(h, dy, 0), dx, 1)
    return np.argwhere(low & ~water)


def flood_basin(h, water, level, cy, cx, budget, rise, blocked=None):
    """genmap.flood_basin with integer heights. The algorithm is unchanged.

    Including the spill point: the level is cut back to the lowest cell left
    on the frontier, or the lake stands above the beach beside it. See
    documentation/procedural-maps.md -- that trap cost a release's worth of
    dark blocks standing out of the sea, and porting it faithfully is cheaper
    than finding it twice.

    The heap is the one structure here that is not obviously 6502-shaped. Its
    entries are (height, y, x), which packs into four bytes, and a lake takes a
    few thousand of them -- chip RAM the generator has to spare, since it is
    stage one and owns the machine.
    """
    size = h.shape[0]
    if blocked is None:
        blocked = water
    start = int(h[cy, cx])
    seen = {(cy, cx)}
    frontier = [(start, cy, cx)]
    region = []
    surface = start
    while frontier and len(region) < budget:
        hh, y, x = heapq.heappop(frontier)
        if hh > start + rise:
            heapq.heappush(frontier, (hh, y, x))
            break
        surface = max(surface, hh)
        region.append((y, x))
        for ny, nx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
            ny, nx = ny % size, nx % size
            if (ny, nx) not in seen and not blocked[ny, nx]:
                seen.add((ny, nx))
                heapq.heappush(frontier, (int(h[ny, nx]), ny, nx))

    if frontier:
        surface = min(surface, min(hh for hh, _, _ in frontier))
    region = [(y, x) for y, x in region if h[y, x] <= surface]
    if not region:
        return 0

    ys = np.array([p[0] for p in region])
    xs = np.array([p[1] for p in region])
    water[ys, xs] = True
    level[ys, xs] = surface
    return len(region)


def fill_lakes(h, water, level, spec, stream):
    """Flood basins at local minima until each reaches its area budget.

    **The candidates are chosen by a histogram, not by a sort.** The float
    version argsorts every local minimum and keeps the lower half; there can
    be tens of thousands of them and a 45GS02 is not going to sort that. One
    pass of the same 1024-bucket histogram the stretch uses gives the median
    height, and "every minimum below the median" is the same set the sort was
    there to produce -- the order does not matter, because the next thing the
    float version does is shuffle it.
    """
    count = COUNTS[spec["lakes"]]
    if not count:
        return
    budget = round(LAKE_AREA[spec["lakes-size"]]
                   * (spec["feature"] * h.shape[0] / DEFAULT_SIZE) ** 2)

    minima = local_minima(h, water)
    if not len(minima):
        return
    depth = h[minima[:, 0], minima[:, 1]]
    counts = np.bincount(depth >> BUCKETSHIFT, minlength=BUCKETS + 1)
    half = int(np.searchsorted(np.cumsum(counts), max(len(depth) // 2, count)))
    candidates = minima[depth <= (half << BUCKETSHIFT)]
    if not len(candidates):
        candidates = minima

    placed = 0
    for pick in stream.pick(len(candidates), len(candidates)):
        if placed == count:
            break
        cy, cx = (int(v) for v in candidates[pick])
        if water[cy, cx]:
            continue
        placed += 1
        flood_basin(h, water, level, cy, cx, budget, int(LAKE_RISE * ONE))


def carve_rivers(h, water, level, spec, stream):
    """Steepest descent from high ground to the first water it reaches.

    Every part of this is already integer in shape -- comparisons, a blurred
    field, eight neighbours -- so the port is the constants and the disc.
    """
    count = COUNTS[spec["rivers"]]
    if not count:
        return
    radius = max(1, round(RIVER_SIZE[spec["river-size"]] * spec["feature"]
                          * h.shape[0] / DEFAULT_SIZE))
    size = h.shape[0]
    flow = box_blur(h, max(1, round(FLOW_BLUR * spec["feature"]
                                    * size / DEFAULT_SIZE)))
    wander = F.scale(2 * value_noise(size, spec["lattice"] << MEANDER_OCTAVES,
                                     stream.salt()) - ONE,
                     int(MEANDER * ONE))
    surface = h.copy()
    depth = int(RIVER_DEPTH * ONE)
    sea = int(spec["sea"] * ONE)

    lo, hi = int(h.min()), int(h.max())
    high = np.argwhere(~water & (h > lo + (66 * (hi - lo)) // 100))
    if not len(high):
        return

    for pick in stream.pick(len(high), count):
        y, x = (int(v) for v in high[pick])
        run = int(surface[y, x])
        standing = water.copy()
        for _ in range(4 * size):
            if standing[y, x]:
                break
            run = min(run, int(surface[y, x]) - depth)
            if run <= sea:
                break
            stamp_river(h, water, level, y, x, radius, run, standing)

            best, score = None, None
            here = flow[y, x]
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    if dy or dx:
                        ny, nx = (y + dy) % size, (x + dx) % size
                        if flow[ny, nx] < here:
                            s = flow[ny, nx] + wander[ny, nx]
                            if score is None or s < score:
                                best, score = (ny, nx), s
            if best is None:
                flood_basin(h, water, level, y, x,
                            round(RIVER_POOL * (spec["feature"] * size
                                                  / DEFAULT_SIZE) ** 2),
                            int(RIVER_POOL_RISE * ONE), blocked=standing)
                break
            y, x = best


def stamp_river(h, water, level, cy, cx, radius, run, standing):
    """One disc of channel at `run`, with water in it. Integer distances."""
    size = h.shape[0]
    span = np.arange(-radius, radius + 1)
    # The float version divides by radius + 0.5, so this doubles first and
    # divides by 2r + 1 -- in that order, or the low bits are gone before the
    # doubling can use them.
    d = ((F.hypot(span[:, None], span[None, :], F.DISCBITS)
          << (F.FRACBITS - F.DISCBITS)) * 2) // (radius * 2 + 1)
    ys, xs = (cy + span) % size, (cx + span) % size
    box = np.ix_(ys, xs)
    free = ~standing[box]

    bank = free & (d <= ONE)
    h[box] = np.where(bank, np.minimum(h[box], run + int(RIVER_DEPTH * ONE)),
                      h[box])
    # Cut, never build up -- the rule the wall of water at the waterline
    # taught. See documentation/procedural-maps.md.
    wet = free & (d <= (7 * ONE) // 10) & (h[box] >= run)
    h[box] = np.where(wet, run, h[box])
    water[box] |= wet
    level[box] = np.where(wet, run, level[box])


# --- the built things ---------------------------------------------------

# Half the base and how tall, in pixels and in height units at DEFAULT_SIZE --
# a map is 120 units tall and 1024 pixels across, so four pixels are a map cell
# and `medium` is 24 cells across and 10 tall. Measured off the pyramid in
# resources/C1W.png, which is about 100 pixels across and 51 units from the
# grass to the top platform: `medium` is that pyramid and the other two are a
# size either side of it. The height is about half the base in both, which is
# what makes the slope read as built rather than as a hill.
#
# **`scale` does not touch these.** It is how big the country is; a building is
# a building, and the whole point of putting a landmark on a distant-scale map
# is that it stands over country that got smaller.
PYRAMID = {
    "small":  (32, 28),
    "medium": (48, 42),
    "large":  (64, 56),
}

# A terrace is two map cells: one of riser and one of tread. That is a floor,
# not a taste -- **the renderer cannot see anything finer.** Built first with
# eight terraces of a pixel and a half each, the pyramid came out of the air as
# a smooth grey mound: the heightmap ships box-averaged to 512, the march
# samples half a cell at best and one cell for most of its range, and a terrace
# narrower than a cell is gone twice over before it reaches the screen. At two
# cells the steps survive both, and the colour carries them the rest of the way
# -- the riser band is a whole cell of the darker course, so a ray that lands on
# one reads terrace even where the geometry has averaged smooth.
TERRACE = 8              # pixels at DEFAULT_SIZE: two map cells
RISER = 4                # of which this much is the riser's own band

# The item types this file knows how to build, and the sizes each takes. An
# item type that is only a mission object -- a survivor, a landing site --
# does not belong in a map file at all: it belongs to whatever flies here, and
# the game holds it in src/mission.c today. So an unknown type is a typo and
# is refused rather than passed along.
ITEMS = {"pyramid": PYRAMID}


def place_items(h, water, items, size):
    """genmap.place_items in Q0.16. Terraces are integers already.

    The only arithmetic here is the median of the footprint, which the float
    version gets from numpy's sort. A pyramid covers 97x97 pixels at most, so
    the sort is small -- but the machine has a histogram already written for
    the stretch, and one that is 1024 buckets wide resolves the site to a
    ninth of a height unit, which is finer than the terraces it is levelling
    for.
    """
    built = np.full(h.shape, -1, np.int64)
    scale = size / DEFAULT_SIZE
    for n, item in enumerate(items):
        if item["type"] != "pyramid":
            continue
        half, height = PYRAMID[item["size"]]
        terrace = max(2, round(TERRACE * scale))
        riser = max(1, round(RISER * scale))
        steps = max(2, round(half * scale) // terrace)
        half = steps * terrace
        height = int(height * ONE) // HEIGHT_MAX
        cy, cx = round(item["y"] * scale), round(item["x"] * scale)

        span = np.arange(-half, half + 1)
        ys, xs = (cy + span) % size, (cx + span) % size
        box = np.ix_(ys, xs)
        if water[box].any():
            sys.exit(f"item {n} (pyramid) at {item['x']},{item['y']} stands in "
                     f"water; move it or shrink it -- it covers "
                     f"{round((2 * half + 1) / scale)} pixels of the map")

        out = np.maximum(np.abs(span)[:, None], np.abs(span)[None, :])
        tier = np.clip((half - out) // terrace + 1, 1, steps)
        band = ((half - out) % terrace) < riser

        base = min(percentile(h[box], 1, 2), ONE - height)
        h[box] = base + (height * tier) // steps
        built[box] = np.where(band, ONE, 0)
    return built


# --- colour -------------------------------------------------------------

def load_palette(path):
    """The shared band layout and the per-climate colours, checked over.

    The bands have to stay inside 16..223 and out of each other's way: below 16
    is the system's (the screen colour, and the panel ink, which has to be
    there because a text character's colour comes from a four-bit field), 224
    up is the sky and the reserved HUD pair, and anything convmap.py finds
    unused it hands to the sprite sheets.

    A shaded band's `count` is elevation *steps* and it claims one index per
    shade per step, so the overlap check has to know the shade count -- which
    is also why the check is worth having: re-cutting the ramp by hand now
    moves every base by a multiple of six.
    """
    with open(path) as f:
        pal = yaml.safe_load(f)

    missing = [b for b in BANDS if b not in (pal.get("bands") or {})]
    if missing:
        sys.exit(f"{path}: no band(s) {', '.join(missing)}. Every band this "
                 f"file names is one the generator asks for by name")

    shades = pal.get("shades")
    if not shades:
        sys.exit(f"{path}: no `shades:` list")
    if shades[len(shades) // 2] != 1.0:
        sys.exit(f"{path}: shades[{len(shades) // 2}] must be 1.00 -- flat "
                 f"ground is put on the middle entry, not looked up")

    seen = {}
    for name, band in pal["bands"].items():
        span = band["count"] * (1 if band.get("shaded") is False else len(shades))
        for i in range(span):
            entry = band["base"] + i
            if not 16 <= entry < 224:
                sys.exit(f"{path}: band {name} index {entry} is outside 16..223")
            if entry in seen:
                sys.exit(f"{path}: bands {seen[entry]} and {name} both claim {entry}")
            seen[entry] = name

    for climate, bands in pal["climates"].items():
        for name, band in pal["bands"].items():
            stops = bands.get(name)
            if not stops or (len(stops) < 2 and band["count"] > 1):
                sys.exit(f"{path}: climate {climate} needs at least two "
                         f"colour stops for band {name}")
    return pal


def band_colours(pal, climate, name):
    """One band's entries and their RGB, interpolated across its stops.

    A shaded band's entries are its elevation *steps*, so they are spaced by
    the shade count and each one is the base of that step's run of shades; an
    unshaded band (water) is spaced by one and its entries are all there is.
    """
    band = pal["bands"][name]
    stride = 1 if band.get("shaded") is False else len(pal["shades"])
    stops = np.asarray(pal["climates"][climate][name], float)
    entries = [band["base"] + i * stride for i in range(band["count"])]
    if band["count"] == 1:
        return entries, [tuple(int(round(v)) for v in stops[0])]

    t = np.linspace(0.0, len(stops) - 1, band["count"])
    lo = np.clip(t.astype(int), 0, len(stops) - 2)
    f = (t - lo)[:, None]
    out = stops[lo] * (1 - f) + stops[lo + 1] * f
    return entries, [tuple(int(round(v)) for v in c) for c in out]


def lit(colour, m):
    """One colour under one of the palette's shade multipliers.

    Below 1 it is a plain scale. Above 1 it is a lerp that far towards white
    instead, because the bright bands are already near it -- snow multiplied by
    1.24 clips to the same flat 255 across the top two shades, and a lit face
    of a peak stops being a face.
    """
    if m <= 1.0:
        return tuple(int(round(c * m)) for c in colour)
    return tuple(int(round(c + (255 - c) * (m - 1.0))) for c in colour)


def sunlight(h, spec):
    """How brightly the sun catches each pixel, 0..ONE across the shades.

    The gradient dot with a horizontal direction, then tanh. The sun's
    bearing is a constant, so its two components are constants too -- at 270
    degrees they are exactly -1 and 0, and the C version will carry them as
    Q0.16 rather than calling a sine.
    """
    cell = h.shape[0] // CELLS
    lx = int(round(np.sin(np.radians(SUN_FROM)) * ONE))
    ly = int(round(-np.cos(np.radians(SUN_FROM)) * ONE))

    hx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * cell // 2
    hy = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) * cell // 2
    rise = F.scale(hx, lx) + F.scale(hy, ly)

    ref = int(SUN_REF * spec["feature"] * ONE)
    return (ONE - F.tanh((rise << F.FRACBITS) // ref)) // 2


def colourise(h, bed, water, level, built, spec, pal, stream):
    """genmap.colourise in Q0.16. The palette itself stays in floats.

    **What the machine computes is the index, not the colour.** The RGB behind
    each entry is worked out once, on the PC, from `maps/palette.yaml`, and
    ships as a 768-byte table -- the generator never interpolates a colour
    stop or multiplies a channel by a shade. So the palette half of
    genmap.colourise is reused as it stands and only the per-pixel decision is
    ported, which is the half that has to run a million times.

    Every transcendental is a table: the ramp's gamma, the sun's tanh, and the
    slope's square root. Every division is a reciprocal: the ramp's top, the
    slope reference, the water's depth.
    """
    shades = pal["shades"]
    rgb = [(0, 0, 0)] * 256
    ramp = []
    for name in LAND_BANDS:
        entries, colours = band_colours(pal, spec["climate"], name)
        for e, c in zip(entries, colours):
            ramp.append(e)
            for l, m in enumerate(shades):
                rgb[e + l] = lit(c, m)
    deep, wcolours = band_colours(pal, spec["climate"], "water")
    for e, c in zip(deep, wcolours):
        rgb[e] = c
    stone, scolours = band_colours(pal, spec["climate"], "masonry")
    for e, c in zip(stone, scolours):
        for l, m in enumerate(shades):
            rgb[e + l] = lit(c, m)

    sea = int(spec["sea"] * ONE)
    land = h[~water & (built < 0)]
    top = percentile(land, 99, 100) if land.size else ONE
    t = F.scale(np.clip(h - sea, 0, None), (ONE * ONE) // max(top - sea, 1))

    cell = h.shape[0] // CELLS
    dy = np.roll(h, -1, 0) - np.roll(h, 1, 0)
    dx = np.roll(h, -1, 1) - np.roll(h, 1, 1)
    slope = F.scale(F.sqrt(np.clip((dy * dy + dx * dx) >> F.FRACBITS, 0, ONE)),
                    (cell * ONE) // 2)

    t = F.lookup(GAMMA, np.clip(t, 0, GAMMA_TOP * ONE) // GAMMA_TOP)
    ref = int(SLOPE_REF * ONE / spec["feature"])
    t = t + F.scale(np.clip(F.scale(slope, (ONE * ONE) // ref), 0, ONE),
                    int(SLOPE_PUSH * ONE))
    t = F.scale(t, int(TYPES[spec["type"]]["ceiling"] * ONE))

    mottle = F.scale(2 * value_noise(h.shape[0], h.shape[0] // 16,
                                     stream.salt()) - ONE,
                     int(MOTTLE * ONE))
    # One shift at the end, not one per term: the float truncates the sum.
    step = np.clip((t * len(ramp) + mottle) >> F.FRACBITS, 0, len(ramp) - 1)

    sun = sunlight(h, spec) * len(shades)
    smottle = F.scale(2 * value_noise(h.shape[0], h.shape[0] // 16,
                                      stream.salt()) - ONE,
                      int(SUN_MOTTLE * ONE))
    face = np.clip((sun + smottle) >> F.FRACBITS, 0, len(shades) - 1)

    indices = np.asarray(ramp, np.uint8)[step] + face.astype(np.uint8)

    course = np.clip(F.scale(built, len(stone) * ONE) >> F.FRACBITS,
                     0, len(stone) - 1)
    dressed = np.clip(sun >> F.FRACBITS, 0, len(shades) - 1).astype(np.uint8)
    indices = np.where(built >= 0,
                       np.asarray(stone, np.uint8)[course] + dressed, indices)

    depth = np.clip(F.scale(np.clip(level - bed, 0, None),
                            (ONE * ONE) // int(DEPTH_REF * ONE)), 0, ONE)
    wshade = np.clip(((ONE - depth) * len(deep)) >> F.FRACBITS, 0, len(deep) - 1)
    indices = np.where(water, np.asarray(deep, np.uint8)[wshade], indices)
    return indices.astype(np.uint8), rgb


# --- the YAML -----------------------------------------------------------

FIELDS = {
    "type": TYPES,
    "climate": None,            # checked against the palette file
    "rivers": COUNTS,
    "river-size": RIVER_SIZE,
    "hills": HILL_COUNTS,
    "hills-size": HILL_SIZE,
    "lakes": COUNTS,
    "lakes-size": LAKE_AREA,
    "ruggedness": RUGGEDNESS,
    "scale": SCALE,
}


def read_map(path):
    with open(path) as f:
        doc = yaml.safe_load(f)
    if not isinstance(doc, dict) or "general" not in doc:
        sys.exit(f"{path}: no `general:` block")
    spec = dict(doc["general"])

    for key in ("id", "seed"):
        if not isinstance(spec.get(key), int):
            sys.exit(f"{path}: general.{key} is required and must be an integer")
    if not 0 <= spec["id"] <= 99:
        sys.exit(f"{path}: general.id {spec['id']} is outside 0..99")

    for key, allowed in FIELDS.items():
        if key not in spec:
            sys.exit(f"{path}: general.{key} is required")
        if allowed is not None and spec[key] not in allowed:
            sys.exit(f"{path}: general.{key} is '{spec[key]}', "
                     f"expected one of {', '.join(sorted(allowed))}")

    # Items are placed by hand in the previewer, and every one of them is part
    # of the *world*: a pyramid is terrain. Anything that belongs to a mission
    # rather than to the map is refused above, so the check here is only that
    # what is asked for can be built.
    for n, item in enumerate(doc.get("items") or []):
        if not isinstance(item, dict) or "type" not in item:
            sys.exit(f"{path}: item {n} has no type")
        kind = item["type"]
        if kind not in ITEMS:
            sys.exit(f"{path}: item {n} is a '{kind}', expected one of "
                     f"{', '.join(sorted(ITEMS))}")
        for axis in ("x", "y"):
            if not isinstance(item.get(axis), int):
                sys.exit(f"{path}: item {n} ({kind}) has no integer {axis}")
        if item.get("size") not in ITEMS[kind]:
            sys.exit(f"{path}: item {n} ({kind}) is size '{item.get('size')}', "
                     f"expected one of {', '.join(sorted(ITEMS[kind]))}")
    return spec, doc.get("items") or []


def check_id_unique(path, spec):
    """No two map files may claim the same id.

    They would overwrite each other's map files, and once the maps are resident
    the id is also the attic RAM slot -- the whole point of it being one number
    is that nothing has to look up where a map lives, so a collision is silent
    in both places.
    """
    folder = os.path.dirname(os.path.abspath(path))
    for name in sorted(os.listdir(folder)):
        other = os.path.join(folder, name)
        if not name.endswith((".yaml", ".yml")) or os.path.samefile(other, path):
            continue
        with open(other) as f:
            doc = yaml.safe_load(f)
        if isinstance(doc, dict) and isinstance(doc.get("general"), dict):
            if doc["general"].get("id") == spec["id"]:
                sys.exit(f"{path}: id {spec['id']} is already used by {other}")


# --- output -------------------------------------------------------------

def write_maps(outdir, spec, heights, indices, rgb):
    hname = os.path.join(outdir, f"hmap{spec['id']:02d}.png")
    cname = os.path.join(outdir, f"cmap{spec['id']:02d}.png")

    Image.fromarray(heights, mode="L").save(hname)

    # Mode P, because convmap.py takes the colour map as palette indices and
    # refuses anything else -- there is no quantisation step anywhere in this
    # pipeline, the indices are chosen here and travel to the VIC-IV unchanged.
    im = Image.fromarray(indices, mode="P")
    im.putpalette([c for entry in rgb for c in entry])
    im.save(cname)
    return hname, cname


def main():
    ap = argparse.ArgumentParser(description="generate a map pair from a map file")
    ap.add_argument("mapfile", metavar="map.yaml")
    ap.add_argument("-o", "--outdir", help="where to write (default: beside the YAML)")
    ap.add_argument("--size", type=int, default=DEFAULT_SIZE)
    ap.add_argument("--palette", help="palette definition (default: palette.yaml "
                                      "beside the YAML)")
    args = ap.parse_args()

    if args.size < CELLS or args.size & (args.size - 1):
        sys.exit(f"--size {args.size} is not a power of two of at least {CELLS}")

    spec, items = read_map(args.mapfile)
    check_id_unique(args.mapfile, spec)
    folder = os.path.dirname(os.path.abspath(args.mapfile))
    pal = load_palette(args.palette or os.path.join(folder, "palette.yaml"))
    if spec["climate"] not in pal["climates"]:
        sys.exit(f"{args.mapfile}: general.climate is '{spec['climate']}', expected "
                 f"one of {', '.join(sorted(pal['climates']))}")

    size = args.size
    stream = F.Stream(spec["seed"])
    spec["sea"] = TYPES[spec["type"]]["sea"]
    spec["lattice"] = SCALE[spec["scale"]]["lattice"]
    spec["feature"] = SCALE[spec["scale"]]["feature"]
    sea = int(spec["sea"] * ONE)

    h = base_terrain(size, spec, stream)
    add_hills(h, spec, stream, sea)

    # The sea first, so lakes and rivers can see it and stop at it.
    water = h <= sea
    level = np.where(water, sea, -1)
    fill_lakes(h, water, level, spec, stream)
    carve_rivers(h, water, level, spec, stream)
    water |= h <= sea                         # carving can cut down to sea level
    level = np.where(water & (level < sea), sea, level)

    # Water surfaces are flat: the renderer has no idea what water is, so a
    # lake is only a lake because its heights are all the same. The bed is kept
    # for the colour, which shades by depth.
    bed = h.copy()
    h = np.where(water, level, h)

    # Last, and after the water: a built thing is not weathered by anything the
    # generator does, and nothing may flood it or cut a channel through it.
    built = place_items(h, water, items, size)

    indices, rgb = colourise(h, bed, water, level, built, spec, pal, stream)
    heights = np.clip((h * HEIGHT_MAX) >> F.FRACBITS, 0, 255).astype(np.uint8)

    outdir = args.outdir or folder
    os.makedirs(outdir, exist_ok=True)
    hname, cname = write_maps(outdir, spec, heights, indices, rgb)

    wet = float(water.mean())
    print(f"map {spec['id']:02d}: {spec['type']} / {spec['climate']} / "
          f"{spec['ruggedness']} / {spec['scale']}, seed {spec['seed']}, "
          f"{size}x{size}")
    print(f"  heights {heights.min()}..{heights.max()} of {HEIGHT_MAX}, "
          f"{wet * 100:.1f}% water, "
          f"{len(np.unique(indices))} palette entries used")
    print(f"  {hname}")
    print(f"  {cname}")
    for n, item in enumerate(items):
        scale = size / DEFAULT_SIZE
        ground = int(heights[round(item["y"] * scale),
                             round(item["x"] * scale)])
        print(f"  item {n}: {item['size']} {item['type']} at "
              f"{item['x']},{item['y']} (cell {item['x'] * CELLS // DEFAULT_SIZE},"
              f"{item['y'] * CELLS // DEFAULT_SIZE}), {ground} high. Built into "
              f"the maps")


if __name__ == "__main__":
    main()
