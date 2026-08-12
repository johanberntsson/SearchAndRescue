#!/usr/bin/env python3
"""Generate a height and colour map pair from a mission YAML.

    genmap.py <mission.yaml> [-o outdir] [--size N] [--palette FILE]

writes hmapNN.png (8-bit greyscale heights) and cmapNN.png (a palette image of
colour indices) beside the YAML, where NN is the `general.id` field.  Those are
exactly the two shapes tools/convmap.py already consumes, so nothing downstream
of here changes: convmap.py samples them down, crunches them and the build puts
them on the disk the same way it does the hand-drawn pair in resources/.

See documentation/procedural-maps.md for the design and the YAML schema.  This
is the first stage of it: terrain only.  mission.bin and the Python previewer
are not here yet, so the `items` block is parsed and checked but not yet
written anywhere.

Everything is drawn from one seeded RNG stream, so the same YAML gives byte
identical output on any machine, and the seed in the file is the whole of the
state needed to get a map back.

The algorithms are deliberately plain -- hash-based value noise on a lattice,
box blurs, steepest descent, priority flood -- because the design wants them
portable to C or 45GS02 later, and one function here is meant to become one
routine there.  numpy is a convenience for doing a megapixel at a time, not
something the generation logic leans on: no gradient tables, no library noise,
nothing that would have to be redesigned before it could move on-device.
"""

import argparse
import heapq
import os
import sys

import numpy as np
import yaml
from PIL import Image

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

# The land ramp is walked with a gamma, so its top bands are the top few per
# cent of the relief rather than an even slice of it. Linear put snow on a
# fifth of every map: the ramp is a set of bands with names, and "peak" has to
# mean the peaks.
RAMP_GAMMA = 1.8

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


# --- the one RNG stream -------------------------------------------------

class Stream:
    """The single seeded source everything draws from.

    Noise salts come out of it in sequence rather than being constants, so two
    octaves never share a lattice and the whole map still reproduces from the
    seed alone. Keep draws in a fixed order: changing the order changes the
    map, which is a feature when it is deliberate and a nuisance when it is
    not.
    """

    def __init__(self, seed):
        self.rng = np.random.default_rng(seed)

    def salt(self):
        return int(self.rng.integers(0, 1 << 30))

    def uniform(self, lo, hi, n=None):
        return self.rng.uniform(lo, hi, n)

    def choice(self, n, k):
        """k distinct indices from range(n), or all of them if there are fewer."""
        return self.rng.permutation(n)[:k]


# --- noise --------------------------------------------------------------

def hash32(x, y, salt):
    """A 32-bit integer hash of a lattice point. One multiply-shift chain.

    Written on uint64 and masked back to 32 bits at every step so it means the
    same thing here as it would in C with uint32_t, rather than relying on how
    numpy happens to wrap.
    """
    m = np.uint64(0xFFFFFFFF)
    h = ((x.astype(np.uint64) * np.uint64(0x1F1F1F1F)) & m) ^ \
        ((y.astype(np.uint64) * np.uint64(0x2545F491)) & m) ^ np.uint64(salt)
    h = (h ^ (h >> np.uint64(13))) & m
    h = (h * np.uint64(0x5BD1E995)) & m
    h = (h ^ (h >> np.uint64(15))) & m
    return h


def lattice_values(period, salt):
    """A period x period grid of hashed values in 0..1."""
    y, x = np.meshgrid(np.arange(period), np.arange(period), indexing="ij")
    return (hash32(x, y, salt) & np.uint64(0xFFFF)).astype(np.float64) / 65535.0


def smoothstep(t):
    return t * t * (3.0 - 2.0 * t)


