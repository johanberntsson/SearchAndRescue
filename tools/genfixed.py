#!/usr/bin/env python3
"""The map generator's terrain stages, in integers, on the way to the MEGA65.

    genfixed.py <mission.yaml> [--size N]      # compare against genmap.py

`tools/genmap.py` is written in floats and cannot move to a 45GS02.
`documentation/on-device-maps.md` costs the port and says what has to happen
first: the arithmetic changes *here*, on the PC, where the previewer and
`checkview.py` can see it, and only then does it become C. This file is that
change in progress -- the same pipeline, stage by stage, against
`tools/fixed.py`'s Q0.16 integers and its xorshift stream.

**It is deliberately a second file, and only until it is finished.** The game
on the disk today is not to move while this is unproven, and neither is the map
under `documentation/reference/`, so `genmap.py` keeps producing exactly what it
produced this morning and this one is compared against it. When the whole
pipeline is here and the difference is understood, this becomes genmap.py's
insides and the float version goes; two generators is not a state to keep.

What it does *not* try to do is match the float version bit for bit. Fixed
point is a different arithmetic: a smoothstep truncating at 1/65536 puts a
lattice corner one height unit lower than a double would, and by the fifth
octave that has moved a coastline by a pixel. The question this file has to
answer is the useful one -- **is the terrain the same terrain** -- which is
what the comparison at the bottom measures, in height units and in the
percentage of pixels that land in a different colour band.

**The whole pipeline is here now**: the lattice hash, value noise, fbm with its
stretch and fold, the island mask, the macro shape, hills, lakes, rivers, the
pyramid and colourise. `--write` runs it for real and produces the same two
PNGs genmap.py does, which the previewer flies without knowing which arithmetic
drew them.

What the comparison reports, over the three example missions:

- **terrain**, given the same draws: worst 0.01-0.03 of one height unit in 120.
- **colour**, given the same terrain and water: 4-15% of land pixels land on a
  different entry, none of them by more than one ramp step or one sun shade.
  That is below the dither the generator already adds on purpose -- MOTTLE is
  1.4 steps -- so it is smaller than the noise it is measured against.
- **water** cannot be compared pixel by pixel and is compared as water: how
  much, in how many bodies, how big. The choosing differs, not the arithmetic
  (see fill_lakes).
"""

import argparse
import heapq
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fixed as F                                         # noqa: E402
import genmap as G                                        # noqa: E402

ONE = F.ONE

# The stretch's histogram: 1024 buckets of the field, see stretch().
BUCKETSHIFT = 6
BUCKETS = ONE >> BUCKETSHIFT


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


def fbm(size, octaves, gain, stream, ridged=False, base=G.LATTICE):
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
                     int(G.ISLAND_WOBBLE * ONE))
    r = r + wobble
    recip = (ONE * ONE) // int(G.ISLAND_FADE * ONE)
    t = np.clip(F.scale(int(G.ISLAND_EDGE * ONE) - r, recip), 0, ONE)
    return F.smoothstep(t)


def base_terrain(size, spec, stream):
    """`type`, `ruggedness` and `scale`: the surface before any feature."""
    octaves, gain = G.RUGGEDNESS[spec["ruggedness"]]
    shape = G.TYPES[spec["type"]]

    n = fbm(size, max(2, octaves + G.SCALE[spec["scale"]]["octaves"]),
            int(gain * ONE), stream, ridged=shape["ridged"],
            base=spec["lattice"])
    h = int(shape["floor"] * ONE) + F.scale(n, int(shape["range"] * ONE))
    if spec["type"] == "island":
        h = F.scale(h, island_mask(size, spec, stream))
    return h


# --- features -----------------------------------------------------------

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
        rough = int(G.HILL_ROUGH * ONE)
        patch = F.scale(patch, ONE - rough + 2 * F.scale(texture[box], rough))
    h[box] += patch


