#!/usr/bin/env python3
"""The PC's answer to what the MEGA65 prints, for the on-device port.

`src/mapgen/noise.c` generates the weighted octave sum of a map's terrain
noise on the machine and prints a checksum of it. This prints the same number
from `tools/genmap.py`'s own `fbm_octaves`, so the two can be compared without
a memory dump -- which matters because `-dumpmem` writes chip RAM only and
cannot see attic RAM at all, where the field is.

**Identical checksums are a byte-for-byte proof through a sixteen-character
report.** That is the verification method
`documentation/on-device-maps.md` asks for, and it is meant to be run on the
first day of every stage of the port rather than after the first mystery.

    python3 tools/fbmcheck.py maps/island.yaml [--size 512] [--stage STAGE]

`--stage` says how far down the pipeline to go, and the stages are the ones the
device has ported so far:

    octaves   the weighted octave sum
    stretch   ... put through the percentile stretch
    shape     ... folded if the type is ridged, then onto the type's floor and
              range, which is genmap.py's base_terrain without its mask
    terrain   ... times the island mask: base_terrain itself
    hills     ... with the hills dropped on it
    minima    the lake candidates: local minima below the median, in scan
              order, checksummed as y then x rather than as a field

Each new pass gets a stage here on the day it is written, so that every one of
them has a number to be checked against rather than only the last.

The checksum is Fletcher's, over the field in the order the device writes it
(row by row, x fastest), with both accumulators taken mod 65536 rather than
65535: the device adds two 16-bit registers and lets them wrap, which is a
weaker checksum than the textbook one and an entirely adequate one for
catching a port that is wrong.
"""

import argparse
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])

import fixed as F  # noqa: E402
import genmap  # noqa: E402


def fletcher(field):
    """Two wrapping 16-bit sums over the field, x fastest. Returns b<<16 | a."""
    a = b = 0
    for row in field:
        for v in row:
            a = (a + int(v)) & 0xFFFF
            b = (b + a) & 0xFFFF
    return (b << 16) | a


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mapfile")
    ap.add_argument("--size", type=int, default=512)
    ap.add_argument("--stage", default="shape",
                    choices=("octaves", "stretch", "shape", "terrain", "hills",
                             "minima"))
    args = ap.parse_args()

    spec, _items = genmap.read_map(args.mapfile)
    octaves, gain = genmap.RUGGEDNESS[spec["ruggedness"]]
    scale = genmap.SCALE[spec["scale"]]
    octaves = max(2, octaves + scale["octaves"])
    base = scale["lattice"]

    # The stream has to be at the same point in its sequence as it is when
    # genmap.py reaches the same call, which for the octave sum means freshly
    # seeded: base_terrain is the first thing generate() does.
    stream = F.Stream(spec["seed"])
    if args.stage in ("terrain", "hills", "minima"):
        # genmap.py's own function, so the mask and the draw order after it
        # cannot drift from what the maps are actually built with.
        spec["lattice"] = base
        spec["feature"] = scale["feature"]
        spec["sea"] = genmap.TYPES[spec["type"]]["sea"]
        field = genmap.base_terrain(args.size, spec, stream)
        if args.stage in ("hills", "minima"):
            genmap.add_hills(field, spec, stream, int(spec["sea"] * F.ONE))
        if args.stage == "minima":
            import numpy as np
            sea = int(spec["sea"] * F.ONE)
            wet = field <= sea
            minima = genmap.local_minima(field, wet)
            depth = field[minima[:, 0], minima[:, 1]]
            counts = np.bincount(depth >> genmap.BUCKETSHIFT,
                                 minlength=genmap.BUCKETS + 1)
            want = max(len(depth) // 2, genmap.COUNTS[spec["lakes"]])
            half = int(np.searchsorted(np.cumsum(counts), want))
            cand = minima[depth <= (half << genmap.BUCKETSHIFT)]
            if not len(cand):
                cand = minima
            a = b = 0
            for cy, cx in cand:
                for v in (int(cy), int(cx)):
                    a = (a + v) & 0xFFFF
                    b = (b + a) & 0xFFFF
            print(f"{args.mapfile}: {len(minima)} minima, {len(cand)} candidates, "
                  f"median bucket {half}")
            print(f"stage minima: checksum {(b << 16) | a:08X}")
            return
        print(f"{args.mapfile}: size {args.size}, base {base}, "
              f"octaves {octaves}, gain {int(gain * F.ONE)}, seed {spec['seed']}")
        print(f"stage {args.stage}: checksum {fletcher(field):08X}")
        return
    field = genmap.fbm_octaves(args.size, octaves, int(gain * F.ONE), stream,
                               base=base)
    if args.stage in ("stretch", "shape"):
        field = genmap.stretch(field)
    if args.stage == "shape":
        shape = genmap.TYPES[spec["type"]]
        if shape["ridged"]:
            field = F.ONE - abs(2 * field - F.ONE)
        field = int(shape["floor"] * F.ONE) + F.scale(field,
                                                      int(shape["range"] * F.ONE))

    print(f"{args.mapfile}: size {args.size}, base {base}, "
          f"octaves {octaves}, gain {int(gain * F.ONE)}, seed {spec['seed']}")
    print(f"stage {args.stage}: checksum {fletcher(field):08X}")


if __name__ == "__main__":
    main()