def value_noise(size, period, salt):
    """One octave: bilinear interpolation over a hashed lattice, smoothstepped.

    The lattice wraps at `period`, so the noise tiles -- which matters because
    the world wraps too (map coordinates are 8.8 fixed point in a uint16_t, so
    flying off one edge arrives at the other). A map whose noise did not tile
    would show a seam there.
    """
    if period > size:
        period = size
    step = size // period
    corner = lattice_values(period, salt)

    i0 = np.arange(size) // step
    i1 = (i0 + 1) % period
    w = smoothstep((np.arange(size) % step) / step)

    top = corner[np.ix_(i0, i0)] * (1 - w) + corner[np.ix_(i0, i1)] * w
    bot = corner[np.ix_(i1, i0)] * (1 - w) + corner[np.ix_(i1, i1)] * w
    return top * (1 - w[:, None]) + bot * w[:, None]


def fbm(size, octaves, gain, stream, ridged=False, base=LATTICE):
    """Octaves of value noise, each twice the frequency and `gain` the weight.

    Ridged folds the sum about its middle, which turns the smooth hills of
    plain value noise into creased ridges with valleys between them -- the
    difference between rolling country and a mountain range.

    Folding each octave separately is the textbook ridged multifractal and was
    tried first. It creases along every lattice's own mid-contour, so the
    ridges come out boxy and follow the noise grid; folding once at the end
    creases along a contour of the finished field instead, and the ridges are
    long and sinuous. It is also one operation on one array rather than one
    per octave, which is the cheaper of the two to port.
    """
    total = np.zeros((size, size))
    weight = 0.0
    amp = 1.0
    for o in range(octaves):
        period = base << o
        if period > size:
            break
        n = value_noise(size, period, stream.salt())
        # Every octave is sampled at its own offset, which is a roll of a
        # tiling array and so still tiles. Without it all the lattices share
        # their cell boundaries and the sum comes out visibly quilted -- most
        # of all when ridged folds each octave about its middle, which puts a
        # crease on exactly those lines.
        oy, ox = (int(v) for v in stream.uniform(0, size, 2))
        n = np.roll(np.roll(n, oy, 0), ox, 1)
        total += amp * n
        weight += amp
        amp *= gain
    total /= weight

    # Stretched to the full 0..1 before anything else looks at it. A sum of
    # octaves is a bell around the middle and never reaches either end, so
    # without this a `range` of 0.8 delivers about half of that, and -- worse
    # for the fold below -- almost every pixel sits near 0.5, which is exactly
    # where the crease is. Percentiles rather than the extremes, so one
    # outlying lattice corner cannot set the scale for the whole map.
    lo, hi = np.percentile(total, (0.5, 99.5))
    total = np.clip((total - lo) / max(hi - lo, 1e-6), 0.0, 1.0)

    if ridged:
        total = 1.0 - np.abs(2.0 * total - 1.0)
    return total


# --- macro shape --------------------------------------------------------

def island_mask(size, spec, stream):
    """Radial falloff from the centre, with a noisy coastline.

    The mask is what makes an island an island; the noise on the radius is what
    stops it being a disc. Because everything outside it is under water, the
    map still tiles even though the mask itself is not periodic: sea meets sea
    at the seam.
    """
    axis = (np.arange(size) - size / 2.0) / (size / 2.0)
    r = np.sqrt(axis[:, None] ** 2 + axis[None, :] ** 2)
    r = r + ISLAND_WOBBLE * (2.0 * value_noise(size, spec["lattice"], stream.salt()) - 1.0)
    return smoothstep(np.clip((ISLAND_EDGE - r) / ISLAND_FADE, 0.0, 1.0))


def base_terrain(size, spec, stream):
    """`type`, `ruggedness` and `scale`: the surface before any feature."""
    octaves, gain = RUGGEDNESS[spec["ruggedness"]]
    shape = TYPES[spec["type"]]

    n = fbm(size, max(2, octaves + SCALE[spec["scale"]]["octaves"]), gain, stream,
            ridged=shape["ridged"], base=spec["lattice"])
    h = shape["floor"] + shape["range"] * n
    if spec["type"] == "island":
        h = h * island_mask(size, spec, stream)
    return h


# --- features -----------------------------------------------------------