def add_hills(h, spec, stream, sea):
    """Bumps of the given size dropped on dry land, in Q0.16."""
    count = G.HILL_COUNTS[spec["hills"]]
    if not count:
        return
    radius, height = G.HILL_SIZE[spec["hills-size"]]
    radius = max(2, round(radius * spec["feature"] * h.shape[0] / G.DEFAULT_SIZE))
    height = int(height * G.TYPES[spec["type"]]["range"] * ONE)
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


# --- water --------------------------------------------------------------

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
    count = G.COUNTS[spec["lakes"]]
    if not count:
        return
    budget = round(G.LAKE_AREA[spec["lakes-size"]]
                   * (spec["feature"] * h.shape[0] / G.DEFAULT_SIZE) ** 2)

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
        flood_basin(h, water, level, cy, cx, budget, int(G.LAKE_RISE * ONE))


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
    h[box] = np.where(bank, np.minimum(h[box], run + int(G.RIVER_DEPTH * ONE)),
                      h[box])
    # Cut, never build up -- the rule the wall of water at the waterline
    # taught. See documentation/procedural-maps.md.
    wet = free & (d <= (7 * ONE) // 10) & (h[box] >= run)
    h[box] = np.where(wet, run, h[box])
    water[box] |= wet
    level[box] = np.where(wet, run, level[box])


def carve_rivers(h, water, level, spec, stream):
    """Steepest descent from high ground to the first water it reaches.

    Every part of this is already integer in shape -- comparisons, a blurred
    field, eight neighbours -- so the port is the constants and the disc.
    """
    count = G.COUNTS[spec["rivers"]]
    if not count:
        return
    radius = max(1, round(G.RIVER_SIZE[spec["river-size"]] * spec["feature"]
                          * h.shape[0] / G.DEFAULT_SIZE))
    size = h.shape[0]
    flow = box_blur(h, max(1, round(G.FLOW_BLUR * spec["feature"]
                                    * size / G.DEFAULT_SIZE)))
    wander = F.scale(2 * value_noise(size, spec["lattice"] << G.MEANDER_OCTAVES,
                                     stream.salt()) - ONE,
                     int(G.MEANDER * ONE))
    surface = h.copy()
    depth = int(G.RIVER_DEPTH * ONE)
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
                            round(G.RIVER_POOL * (spec["feature"] * size
                                                  / G.DEFAULT_SIZE) ** 2),
                            int(G.RIVER_POOL_RISE * ONE), blocked=standing)
                break
            y, x = best


