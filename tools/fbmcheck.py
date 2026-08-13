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

    python3 tools/fbmcheck.py maps/island.yaml [--size 512]

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
    field = genmap.fbm_octaves(args.size, octaves, int(gain * F.ONE), stream,
                               base=base)

    print(f"{args.mapfile}: size {args.size}, base {base}, "
          f"octaves {octaves}, gain {int(gain * F.ONE)}, seed {spec['seed']}")
    print(f"checksum {fletcher(field):08X}")


if __name__ == "__main__":
    main()