def add_hills(h, spec, stream, sea):
    """Bumps of the given size dropped on dry land.

    A shouldered dome rather than a cone, so the hill blends into whatever it
    lands on instead of creasing it; and the placement is rejected if it falls
    in water, because a hill in the sea is an island nobody asked for.

    Every dome is modulated by a noise field, because a radial function alone
    puts a perfect circle on the map -- which the eye finds immediately from
    the air, and which the colour ramp then crowns with a round bald summit.
    """
    count = HILL_COUNTS[spec["hills"]]
    if not count:
        return
    radius, height = HILL_SIZE[spec["hills-size"]]
    radius = max(2, round(radius * spec["feature"] * h.shape[0] / DEFAULT_SIZE))
    height *= TYPES[spec["type"]]["range"]
    texture = value_noise(h.shape[0], spec["lattice"] << 3, stream.salt())

    placed = 0
    for _ in range(count * 8):
        if placed == count:
            break
        cy, cx = (int(v) for v in stream.uniform(0, h.shape[0], 2))
        if h[cy, cx] <= sea:
            continue
        stamp(h, cy, cx, radius, lambda d: height * smoothstep(1.0 - d), texture)
        placed += 1


def stamp(h, cy, cx, radius, profile, texture=None):
    """Add a radial profile to the map at (cy, cx), wrapping at the edges.

    `profile` takes the distance from the centre as a fraction of the radius
    and returns what to add. Wrapping matters for the same reason the noise
    tiles: a feature that fell off the edge would leave a half-hill at a seam
    the pilot can fly across.
    """
    size = h.shape[0]
    span = np.arange(-radius, radius + 1)
    d = np.sqrt(span[:, None] ** 2 + span[None, :] ** 2) / radius
    inside = d <= 1.0
    ys = (cy + span) % size
    xs = (cx + span) % size
    box = np.ix_(ys, xs)
    patch = np.where(inside, profile(np.clip(d, 0.0, 1.0)), 0.0)
    if texture is not None:
        patch = patch * (1.0 - HILL_ROUGH + 2.0 * HILL_ROUGH * texture[box])
    h[box] += patch


