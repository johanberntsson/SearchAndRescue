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

Ported so far: the lattice hash, value noise, fbm with its stretch and fold,
the island mask, the macro shape, and the hills. Still float in genmap.py:
lakes, rivers, the pyramid, and the whole of colourise.
"""

import argparse
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
    counts = np.bincount((total >> BUCKETSHIFT).ravel(), minlength=BUCKETS + 1)
    cum = np.cumsum(counts)
    n = total.size
    lo = int(np.searchsorted(cum, n * 5 // 1000)) << BUCKETSHIFT
    hi = int(np.searchsorted(cum, n - n * 5 // 1000)) << BUCKETSHIFT
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


def terrain(size, spec, stream):
    """Everything ported so far, in one call: the surface before any water."""
    h = base_terrain(size, spec, stream)
    add_hills(h, spec, stream, int(spec["sea"] * ONE))
    return h


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

    fl = SameDraws(spec["seed"])
    a = G.base_terrain(size, spec, fl)
    G.add_hills(a, spec, fl, spec["sea"])

    fx = F.Stream(spec["seed"])
    b = np.asarray(terrain(size, spec, fx), float) / ONE

    d = np.abs(a - b) * G.HEIGHT_MAX
    wet_a, wet_b = a <= spec["sea"], b <= spec["sea"]
    print(f"{os.path.basename(mission)}: {spec['type']} / {spec['ruggedness']}"
          f" / {spec['scale']}, seed {spec['seed']}, {size}x{size}")
    print(f"  heights   float {a.min() * G.HEIGHT_MAX:6.1f}..{a.max() * G.HEIGHT_MAX:6.1f}"
          f"   fixed {b.min() * G.HEIGHT_MAX:6.1f}..{b.max() * G.HEIGHT_MAX:6.1f}"
          f"  of {G.HEIGHT_MAX}")
    print(f"  difference  mean {d.mean():5.2f}  median {np.median(d):5.2f}  "
          f"99th {np.percentile(d, 99):5.2f}  worst {d.max():5.2f} height units")
    print(f"  water     float {wet_a.mean() * 100:5.2f}%   fixed "
          f"{wet_b.mean() * 100:5.2f}%   coastline moved on "
          f"{(wet_a != wet_b).mean() * 100:.3f}% of the map")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("mission", nargs="*",
                    default=["maps/island.yaml", "maps/highlands.yaml",
                             "maps/plains.yaml"])
    ap.add_argument("--size", type=int, default=G.DEFAULT_SIZE)
    args = ap.parse_args()
    for m in args.mission:
        compare(m, args.size)


if __name__ == "__main__":
    main()
