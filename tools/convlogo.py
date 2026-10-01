#!/usr/bin/env python3
"""The title logo, as full-colour characters for the boot and title screens.

    convlogo.py screenshots/logo.png build/logo.lgo [--preview out.png]

The logo is a picture and the pages are text, but the display picks text or
full colour per character *number*, so a page can name LOGO_COLS x LOGO_ROWS
full-colour characters among its letters and they cost no more than letters
do. This writes them, the way `convmap.py --panel` writes the panel artwork:

  - the image is centred in LOGO_COLS x LOGO_ROWS characters, which must hold
    it -- it is not scaled, because a scaled logo is a blurred one;
  - every colour is rounded to the VIC-IV's four bits per channel *before*
    it is quantised, so no two entries are spent on one colour;
  - a pixel that rounds to black is 0, which a full-colour character draws
    in the screen colour -- black on every page -- so the background costs no
    entry at all;
  - the rest go in LOGO_COLOURS entries from LOGO_BASE. Those are the sky's
    and the panel artwork's (224..255), which no page shows: the logo borrows
    them while a page is up, and a flight's map_use() puts the whole palette
    back.

The file, once decrunched, is the characters in reading order and then the
palette as three planes of LOGO_COLOURS bytes, nybble-swapped for the
registers. It is crunched and padded like every other resource on the disk.
Keep the constants in step with src/screens.h.
"""

import argparse
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from convmap import TAIL_PAD, crunch, find_exomizer, nybswap  # noqa: E402

LOGO_COLS = 38
LOGO_ROWS = 4
LOGO_BASE = 224
LOGO_COLOURS = 256 - LOGO_BASE
W, H = LOGO_COLS * 8, LOGO_ROWS * 8


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("png")
    ap.add_argument("out")
    ap.add_argument("--preview", help="write what the machine will show")
    args = ap.parse_args()

    im = Image.open(args.png).convert("RGB")
    if im.width > W or im.height > H:
        sys.exit(f"{args.png} is {im.width}x{im.height}; the logo has "
                 f"{W}x{H} pixels of characters and is not scaled to fit")
    canvas = Image.new("RGB", (W, H))
    canvas.paste(im, ((W - im.width) // 2, (H - im.height) // 2))

    # Four bits a channel, as the palette registers hold it. Rounded rather
    # than truncated, and scaled back up so the quantiser sees the colours the
    # machine will actually show.
    a = np.asarray(canvas).astype(np.int32)
    a4 = np.clip((a + 8) // 17, 0, 15)
    black = (a4 == 0).all(axis=2)
    shown = Image.fromarray((a4 * 17).astype(np.uint8))

    q = shown.quantize(colors=LOGO_COLOURS, method=Image.MEDIANCUT,
                       dither=Image.Dither.NONE)
    qi = np.asarray(q)
    qp = q.getpalette()[:LOGO_COLOURS * 3]

    pixels = np.where(black, 0, qi.astype(np.int32) + LOGO_BASE)
    pixels = pixels.astype(np.uint8)

    chars = pixels.reshape(LOGO_ROWS, 8, LOGO_COLS, 8).transpose(0, 2, 1, 3)
    planes = bytearray(3 * LOGO_COLOURS)
    for i in range(LOGO_COLOURS):
        for c in range(3):
            v = qp[i * 3 + c] if i * 3 + c < len(qp) else 0
            planes[c * LOGO_COLOURS + i] = nybswap(v)

    data = chars.tobytes() + bytes(planes)
    with open(args.out, "wb") as f:
        f.write(crunch(find_exomizer(), data) + TAIL_PAD)

    used = len(np.unique(qi[~black]))
    print(f"logo: {LOGO_COLS}x{LOGO_ROWS} characters, {used} of "
          f"{LOGO_COLOURS} entries, {len(data)} bytes")

    if args.preview:
        rgb = np.zeros((256, 3), np.uint8)
        for i in range(LOGO_COLOURS):
            rgb[LOGO_BASE + i] = [(qp[i * 3 + c] >> 4) * 17 for c in range(3)]
        Image.fromarray(rgb[pixels]).resize((W * 3, H * 3), Image.NEAREST) \
            .save(args.preview)


if __name__ == "__main__":
    main()