def box_blur(a, radius):
    """Separable moving average, wrapping. Two passes of a running sum."""
    n = 2 * radius + 1
    for axis in (0, 1):
        a = np.moveaxis(a, axis, 0)
        pad = np.concatenate([a[-radius:], a, a[:radius]], axis=0)
        c = np.cumsum(pad, axis=0)
        c = np.concatenate([np.zeros((1,) + a.shape[1:]), c], axis=0)
        a = (c[n:] - c[:-n]) / n
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
    """Flood the basin around one cell until it fills `budget` cells.

    A priority flood: the region grows by repeatedly swallowing its lowest
    frontier cell, so the water level only ever rises and the region is exactly
    the basin that level fills. The surface is the highest cell taken, which is
    the lip it would spill over.

    A basin too shallow to hold the budget hits `rise` first and keeps whatever
    it had -- better a small lake than a flooded plain.

    **Both of those stop the fill early, and a level taken from the highest
    cell swallowed is then water standing above the land beside it.** The
    frontier is still holding cells lower than that level, and they stay dry:
    the lake keeps its shape but gains a rim it is pouring over, drawn as a
    wall of water several units proud of the ground. It is worst where the sea
    blocks the fill -- a coastal basin runs out of budget instead of reaching a
    shore -- and from the air it is a row of dark blocks standing out of the
    water, which is how it was found. So the level is cut back to the lowest
    cell left on the frontier, which is the point the basin would actually
    spill at, and anything above the new level is not part of the lake.

    `blocked` is the water this pool may not grow into, and it is not always
    the live map: a pool at the end of a river is surrounded by that river's
    own channel, so blocking on live water walls it in at one pixel. Passing
    the water as it stood before the river set out lets the pool back up into
    its own channel, which is what a pool does, while still refusing to spread
    into a lake or the sea.
    """
    size = h.shape[0]
    if blocked is None:
        blocked = water
    start = h[cy, cx]
    seen = {(cy, cx)}
    frontier = [(float(start), cy, cx)]
    region = []
    surface = start
    while frontier and len(region) < budget:
        hh, y, x = heapq.heappop(frontier)
        if hh > start + rise:
            heapq.heappush(frontier, (hh, y, x))   # it is a spill point too
            break
        surface = max(surface, hh)
        region.append((y, x))
        for ny, nx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
            ny, nx = ny % size, nx % size
            # Standing water is not somewhere a basin can grow into. Left in,
            # the frontier reaches the coast, finds the sea to be the lowest
            # thing available and spends the whole area budget painting ocean
            # at the lake's own surface level -- a raised sheet of water
            # halfway across the map.
            if (ny, nx) not in seen and not blocked[ny, nx]:
                seen.add((ny, nx))
                heapq.heappush(frontier, (float(h[ny, nx]), ny, nx))

    # Where the basin would spill: the lowest cell still on the frontier, which
    # is dry ground adjacent to the lake. Water cannot stand above it, so it is
    # a ceiling on the level, and the cells the lower level no longer covers
    # are not lake. Every remaining cell was popped in increasing order, so the
    # ones that survive are a prefix and the lake keeps its shape.
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

    A priority flood: the region grows by repeatedly swallowing its lowest
    frontier cell, so the water level only ever rises and the region is exactly
    the basin that level fills. The surface is the highest cell taken, which is
    the lip it would spill over.

    A basin that is too shallow to hold the budget hits LAKE_RISE first and
    keeps whatever it had -- better a small lake than a flooded plain.
    """
    count = COUNTS[spec["lakes"]]
    if not count:
        return
    # Areas, so the scale's length multiplier goes in squared.
    budget = round(LAKE_AREA[spec["lakes-size"]]
                   * (spec["feature"] * h.shape[0] / DEFAULT_SIZE) ** 2)
    size = h.shape[0]

    minima = local_minima(h, water)
    if not len(minima):
        return
    # The lower half of the minima, shuffled, and taken until enough lakes are
    # placed. Taking the lowest few instead put every lake in one basin: a flat
    # basin floor is thousands of cells that are all no higher than their
    # neighbours, so the lowest candidates are all the same puddle. A candidate
    # that some earlier lake has already flooded is skipped for the same
    # reason, which is what actually spreads them out.
    order = np.argsort(h[minima[:, 0], minima[:, 1]])
    candidates = minima[order[:max(len(order) // 2, count)]]
    placed = 0
    for pick in stream.choice(len(candidates), len(candidates)):
        if placed == count:
            break
        cy, cx = (int(v) for v in candidates[pick])
        if water[cy, cx]:
            continue
        placed += 1
        flood_basin(h, water, level, cy, cx, budget, LAKE_RISE)


def carve_rivers(h, water, level, spec, stream):
    """Steepest descent from high ground to the first water it reaches.

    The walk follows a blurred copy of the terrain, because the real surface
    has a noise pit every few pixels and a raw descent stops in the first one.
    The channel is cut into the real surface though, and its floor is kept
    monotonically falling -- once the river has been down to a level it never
    climbs back, so a channel is always something water could run along rather
    than a groove draped over the noise.

    **The channel is measured against the terrain as it was before any of this
    ran.** Against the live map it digs itself in: a disc flattens the ground
    several pixels ahead of the walk, so when the walk arrives there the ground
    it reads is its own channel floor and it cuts RIVER_DEPTH below that again.
    Every step took another notch out, and after thirty of them the river was
    below sea level -- which the sea pass downstream then flattens and paints
    in the deepest shade there is. That is a black canyon across the map, and
    it is what a river looked like here until the pristine copy was kept.
    """
    count = COUNTS[spec["rivers"]]
    if not count:
        return
    radius = max(1, round(RIVER_SIZE[spec["river-size"]] * spec["feature"]
                          * h.shape[0] / DEFAULT_SIZE))
    size = h.shape[0]
    flow = box_blur(h, max(1, round(FLOW_BLUR * spec["feature"] * size / DEFAULT_SIZE)))
    wander = MEANDER * (2.0 * value_noise(size, spec["lattice"] << MEANDER_OCTAVES,
                                          stream.salt()) - 1.0)
    surface = h.copy()

    # Sources in the top third of the terrain, so a river has somewhere to run
    # from; below that it would meet water within a few steps.
    high = np.argwhere(~water & (h > h.min() + 0.66 * (h.max() - h.min())))
    if not len(high):
        return

    for pick in stream.choice(len(high), count):
        y, x = (int(v) for v in high[pick])
        run = surface[y, x]
        # The water that was already standing when this river set out: the sea,
        # the lakes, and whatever earlier rivers cut. It is both where this one
        # stops and where it may not write, because a channel arriving at the
        # coast is still well above sea level and stamping its own surface over
        # the sea would raise a plateau of water out in the bay. Its own
        # channel has to be excluded from the test, or it would stop on the
        # first pixel it had just cut.
        standing = water.copy()
        for _ in range(4 * size):
            if standing[y, x]:
                break            # reached the sea, a lake or another river
            run = min(run, surface[y, x] - RIVER_DEPTH)
            if run <= spec["sea"]:
                break            # down to the sea plane: there is nothing lower
            stamp_river(h, water, level, y, x, radius, run, standing)

            # Every neighbour that is genuinely downhill, and the meander picks
            # between them. The test that ends the river is on the blurred
            # field alone, so the wander can bend the path without ever being
            # able to stop it.
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
                # A pit in the macro slope. The river has to end in *something*
                # -- a blue thread stopping in the middle of a plain is the one
                # thing you notice from the air -- so it ends in the pool it
                # would actually make, which on a plain with nowhere to drain
                # is what happens to a river anyway.
                flood_basin(h, water, level, y, x,
                            round(RIVER_POOL * (spec["feature"] * size
                                                / DEFAULT_SIZE) ** 2),
                            RIVER_POOL_RISE, blocked=standing)
                break
            y, x = best


def stamp_river(h, water, level, cy, cx, radius, run, standing):
    """Cut one disc of channel at `run` and put water in it."""
    size = h.shape[0]
    span = np.arange(-radius, radius + 1)
    d = np.sqrt(span[:, None] ** 2 + span[None, :] ** 2) / (radius + 0.5)
    ys, xs = (cy + span) % size, (cx + span) % size
    box = np.ix_(ys, xs)
    free = ~standing[box]

    # The banks are cut too, one shoulder wider than the water, so the channel
    # has sides rather than being a flat strip laid over the hillside.
    bank = free & (d <= 1.0)
    h[box] = np.where(bank, np.minimum(h[box], run + RIVER_DEPTH), h[box])
    # The channel floor is *set* to the run rather than cut down to it, so the
    # bed and the surface are the same number and the colour reads the river as
    # the shallow water it is. Cutting left a pixel that an earlier, higher
    # disc had made wet sitting over a bed a later, lower one had dug, and the
    # difference came out as deep-water colour along half of every river.
    # `h[box] >= run` is the same rule the lake's spill point is: a channel may
    # be *cut* into the ground but never built up out of it. Without it a disc
    # crossing a slope raises its downhill half to the run, and the river comes
    # out running along the top of a low wall of its own water. The centre
    # always passes -- the run was taken RIVER_DEPTH below it -- so the channel
    # cannot be broken by this, only narrowed where the ground falls away.
    wet = free & (d <= 0.7) & (h[box] >= run)
    h[box] = np.where(wet, run, h[box])
    water[box] |= wet
    # Plain assignment, not a maximum: `run` only ever falls, and consecutive
    # discs overlap heavily, so keeping the higher of the two would hold the
    # surface at whatever the river was doing upstream and leave every channel
    # reading as deep water. The standing water this must not overwrite is
    # already excluded by `free`.
    level[box] = np.where(wet, run, level[box])


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
    """How brightly the sun catches each pixel, 0..1 across the shade list.

    Not a Lambert dot against a surface normal but the plainer thing the
    hand-drawn map turns out to be doing: how fast the ground rises *towards
    the sun*, which is one dot product of the gradient with a horizontal
    direction. Its luminance correlates 0.72 with the east-west gradient and
    0.04 with the north-south one, which is what a light on the horizon gives
    and what a normal-based model with a sun 40 degrees up does not.

    A full Lambert was written first and measured worse in a way worth
    recording: it needs the relief exaggerated into cell units before a normal
    means anything, and that exaggeration then interacts with each map type's
    own height range, so the same constant put 71% of a mountain range in the
    two end shades and left a plain with no shading at all. Rise-towards-the-sun
    against one reference steepness behaves the same on all three.

    tanh rather than a clip, because the interesting terrain is the gentle
    two-thirds around flat: a linear ramp steep enough to shade *that* piles
    every hillside up in the end shades. Flat ground returns exactly 0.5, which
    is the middle shade and the palette's 1.00 entry, so level ground comes out
    in the climate's own colours whether it is shaded or not.
    """
    cell = h.shape[0] / CELLS
    hx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) / 2.0 * cell
    hy = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) / 2.0 * cell

    # Note the minus: a lit face is one where the ground *falls* towards the
    # sun. Walk west into a sun in the west and a hillside that rises under you
    # is the one turning its back on it, which is the east face -- exactly the
    # face the pilot sees dark on the hand-drawn map.
    az = np.radians(SUN_FROM)
    lx, ly = np.sin(az), -np.cos(az)
    rise = hx * lx + hy * ly
    return 0.5 - 0.5 * np.tanh(rise / (SUN_REF * spec["feature"]))


def colourise(h, bed, water, level, spec, pal, stream):
    """Height, slope, sun and water into palette indices, and the palette.

    The land bands are one contiguous ramp, so the whole of the land decision
    is: how far up the terrain is this pixel, plus how steep it is, plus a
    little dither -- and the answer indexes straight into the ramp. Where the
    band boundaries fall is a matter of what colours the palette puts there and
    nothing the code has to know about.

    The ramp is two dimensional now: every elevation step owns one entry per
    shade, so the index is the step's base plus which way the ground faces the
    sun. The map is still one add and one lookup away from a height, and the
    renderer never learns that any of this happened -- it reads a palette index
    per cell as it always did.
    """
    shades = pal["shades"]
    rgb = [(0, 0, 0)] * 256
    ramp = []
    for name in ("shore", "lowland", "highland", "peak"):
        entries, colours = band_colours(pal, spec["climate"], name)
        for e, c in zip(entries, colours):
            ramp.append(e)
            for l, m in enumerate(shades):
                rgb[e + l] = lit(c, m)
    deep, wcolours = band_colours(pal, spec["climate"], "water")
    for e, c in zip(deep, wcolours):
        rgb[e] = c

    # The top of the ramp is the map's own high ground rather than a constant,
    # so flatlands use the whole ramp instead of coming out uniformly shore
    # coloured. The 99th percentile and not the maximum: one hill peak should
    # not spend the top half of the ramp on a dozen pixels.
    land = h[~water]
    top = np.percentile(land, 99) if land.size else 1.0
    sea = spec["sea"]
    t = (h - sea) / max(top - sea, 1e-6)

    dy = np.roll(h, -1, 0) - np.roll(h, 1, 0)
    dx = np.roll(h, -1, 1) - np.roll(h, 1, 1)
    slope = np.sqrt(dy ** 2 + dx ** 2) / 2.0 * (h.shape[0] / CELLS)
    t = np.clip(t, 0.0, None) ** RAMP_GAMMA
    t = t + SLOPE_PUSH * np.clip(slope / (SLOPE_REF / spec["feature"]), 0.0, 1.0)
    t = t * TYPES[spec["type"]]["ceiling"]

    # Both dithers are added after the scaling, in steps and in shades, so each
    # one means the same thing however the palette is re-cut. They are separate
    # noise fields on purpose: one field would put the same pixels at the top of
    # their elevation step and the top of their shade, which is a texture rather
    # than a dither.
    step = t * len(ramp) + MOTTLE * (
        2.0 * value_noise(h.shape[0], h.shape[0] // 16, stream.salt()) - 1.0)
    step = np.clip(step.astype(int), 0, len(ramp) - 1)

    face = sunlight(h, spec) * len(shades) + SUN_MOTTLE * (
        2.0 * value_noise(h.shape[0], h.shape[0] // 16, stream.salt()) - 1.0)
    face = np.clip(face.astype(int), 0, len(shades) - 1)

    indices = np.asarray(ramp, np.uint8)[step] + face.astype(np.uint8)

    # Water is shaded by how deep it is over the bed the terrain would have
    # had, which is why the bed is kept before the surface is flattened onto
    # it: a lake edge and a lake middle are the same height and should not be
    # the same colour.
    depth = np.clip((level - bed) / DEPTH_REF, 0.0, 1.0)
    wshade = np.clip(((1.0 - depth) * len(deep)).astype(int), 0, len(deep) - 1)
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


def read_mission(path):
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

    # Items are placed by hand in the previewer and read by the game out of
    # mission.bin -- neither of which exists yet. Checked here anyway so a
    # typo is caught now rather than in whatever writes that file.
    for n, item in enumerate(doc.get("items") or []):
        if not isinstance(item, dict) or "type" not in item:
            sys.exit(f"{path}: item {n} has no type")
        for axis in ("x", "y"):
            if not isinstance(item.get(axis), int):
                sys.exit(f"{path}: item {n} ({item['type']}) has no integer {axis}")
    return spec, doc.get("items") or []


def check_id_unique(path, spec):
    """No two mission YAMLs may claim the same id.

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
    ap = argparse.ArgumentParser(description="generate a map pair from a mission YAML")
    ap.add_argument("mission")
    ap.add_argument("-o", "--outdir", help="where to write (default: beside the YAML)")
    ap.add_argument("--size", type=int, default=DEFAULT_SIZE)
    ap.add_argument("--palette", help="palette definition (default: palette.yaml "
                                      "beside the YAML)")
    args = ap.parse_args()

    if args.size < CELLS or args.size & (args.size - 1):
        sys.exit(f"--size {args.size} is not a power of two of at least {CELLS}")

    spec, items = read_mission(args.mission)
    check_id_unique(args.mission, spec)
    folder = os.path.dirname(os.path.abspath(args.mission))
    pal = load_palette(args.palette or os.path.join(folder, "palette.yaml"))
    if spec["climate"] not in pal["climates"]:
        sys.exit(f"{args.mission}: general.climate is '{spec['climate']}', expected "
                 f"one of {', '.join(sorted(pal['climates']))}")

    size = args.size
    stream = Stream(spec["seed"])
    spec["sea"] = TYPES[spec["type"]]["sea"]
    spec["lattice"] = SCALE[spec["scale"]]["lattice"]
    spec["feature"] = SCALE[spec["scale"]]["feature"]

    h = base_terrain(size, spec, stream)
    add_hills(h, spec, stream, spec["sea"])

    # The sea first, so lakes and rivers can see it and stop at it.
    water = h <= spec["sea"]
    level = np.where(water, spec["sea"], -1.0)
    fill_lakes(h, water, level, spec, stream)
    carve_rivers(h, water, level, spec, stream)
    water |= h <= spec["sea"]                 # carving can cut down to sea level
    level = np.where(water & (level < spec["sea"]), spec["sea"], level)

    # Water surfaces are flat: the renderer has no idea what water is, so a
    # lake is only a lake because its heights are all the same. The bed is kept
    # for the colour, which shades by depth.
    bed = h.copy()
    h = np.where(water, level, h)

    indices, rgb = colourise(h, bed, water, level, spec, pal, stream)
    heights = np.clip(h * HEIGHT_MAX, 0, 255).round().astype(np.uint8)

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
    if items:
        print(f"  {len(items)} item(s) parsed, not yet written "
              f"(mission.bin is the next stage)")


if __name__ == "__main__":
    main()