# --- the built things ---------------------------------------------------

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
    scale = size / G.DEFAULT_SIZE
    for n, item in enumerate(items):
        if item["type"] != "pyramid":
            continue
        half, height = G.PYRAMID[item["size"]]
        terrace = max(2, round(G.TERRACE * scale))
        riser = max(1, round(G.RISER * scale))
        steps = max(2, round(half * scale) // terrace)
        half = steps * terrace
        height = int(height * ONE) // G.HEIGHT_MAX
        cy, cx = round(item["y"] * scale), round(item["x"] * scale)

        span = np.arange(-half, half + 1)
        ys, xs = (cy + span) % size, (cx + span) % size
        box = np.ix_(ys, xs)
        if water[box].any():
            sys.exit(f"item {n} (pyramid) at {item['x']},{item['y']} stands in "
                     f"water")

        out = np.maximum(np.abs(span)[:, None], np.abs(span)[None, :])
        tier = np.clip((half - out) // terrace + 1, 1, steps)
        band = ((half - out) % terrace) < riser

        base = min(percentile(h[box], 1, 2), ONE - height)
        h[box] = base + (height * tier) // steps
        built[box] = np.where(band, ONE, 0)
    return built


# --- colour -------------------------------------------------------------

# The ramp's gamma, over 0..2 rather than 0..1: `t` is a height over the map's
# own 99th percentile and the top one per cent of the terrain is above 1.0 by
# construction. See fixed.gamma_table.
GAMMA_TOP = 2
GAMMA = F.gamma_table(G.RAMP_GAMMA, GAMMA_TOP)


def sunlight(h, spec):
    """How brightly the sun catches each pixel, 0..ONE across the shades.

    The gradient dot with a horizontal direction, then tanh. The sun's
    bearing is a constant, so its two components are constants too -- at 270
    degrees they are exactly -1 and 0, and the C version will carry them as
    Q0.16 rather than calling a sine.
    """
    cell = h.shape[0] // G.CELLS
    lx = int(round(np.sin(np.radians(G.SUN_FROM)) * ONE))
    ly = int(round(-np.cos(np.radians(G.SUN_FROM)) * ONE))

    hx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * cell // 2
    hy = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) * cell // 2
    rise = F.scale(hx, lx) + F.scale(hy, ly)

    ref = int(G.SUN_REF * spec["feature"] * ONE)
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
    for name in G.LAND_BANDS:
        entries, colours = G.band_colours(pal, spec["climate"], name)
        for e, c in zip(entries, colours):
            ramp.append(e)
            for l, m in enumerate(shades):
                rgb[e + l] = G.lit(c, m)
    deep, wcolours = G.band_colours(pal, spec["climate"], "water")
    for e, c in zip(deep, wcolours):
        rgb[e] = c
    stone, scolours = G.band_colours(pal, spec["climate"], "masonry")
    for e, c in zip(stone, scolours):
        for l, m in enumerate(shades):
            rgb[e + l] = G.lit(c, m)

    sea = int(spec["sea"] * ONE)
    land = h[~water & (built < 0)]
    top = percentile(land, 99, 100) if land.size else ONE
    t = F.scale(np.clip(h - sea, 0, None), (ONE * ONE) // max(top - sea, 1))

    cell = h.shape[0] // G.CELLS
    dy = np.roll(h, -1, 0) - np.roll(h, 1, 0)
    dx = np.roll(h, -1, 1) - np.roll(h, 1, 1)
    slope = F.scale(F.sqrt(np.clip((dy * dy + dx * dx) >> F.FRACBITS, 0, ONE)),
                    (cell * ONE) // 2)

    t = F.lookup(GAMMA, np.clip(t, 0, GAMMA_TOP * ONE) // GAMMA_TOP)
    ref = int(G.SLOPE_REF * ONE / spec["feature"])
    t = t + F.scale(np.clip(F.scale(slope, (ONE * ONE) // ref), 0, ONE),
                    int(G.SLOPE_PUSH * ONE))
    t = F.scale(t, int(G.TYPES[spec["type"]]["ceiling"] * ONE))

    mottle = F.scale(2 * value_noise(h.shape[0], h.shape[0] // 16,
                                     stream.salt()) - ONE,
                     int(G.MOTTLE * ONE))
    # One shift at the end, not one per term: the float truncates the sum.
    step = np.clip((t * len(ramp) + mottle) >> F.FRACBITS, 0, len(ramp) - 1)

    sun = sunlight(h, spec) * len(shades)
    smottle = F.scale(2 * value_noise(h.shape[0], h.shape[0] // 16,
                                      stream.salt()) - ONE,
                      int(G.SUN_MOTTLE * ONE))
    face = np.clip((sun + smottle) >> F.FRACBITS, 0, len(shades) - 1)

    indices = np.asarray(ramp, np.uint8)[step] + face.astype(np.uint8)

    course = np.clip(F.scale(built, len(stone) * ONE) >> F.FRACBITS,
                     0, len(stone) - 1)
    dressed = np.clip(sun >> F.FRACBITS, 0, len(shades) - 1).astype(np.uint8)
    indices = np.where(built >= 0,
                       np.asarray(stone, np.uint8)[course] + dressed, indices)

    depth = np.clip(F.scale(np.clip(level - bed, 0, None),
                            (ONE * ONE) // int(G.DEPTH_REF * ONE)), 0, ONE)
    wshade = np.clip(((ONE - depth) * len(deep)) >> F.FRACBITS, 0, len(deep) - 1)
    indices = np.where(water, np.asarray(deep, np.uint8)[wshade], indices)
    return indices.astype(np.uint8), rgb


def terrain(size, spec, stream):
    """Everything ported so far, in one call: the surface, and its water."""
    sea = int(spec["sea"] * ONE)
    h = base_terrain(size, spec, stream)
    add_hills(h, spec, stream, sea)

    water = h <= sea
    level = np.where(water, sea, -1)
    fill_lakes(h, water, level, spec, stream)
    carve_rivers(h, water, level, spec, stream)
    water |= h <= sea
    level = np.where(water & (level < sea), sea, level)
    bed = h.copy()
    h = np.where(water, level, h)
    return h, bed, water, level


# --- the comparison -----------------------------------------------------

class SameDraws:
    """genmap.Stream's interface, answered from the xorshift stream.

    Without this the comparison measures nothing. The float generator draws
    its octave offsets and its hill placements from numpy's PCG64 and this one
    from xorshift32, so the two build *different maps* however good the
    arithmetic is -- 8 height units apart on average, which says only that the
    seeds re-rolled. Feeding both pipelines the same draws leaves the
    arithmetic as the only difference between them, which is the thing under
    test. The draws have to arrive in the same order for that to hold, so the
    two-value form of uniform() makes two below() calls exactly as the fixed
    point side does.
    """

    def __init__(self, seed):
        self.fx = F.Stream(seed)

    def salt(self):
        return self.fx.salt()

    def uniform(self, lo, hi, n=None):
        if n is None:
            return float(lo) + self.fx.below(int(hi - lo))
        return np.array([float(lo) + self.fx.below(int(hi - lo))
                         for _ in range(n)], float)

    def choice(self, n, k):
        return np.array(self.fx.pick(n, k), int)


def compare(mission, size):
    """The fixed-point terrain against genmap.py's float terrain.

    Not a bit-for-bit check -- see the module docstring, it cannot be one --
    but the two questions that matter: how far apart are the surfaces in the
    units the map is quantised to, and do they still describe the same
    country. The second is answered by how much of the map changes which side
    of the sea plane it is on, since a coastline moving is the difference
    anyone would actually see.
    """
    spec, _ = G.read_mission(mission)
    spec["sea"] = G.TYPES[spec["type"]]["sea"]
    spec["lattice"] = G.SCALE[spec["scale"]]["lattice"]
    spec["feature"] = G.SCALE[spec["scale"]]["feature"]

    # Both pipelines, once each, all the way through the water.
    fl = SameDraws(spec["seed"])
    a = G.base_terrain(size, spec, fl)
    G.add_hills(a, spec, fl, spec["sea"])
    terrain_a = a.copy()

    fx = F.Stream(spec["seed"])
    hb = base_terrain(size, spec, fx)
    add_hills(hb, spec, fx, int(spec["sea"] * ONE))
    terrain_b = np.asarray(hb, float) / ONE

    d = np.abs(terrain_a - terrain_b) * G.HEIGHT_MAX
    wet_a, wet_b = terrain_a <= spec["sea"], terrain_b <= spec["sea"]
    print(f"{os.path.basename(mission)}: {spec['type']} / {spec['ruggedness']}"
          f" / {spec['scale']}, seed {spec['seed']}, {size}x{size}")
    print(f"  terrain     float {terrain_a.min() * G.HEIGHT_MAX:6.1f}.."
          f"{terrain_a.max() * G.HEIGHT_MAX:6.1f}   fixed "
          f"{terrain_b.min() * G.HEIGHT_MAX:6.1f}..{terrain_b.max() * G.HEIGHT_MAX:6.1f}"
          f"  of {G.HEIGHT_MAX}")
    print(f"  difference  mean {d.mean():5.2f}  median {np.median(d):5.2f}  "
          f"99th {np.percentile(d, 99):5.2f}  worst {d.max():5.2f} height units")
    print(f"  coastline   moved on {(wet_a != wet_b).mean() * 100:.3f}% of the map")

    # The water, which cannot be compared pixel by pixel -- see below.
    wa = terrain_a <= spec["sea"]
    la = np.where(wa, spec["sea"], -1.0)
    G.fill_lakes(a, wa, la, spec, fl)
    G.carve_rivers(a, wa, la, spec, fl)
    wb, lb = water_of(hb, spec, fx)

    pa = pieces(wa & (la > spec["sea"]))
    pb = pieces(wb & (lb > int(spec["sea"] * ONE)))
    print(f"  water       float {wa.mean() * 100:5.2f}%   fixed "
          f"{wb.mean() * 100:5.2f}% of the map")
    print(f"  inland      float {len(pa):3d} bodies, largest {pa[:3]}")
    print(f"              fixed {len(pb):3d} bodies, largest {pb[:3]}")

    # And the whole way through: the palette index every pixel ends up with.
    #
    # **Both sides are re-seeded here**, and that is the second time this
    # harness has had to be told to stop measuring the stream. colourise draws
    # two noise fields for its dithers, and by this point the two pipelines
    # have consumed different numbers of draws -- the lakes above see to that
    # -- so the dithers were different fields entirely and 70% of pixels
    # "differed" by a step of dither. Given the same noise, the arithmetic
    # speaks for itself.
    pal = G.load_palette(os.path.join(os.path.dirname(os.path.abspath(mission)),
                                      "palette.yaml"))
    _, items = G.read_mission(mission)
    seed = spec["seed"] ^ 0xC010A

    bed_a = a.copy()
    a = np.where(wa, la, a)
    bed_b = hb.copy()
    hb = np.where(wb, lb, hb)
    try:
        built_a = G.place_items(a, wa, items, size)
        built_b = place_items(hb, wb, items, size)
    except SystemExit as why:
        # The two sides put their lakes in different basins (see above), so a
        # site that is dry in the real generator can be under water in one
        # half of this harness. That is the comparison's problem, not the
        # generator's: drop the items and compare the terrain colour, which is
        # what the remaining 99.99% of the map is.
        print(f"  items       skipped -- {why}")
        built_a = np.full(a.shape, -1.0)
        built_b = np.full(hb.shape, -1, np.int64)
    idx_a, _ = G.colourise(a, bed_a, wa, la, built_a, spec, pal, SameDraws(seed))
    idx_b, _ = colourise(hb, bed_b, wb, lb, built_b, spec, pal, F.Stream(seed))

    # An index is a ramp step plus a sun shade, and those are the units worth
    # reporting: a whole step is six entries, so a raw index distance says
    # nothing about whether the colour moved a shade or a band.
    ramp = []
    for name in G.LAND_BANDS:
        entries, _ = G.band_colours(pal, spec["climate"], name)
        ramp += entries
    ramp = np.asarray(ramp)

    def split(idx):
        step = np.clip(np.searchsorted(ramp, idx, side="right") - 1, 0, None)
        return step, idx.astype(int) - ramp[step]

    # Only where the two agree on what is water: a lake in another basin is
    # not a colour difference, it is a different lake, and it is counted above.
    dry = ~wa & ~wb
    sa, fa = split(idx_a[dry])
    sb, fb = split(idx_b[dry])
    print(f"  colour      of the land both agree on, "
          f"{(idx_a[dry] != idx_b[dry]).mean() * 100:5.2f}% of pixels differ")
    print(f"              ramp step  {(sa != sb).mean() * 100:5.2f}%, worst "
          f"{np.abs(sa - sb).max()}   sun shade {(fa != fb).mean() * 100:5.2f}%,"
          f" worst {np.abs(fa - fb).max()}")

    # ...and again with the float pipeline's own terrain and water handed to
    # both, which is the only way to see the *colour* arithmetic on its own.
    # End to end, most of the difference above is the ramp's top: it is the
    # 99th percentile of the land, the two sides put their lakes in different
    # basins, so they are taking a percentile over different sets of pixels and
    # the whole ramp shifts a step under them.
    idx_c, _ = colourise(np.round(a * ONE).astype(np.int64),
                         np.round(bed_a * ONE).astype(np.int64), wa,
                         np.round(la * ONE).astype(np.int64),
                         np.where(built_a >= 0,
                                  np.round(built_a * ONE), -1).astype(np.int64),
                         spec, pal, F.Stream(seed))
    sc, fc = split(idx_c[~wa])
    sd, fd = split(idx_a[~wa])
    print(f"  arithmetic  on the float pipeline's own terrain and water, "
          f"{(idx_a[~wa] != idx_c[~wa]).mean() * 100:5.2f}% differ")
    print(f"              ramp step  {(sc != sd).mean() * 100:5.2f}%, worst "
          f"{np.abs(sc - sd).max()}   sun shade {(fc != fd).mean() * 100:5.2f}%,"
          f" worst {np.abs(fc - fd).max()}")


def water_of(h, spec, stream):
    """The fixed pipeline's lakes and rivers over an already built terrain."""
    sea = int(spec["sea"] * ONE)
    water = h <= sea
    level = np.where(water, sea, -1)
    fill_lakes(h, water, level, spec, stream)
    carve_rivers(h, water, level, spec, stream)
    return water, level


def pieces(wet):
    """Connected water bodies and their sizes, four-connected, iterative.

    Only ever called on *inland* water. The sea is one enormous piece and
    would drown the numbers that say anything.
    """
    seen = np.zeros_like(wet)
    out = []
    for sy, sx in zip(*np.nonzero(wet)):
        if seen[sy, sx]:
            continue
        n, stack = 0, [(sy, sx)]
        seen[sy, sx] = True
        while stack:
            y, x = stack.pop()
            n += 1
            for ny, nx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
                ny, nx = ny % wet.shape[0], nx % wet.shape[1]
                if wet[ny, nx] and not seen[ny, nx]:
                    seen[ny, nx] = True
                    stack.append((ny, nx))
        out.append(n)
    return sorted(out, reverse=True)


def write(mission, size, outdir):
    """Run the whole fixed pipeline and write the two PNGs, like genmap.py.

    This is the proof that the port is a generator and not a comparison: the
    same YAML in, the same two shapes out, and `tools/preview.py` will fly the
    result without knowing which arithmetic drew it.
    """
    spec, items = G.read_mission(mission)
    G.check_id_unique(mission, spec)
    folder = os.path.dirname(os.path.abspath(mission))
    pal = G.load_palette(os.path.join(folder, "palette.yaml"))
    spec["sea"] = G.TYPES[spec["type"]]["sea"]
    spec["lattice"] = G.SCALE[spec["scale"]]["lattice"]
    spec["feature"] = G.SCALE[spec["scale"]]["feature"]

    stream = F.Stream(spec["seed"])
    h, bed, water, level = terrain(size, spec, stream)
    built = place_items(h, water, items, size)
    indices, rgb = colourise(h, bed, water, level, built, spec, pal, stream)
    heights = np.clip((h * G.HEIGHT_MAX) >> F.FRACBITS, 0, 255).astype(np.uint8)

    os.makedirs(outdir, exist_ok=True)
    hname, cname = G.write_maps(outdir, spec, heights, indices, rgb)
    print(f"map {spec['id']:02d}: {spec['type']} / {spec['climate']} / "
          f"{spec['ruggedness']} / {spec['scale']}, seed {spec['seed']}, "
          f"{size}x{size}, in integers")
    print(f"  heights {heights.min()}..{heights.max()} of {G.HEIGHT_MAX}, "
          f"{water.mean() * 100:.1f}% water, "
          f"{len(np.unique(indices))} palette entries used")
    print(f"  {hname}\n  {cname}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("mission", nargs="*",
                    default=["maps/island.yaml", "maps/highlands.yaml",
                             "maps/plains.yaml"])
    ap.add_argument("--size", type=int, default=G.DEFAULT_SIZE)
    ap.add_argument("--write", metavar="OUTDIR",
                    help="generate for real and write the PNGs there, instead "
                         "of comparing against genmap.py")
    args = ap.parse_args()
    for m in args.mission:
        if args.write:
            write(m, args.size, args.write)
        else:
            compare(m, args.size)


if __name__ == "__main__":
    main()
