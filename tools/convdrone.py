#!/usr/bin/env python3
"""The title screen's drone, as two pairs of 16-colour hardware sprites.

    convdrone.py resources/drone.png build/drone.drn

The picture is two frames side by side, each DRONE_W x DRONE_H, which differ
only in the rotors: shown alternately they spin. A 16-colour sprite is sixteen
pixels wide -- eight bytes a row, a pixel a nybble, the left one high -- so a
frame is two sprites, its left and right halves, and the file is

    frame 0 left, frame 0 right, frame 1 left, frame 1 right

each DRONE_H rows of eight bytes, which is 192 at 24 rows and keeps every
block on the 64-byte boundary a sprite pointer needs. Then the colours, as
three planes of sixteen bytes, nybble-swapped for the registers.

Pixel 0 is transparent: a pixel whose alpha is under half. Every other colour
is rounded to the VIC-IV's four bits a channel and given an index from 1 in
the order it is first met; fifteen is the most a sprite can carry, and more is
refused rather than quietly merged. Crunched and padded like every other
resource on the disk. Keep the constants in step with src/drone.h.
"""

import argparse
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from convmap import TAIL_PAD, crunch, find_exomizer, nybswap  # noqa: E402

DRONE_W = 32
DRONE_H = 24
FRAMES = 2


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("png")
    ap.add_argument("out")
    args = ap.parse_args()

    im = Image.open(args.png).convert("RGBA")
    if im.size != (DRONE_W * FRAMES, DRONE_H):
        sys.exit(f"{args.png} is {im.width}x{im.height}; it wants {FRAMES} "
                 f"frames of {DRONE_W}x{DRONE_H} side by side")
    a = np.asarray(im).astype(np.int32)
    rgb4 = np.clip((a[..., :3] + 8) // 17, 0, 15)
    clear = a[..., 3] < 128

    colours = []
    index = np.zeros((DRONE_H, DRONE_W * FRAMES), dtype=np.uint8)
    for y in range(DRONE_H):
        for x in range(DRONE_W * FRAMES):
            if clear[y, x]:
                continue
            c = tuple(int(v) for v in rgb4[y, x])
            if c not in colours:
                colours.append(c)
            index[y, x] = colours.index(c) + 1
    if len(colours) > 15:
        sys.exit(f"{args.png} has {len(colours)} colours at four bits a "
                 f"channel; a 16-colour sprite holds 15 and a clear one")

    data = bytearray()
    for f in range(FRAMES):
        for half in range(2):
            x0 = f * DRONE_W + half * 16
            for y in range(DRONE_H):
                row = index[y, x0:x0 + 16]
                data += bytes((int(row[i]) << 4) | int(row[i + 1])
                              for i in range(0, 16, 2))

    planes = bytearray(48)
    for i, c in enumerate(colours, 1):
        for ch in range(3):
            planes[ch * 16 + i] = nybswap(c[ch] * 17)
    data += planes

    with open(args.out, "wb") as f:
        f.write(crunch(find_exomizer(), bytes(data)) + TAIL_PAD)
    print(f"drone: {FRAMES} frames of {DRONE_W}x{DRONE_H}, "
          f"{len(colours)} colours, {len(data)} bytes")


if __name__ == "__main__":
    main()
