#!/usr/bin/env python3
"""Fly a generated map on the PC, with the MEGA65's own renderer.

    preview.py [maps/island.yaml] [--scale 2] [--fps 12.5] [--shot out.png]

Opens a window on the map `tools/genmap.py` wrote from that map file and flies it
with the game's controls, so terrain can be judged the way it will actually be
seen -- from the air, at the draw distance the march really has -- instead of
from a PNG viewed from above. It is also where item coordinates come from:
fly to a spot, press `M`, and the position is printed in the form the map file
wants and appended to a marks file.

**This is the game's renderer, not a lookalike.** The march schedule, the
projection, the map sampling, the sky, the flight model and the readouts are
all the ones in `src/`, and the constants they need are *read out of the C
source* at startup rather than copied here -- see `read_constants`. What comes
out is the same picture the MEGA65 draws, at the same 12.5 frames a second,
with the same 120 cells of draw distance and the same band edges popping.

Two things it does not do, deliberately: no wind (it would blow you off a spot
while you were noting it down), and no terrain following in any mode -- the
ground clamp is there, but nothing here can crash. Nothing in the game changes
by anything in this file; it only reads.

Requires numpy, Pillow and tkinter, all of which the build already needs bar
tkinter (`python3-tkinter` on Fedora, `python3-tk` on Debian).
"""

import argparse
import os
import re
import sys
import time

import numpy as np
import yaml
from PIL import Image, ImageTk

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import convmap                                            # noqa: E402
import genmap                                             # noqa: E402

# The build's defaults, and what the maps are sampled down to before anything
# is drawn -- so the preview shows the resolution that ships, not the 1024x1024
# the generator wrote.
DEFAULT_HGT_SIZE = convmap.DEFAULT_HGT_SIZE
DEFAULT_COL_SIZE = convmap.DEFAULT_COL_SIZE

# 160 rays across a 320 pixel buffer, each filling the two pixels it owns.
# WIDE=1 marched all 320 and is retired (src/loader.h #errors on it), so this
# is not read from the C: the constant there is inside an #if.
VX_COLS = 160

# The MEGA65 manages about 12.5 frames a second, and every rate in the flight
# model is per *frame* -- so a preview running at 60 would fly six times as
# fast as the machine. Pace it the same and the feel carries over.
DEFAULT_FPS = 12.5

MARK_SUFFIX = ".marks.yaml"


# --- the constants, read out of src/ ------------------------------------

# Plain `#define NAME <integer>` in the file named. Copying them here instead
# would be one more pair of numbers to keep in step, and this project's history
# is mostly about what happens when two copies of a constant disagree.
C_DEFINES = {
    "voxel.c":  ["BAND_STEPS", "BANDS", "Z_NEAR", "Z_STEP0", "SCALE_H",
                 "CAM_BIAS"],
    "voxel.h":  ["SKY_BASE", "SKY_SHADES", "TAN_HALF_FOV"],
    "vic4.h":   ["FB_WIDTH", "FB_HEIGHT"],
    "main.c":   ["TURN_RATE", "CLIMB_RATE", "GROUND_GAP", "TILT_RATE",
                 "TILT_MIN", "TILT_MAX"],
    "panel.h":  ["MAP_LAT_SOUTH", "MAP_LON_WEST"],
    "sprite.c": ["SPR_WORLD_H", "SPR_Z_NEAR", "SPR_Z_FAR"],
}


class Constants:
    """What `src/` says the renderer is, loaded once at startup."""

    def __init__(self, src):
        self.src = src
        text = {}
        for name in set(C_DEFINES) | {"voxel.c"}:
            path = os.path.join(src, name)
            if not os.path.isfile(path):
                sys.exit(f"{path}: not found -- point --src at the game's src/")
            with open(path) as f:
                text[name] = f.read()

        for name, wanted in C_DEFINES.items():
            for key in wanted:
                m = re.search(rf"^#define\s+{key}\s+\(?(-?\d+)\)?\s*(?://|$)",
                              text[name], re.M)
                if not m:
                    sys.exit(f"{name}: cannot find #define {key}. If it moved "
                             f"or became an expression, teach C_DEFINES about it "
                             f"rather than copying the number here.")
                setattr(self, key, int(m.group(1)))

        # Two that are expressions rather than literals, and the one table the
        # camera's own motion comes out of. The sine table is parsed for the
        # same reason as the rest: a preview that turned at a slightly
        # different rate would be a preview of a different game.
        self.TILT_LEVEL = self.FB_HEIGHT * 2 // 5
        self.PX_PER_TAN = (self.FB_WIDTH // 2 * 256 + self.TAN_HALF_FOV // 2) \
            // self.TAN_HALF_FOV
        self.speed_limit = self._array(text["main.c"], "speed_limit")
        self.sin_quarter = np.array(self._array(text["voxel.c"], "sin_quarter"),
                                    np.int32)
        if len(self.sin_quarter) != 65:
            sys.exit("src/voxel.c: sin_quarter is not 65 entries")

        self.NSTEPS = self.BANDS * self.BAND_STEPS

    @staticmethod
    def _array(text, name):
        m = re.search(rf"{name}\s*\[[^\]]*\]\s*=\s*\{{(.*?)\}}", text, re.S)
        if not m:
            sys.exit(f"cannot find the {name} table in the C source")
        return [int(v) for v in re.findall(r"-?\d+", m.group(1))]


# --- fixed point, the way the 45GS02 does it ----------------------------

def mul_shift8(a, b):
    """(a * b) >> 8, signed, arithmetic shift -- src/voxel.c mul_shift8."""
    return np.right_shift(np.asarray(a, np.int32) * np.asarray(b, np.int32), 8)


def to_int16(v):
    """Wrap to 16 bits the way every intermediate in the C does."""
    return ((np.asarray(v, np.int64) + 0x8000) & 0xFFFF).astype(np.int32) - 0x8000


class Sine:
    """voxel_sin: 256 units to the turn, 8.8 result, from the C's own table."""

    def __init__(self, quarter):
        table = np.empty(256, np.int32)
        for a in range(256):
            q, i = divmod(a, 64)
            v = quarter[i] if q in (0, 2) else quarter[64 - i]
            table[a] = -v if q >= 2 else v
        self.table = table

    def sin(self, angle):
        return self.table[np.asarray(angle) & 0xFF]

    def cos(self, angle):
        return self.table[(np.asarray(angle) + 64) & 0xFF]


# --- the map ------------------------------------------------------------

class Maps:
    """The two maps as the disk will carry them, plus the palette as RGB.

    Loaded through `tools/convmap.py`'s own readers, so the preview samples
    exactly what the converter would put on the disk -- the same box-averaged
    heightmap, the same point-sampled colour indices, the same sky gradient
    baked into the same palette entries. A separate reader here would be a
    second opinion about what the disk contains, which is the one thing this
    tool must not have.
    """

    def __init__(self, hgt_png, col_png, hgt_size, col_size):
        self.heights = convmap.load_heightmap(hgt_png, hgt_size)
        indices, rgb = convmap.load_colourmap(col_png, col_size)
        self.colours = indices
        used = set(np.unique(indices).tolist())
        convmap.add_sky(rgb, used)
        rgb[0] = (0, 0, 0)
        # The two entries convmap.py reserves for drawing over the 3D view.
        # Nothing in the game uses them yet; the item markers here are exactly
        # what they are for, so they are given the colours the converter would.
        rgb[convmap.HUD_INK] = (255, 255, 255)
        rgb[convmap.HUD_PAPER] = (0, 0, 0)
        self.rgb = np.array(rgb, np.uint8)

        # A cell is addressed by the high byte of the 8.8 position; the top
        # bits of the fraction pick the sub-cell. Indexing the full map with
        # (pos * axis) >> 8 is the same sample the plane lookup makes on the
        # MEGA65, without needing the planes.
        self.hgt_axis = hgt_size // convmap.CELLS
        self.col_axis = col_size // convmap.CELLS

    def ground(self, x, y):
        """voxel_ground: the height under an 8.8 position."""
        return int(self.heights[(int(y) * self.hgt_axis >> 8) & (self.heights.shape[0] - 1),
                                (int(x) * self.hgt_axis >> 8) & (self.heights.shape[1] - 1)])

    def overview(self, px):
        """The colourmap as an RGB thumbnail, for finding your way about."""
        n = max(1, self.colours.shape[0] // px)
        small = self.colours[n // 2::n, n // 2::n][:px, :px]
        return Image.fromarray(self.rgb[small], "RGB")


# --- the march ----------------------------------------------------------

class March:
    """src/voxel.c and src/voxel_asm.s, one column at a time turned sideways.

    The hardware walks a column at a time and keeps a y buffer per column; this
    walks a *step* at a time across all 160 columns at once, which is the same
    front-to-back order with the loops exchanged. Everything inside it -- the
    band schedule, the position update in 8.8 with its 16-bit wrap, the
    inv_z table, the biased horizon, the span that fills up to the y buffer --
    is the assembly's arithmetic, done on arrays.
    """

    def __init__(self, c, maps, sine):
        self.c, self.maps, self.sine = c, maps, sine

        x = np.arange(VX_COLS, dtype=np.int32)
        # C division truncates toward zero, and this one is negative for the
        # left half of the screen.
        self.tan_tab = ((x - VX_COLS // 2) * c.TAN_HALF_FOV /
                        (VX_COLS // 2)).astype(np.int32)

        inv_z, z, step = [], c.Z_NEAR, c.Z_STEP0
        for _ in range(c.BANDS):
            for _ in range(c.BAND_STEPS):
                inv_z.append((c.SCALE_H << 16) // z)
                z += step
            step <<= 1
        self.inv_z = np.array(inv_z, np.int32)
        # Which band each step belongs to, so the whole march is one cumulative
        # sum rather than four nested loops.
        self.band = np.arange(c.NSTEPS) // c.BAND_STEPS

        self.sky = (c.SKY_BASE + (np.arange(c.FB_HEIGHT) * c.SKY_SHADES)
                    // c.FB_HEIGHT).astype(np.uint8)
        self.rows = np.arange(c.FB_HEIGHT, dtype=np.int32)[:, None]

    def sample(self, cam):
        """Every sample of the frame: rows are steps, columns are rays.

        Returns the projected row of each sample, the colour it read, and the
        depth-ordered positions, which the marker code borrows.
        """
        c = self.c
        cs, sn = int(self.sine.cos(cam.angle)), int(self.sine.sin(cam.angle))
        dirx = cs - mul_shift8(sn, self.tan_tab)
        diry = sn + mul_shift8(cs, self.tan_tab)
        # >> 1, arithmetic: the first step is half a cell, which is Z_STEP0.
        stepx, stepy = to_int16(dirx) >> 1, to_int16(diry) >> 1

        # The step doubles at each band boundary, and the position is 8.8 in a
        # uint16 -- so it wraps the map for free, exactly as the hardware's
        # does. The first sample is one step out, not zero.
        incx = to_int16(stepx[None, :] << self.band[:, None])
        incy = to_int16(stepy[None, :] << self.band[:, None])
        px = (int(cam.x) + np.cumsum(incx, axis=0)) & 0xFFFF
        py = (int(cam.y) + np.cumsum(incy, axis=0)) & 0xFFFF

        h = self.maps.heights[(py * self.maps.hgt_axis) >> 8,
                              (px * self.maps.hgt_axis) >> 8].astype(np.int32)
        colour = self.maps.colours[(py * self.maps.col_axis) >> 8,
                                   (px * self.maps.col_axis) >> 8]

        # ys = horizon - inv_z + ((camh + CAM_BIAS - height) * inv_z >> 8).
        # The bias keeps the multiplier's input positive on the hardware and
        # comes back out of the horizon exactly; it is kept here so the
        # rounding is the machine's rounding.
        inv = self.inv_z[:, None]
        dh = int(cam.height) + c.CAM_BIAS - h
        ys = to_int16(cam.horizon - inv + np.right_shift(dh * inv, 8))
        return ys, colour, px, py

    def render(self, cam):
        """One frame of 3D view, as palette indices, 320 wide."""
        c = self.c
        ys, colour, _, _ = self.sample(cam)

        # A sample above the top of the screen is clamped to row 0; one at 256
        # or more is below anything that can be drawn. Everything between is
        # tested against the y buffer, which is what hides it behind nearer
        # terrain.
        ys = np.where(ys < 0, 0, np.where(ys > 255, c.FB_HEIGHT, ys))

        view = np.repeat(self.sky[:, None], VX_COLS, axis=1)
        ybuf = np.full(VX_COLS, c.FB_HEIGHT, np.int32)
        for k in range(c.NSTEPS):
            top = ys[k]
            span = (self.rows >= top[None, :]) & (self.rows < ybuf[None, :])
            view = np.where(span, colour[k][None, :], view)
            ybuf = np.minimum(ybuf, top)
        self.ybuf = ybuf
        self.ys = ys
        return np.repeat(view, c.FB_WIDTH // VX_COLS, axis=1)

    def clip_at(self, depth):
        """The y buffer as it stood at `depth`: what stands in front of it.

        The march keeps this per column anyway -- the hardware snapshots it at
        the first span at or past the billboard's depth. Here the whole ys
        table is still in hand, so it is a running minimum over the steps in
        front, which is the same number.
        """
        step = self.step_at(depth)
        if step is None:
            return self.ybuf
        return np.minimum(self.ys[:step].min(axis=0), self.c.FB_HEIGHT) \
            if step else np.full(VX_COLS, self.c.FB_HEIGHT, np.int32)

    def step_at(self, depth):
        """voxel_depth_step: the first march step at or beyond `depth`."""
        c = self.c
        zk, step = c.Z_NEAR, c.Z_STEP0
        k = 0
        for _ in range(c.BANDS):
            for _ in range(c.BAND_STEPS):
                if zk >= depth:
                    return k
                zk += step
                k += 1
            step <<= 1
        return None


# --- the flight ---------------------------------------------------------

class Camera:
    __slots__ = ("x", "y", "angle", "height", "horizon")

    def __init__(self, x, y, angle, height, horizon):
        self.x, self.y, self.angle = x, y, angle
        self.height, self.horizon = height, horizon


class Flight:
    """src/main.c's `fly`, minus the wind, the battery and the mission.

    The rates are per frame and are the C's own, so at the previewer's 12.5
    frames a second the drone covers the ground it covers on the machine.
    """

    def __init__(self, c, maps, sine):
        self.c, self.maps, self.sine = c, maps, sine
        self.speed_mode = 1
        self.cam = Camera(128 << 8, 128 << 8, 0, 0, c.TILT_LEVEL)
        self.cam.height = maps.ground(self.cam.x, self.cam.y) + 60

    def step(self, held):
        c, cam = self.c, self.cam
        if "a" in held:
            cam.angle = (cam.angle - c.TURN_RATE) & 0xFF
        if "d" in held:
            cam.angle = (cam.angle + c.TURN_RATE) & 0xFF

        speed = 0
        if "w" in held:
            speed = c.speed_limit[self.speed_mode]
        elif "s" in held:
            speed = -c.speed_limit[self.speed_mode]
        if speed:
            cam.x = (cam.x + (int(self.sine.sin(cam.angle + 64)) * speed >> 8)) & 0xFFFF
            cam.y = (cam.y + (int(self.sine.sin(cam.angle)) * speed >> 8)) & 0xFFFF

        if "r" in held:
            cam.height += c.CLIMB_RATE
        if "f" in held:
            cam.height -= c.CLIMB_RATE
        if "q" in held:
            cam.horizon += c.TILT_RATE
        if "e" in held:
            cam.horizon -= c.TILT_RATE
        cam.horizon = max(c.TILT_MIN, min(c.TILT_MAX, cam.horizon))

        # The clamp the game applies in every mode but sport, where it is what
        # ends the flight instead. Nothing here crashes.
        floor = self.maps.ground(cam.x, cam.y) + c.GROUND_GAP
        if cam.height < floor:
            cam.height = floor

    def readout(self):
        """The panel's own numbers, worked out the panel's own way."""
        c, cam = self.c, self.cam
        heading = (((cam.angle + 64) & 0xFF) * 45) // 32
        cx, cy = cam.x >> 8, cam.y >> 8
        lat = (c.MAP_LAT_SOUTH + (255 - cy)) / 1000.0
        lon = (c.MAP_LON_WEST + cx) / 1000.0
        return heading, lat, lon, cx, cy


# --- items --------------------------------------------------------------

def draw_markers(view, c, march, maps, cam, sine, items, size):
    """A pin where each item in the YAML stands, clipped by the terrain.

    Not the sprite -- there are no pictures for most of these -- but the same
    projection `src/sprite.c` uses, so what the marker covers is what a figure
    standing there would cover, and the depth clip is the march's own y buffer
    sampled at its depth. That is the whole point of drawing them: to see
    whether a spot is visible from where the pilot will be flying.
    """
    for item in items:
        # Nothing is pinned that genmap.py builds into the terrain. A pyramid
        # is already there to be seen, in the maps themselves; a pin on top of
        # it would only hide the thing it was pointing at, and the question a
        # marker answers -- can this be spotted from the air -- the structure
        # answers for itself.
        if item["type"] in genmap.ITEMS:
            continue
        # YAML coordinates are map pixels at genmap.py's DEFAULT_SIZE, which is
        # a quarter of a cell -- so the 8.8 position is exact. Not the size of
        # the map actually loaded: an item is at a place in the world, and
        # --size is a resolution knob that must not move it.
        axis = genmap.DEFAULT_SIZE // convmap.CELLS
        ix = (item["x"] * 256 // axis) & 0xFFFF
        iy = (item["y"] * 256 // axis) & 0xFFFF

        dx, dy = to_int16(ix - cam.x), to_int16(iy - cam.y)
        if max(abs(int(dx)), abs(int(dy))) > c.SPR_Z_FAR:
            continue
        cs, sn = int(sine.cos(cam.angle)), int(sine.sin(cam.angle))
        z = int(mul_shift8(dx, cs)) + int(mul_shift8(dy, sn))
        if not c.SPR_Z_NEAR <= z <= c.SPR_Z_FAR:
            continue
        u = int(mul_shift8(dy, cs)) - int(mul_shift8(dx, sn))

        centre = c.FB_WIDTH // 2 + int(u * c.PX_PER_TAN / z)
        inv_z = (c.SCALE_H << 16) // z
        high = (c.SPR_WORLD_H * inv_z) >> 8
        if high < 2 or high > c.FB_HEIGHT:
            continue
        ground = maps.ground(ix, iy)
        feet = ((cam.height - ground) * inv_z) >> 8
        top = cam.horizon + feet - high

        clip = march.clip_at(z)
        wide = max(1, high // 3)
        for px in range(centre - wide // 2, centre - wide // 2 + wide):
            if not 0 <= px < c.FB_WIDTH:
                continue
            # The clip is per ray, and a ray owns two pixels.
            if top + high > clip[px * VX_COLS // c.FB_WIDTH]:
                continue
            lo, hi = max(top, 0), min(top + high, c.FB_HEIGHT)
            view[lo:hi, px] = convmap.HUD_INK
        # A foot mark, so a pin whose whole body is behind a ridge still shows
        # where it stands when it is not.
        for px in range(centre - wide, centre + wide + 1):
            row = top + high
            if 0 <= px < c.FB_WIDTH and 0 <= row < c.FB_HEIGHT:
                if row <= clip[px * VX_COLS // c.FB_WIDTH]:
                    view[row, px] = convmap.HUD_PAPER


# --- the map file --------------------------------------------------------

def read_map(path):
    with open(path) as f:
        doc = yaml.safe_load(f)
    if not isinstance(doc, dict) or "general" not in doc:
        sys.exit(f"{path}: no `general:` block")
    return doc["general"], doc.get("items") or []


def map_files(path, spec):
    folder = os.path.dirname(os.path.abspath(path))
    hgt = os.path.join(folder, f"hmap{spec['id']:02d}.png")
    col = os.path.join(folder, f"cmap{spec['id']:02d}.png")
    for f in (hgt, col):
        if not os.path.isfile(f):
            sys.exit(f"{f}: not there. Run tools/genmap.py {path} first.")
    return hgt, col


# --- the window ---------------------------------------------------------

HELP = ("W/S fly  A/D yaw  R/F climb  Q/E gimbal  1/2/3 speed  "
        "M mark  L reload  ESC quit")


class Preview:
    def __init__(self, args):
        self.args = args
        self.c = Constants(args.src)
        self.spec, self.items = read_map(args.mapfile)
        self.load_maps()

        self.sine = Sine(self.c.sin_quarter)
        self.march = March(self.c, self.maps, self.sine)
        self.flight = Flight(self.c, self.maps, self.sine)
        self.marks = []
        self.held = set()
        self.pending = {}
        self.frame_ms = max(1, int(round(1000.0 / args.fps)))
        self.last = time.monotonic()
        self.fps = args.fps

    def load_maps(self):
        hgt, col = map_files(self.args.mapfile, self.spec)
        self.maps = Maps(hgt, col, self.args.hgt_size, self.args.col_size)
        self.size = Image.open(col).size[0]

    # --- drawing

    def frame(self):
        cam = self.flight.cam
        view = self.march.render(cam)
        if self.items:
            draw_markers(view, self.c, self.march, self.maps, cam, self.sine,
                         self.items, self.size)
        return Image.fromarray(self.maps.rgb[view], "RGB")

    def compose(self):
        """The view, and the whole map beside it with a crosshair on you."""
        c, s = self.c, self.args.scale
        view = self.frame()
        if not self.args.overview:
            return view.resize((c.FB_WIDTH * s, c.FB_HEIGHT * s), Image.NEAREST)

        px = c.FB_HEIGHT
        page = Image.new("RGB", (c.FB_WIDTH + 8 + px, c.FB_HEIGHT), (0, 0, 0))
        page.paste(view, (0, 0))
        page.paste(self.thumb.copy(), (c.FB_WIDTH + 8, 0))
        self.crosshair(page, c.FB_WIDTH + 8, px)
        return page.resize((page.width * s, page.height * s), Image.NEAREST)

    def crosshair(self, page, x0, px):
        """Where you are on the thumbnail: a white cross inside a black one.

        Plain white disappears against snow, which is exactly the terrain you
        are most likely to be lost in.
        """
        cam = self.flight.cam
        cx = x0 + (cam.x >> 8) * px // 256
        cy = (cam.y >> 8) * px // 256
        def dot(x, y, colour):
            if x0 <= x < x0 + px and 0 <= y < px:
                page.putpixel((x, y), colour)

        for d in range(-5, 6):                    # the black halo, three thick
            for o in (-1, 0, 1):
                dot(cx + d, cy + o, (0, 0, 0))
                dot(cx + o, cy + d, (0, 0, 0))
        for d in range(-4, 5):
            dot(cx + d, cy, (255, 255, 255))
            dot(cx, cy + d, (255, 255, 255))

    # --- keys

    def press(self, event):
        key = event.keysym.lower()
        tid = self.pending.pop(key, None)
        if tid is not None:
            self.root.after_cancel(tid)
        self.held.add(key)

        if key in ("1", "2", "3"):
            self.flight.speed_mode = int(key) - 1
        elif key == "m":
            self.mark()
        elif key == "l":
            self.reload()
        elif key == "escape":
            self.root.destroy()

    def release(self, event):
        # X11 repeats a held key as release/press pairs at the same
        # millisecond, so a release only counts if no press follows it.
        key = event.keysym.lower()
        self.pending[key] = self.root.after(
            40, lambda: (self.held.discard(key), self.pending.pop(key, None)))

    # --- marks

    def mark(self):
        cam = self.flight.cam
        _, lat, lon, _, _ = self.flight.readout()
        # At genmap.py's DEFAULT_SIZE, not the size of the map loaded: a mark
        # is a place in the world, and it has to mean the same thing whatever
        # resolution this happens to be flying.
        axis = genmap.DEFAULT_SIZE // convmap.CELLS
        x, y = (cam.x * axis) >> 8, (cam.y * axis) >> 8
        entry = {"x": int(x), "y": int(y), "lat": round(lat, 3),
                 "lon": round(lon, 3), "alt": int(cam.height)}
        self.marks.append(entry)

        block = (f"  - type: CHANGEME\n    size: medium\n"
                 f"    x: {entry['x']}\n    y: {entry['y']}"
                 f"      # {entry['lat']:.3f}N {entry['lon']:.3f}E, "
                 f"alt {entry['alt']}\n")
        print(block, end="", flush=True)
        path = os.path.splitext(self.args.mapfile)[0] + MARK_SUFFIX
        with open(path, "a") as f:
            f.write(block)
        self.note = f"marked {entry['x']},{entry['y']} -> {os.path.basename(path)}"

    def reload(self):
        """Pick up a rerun of genmap.py without losing where you are."""
        self.spec, self.items = read_map(self.args.mapfile)
        self.load_maps()
        self.march = March(self.c, self.maps, self.sine)
        self.thumb = self.maps.overview(self.c.FB_HEIGHT)
        self.note = "reloaded"

    # --- the loop

    def tick(self):
        started = time.monotonic()
        self.flight.step(self.held)
        image = self.compose()
        self.photo = ImageTk.PhotoImage(image)
        self.canvas.itemconfigure(self.item, image=self.photo)

        self.fps = 0.8 * self.fps + 0.2 / max(started - self.last, 1e-6)
        self.last = started
        self.status()
        # The frame's own cost comes off the wait, or a preview that takes 15ms
        # to draw runs at 10.5 frames a second rather than the machine's 12.5 --
        # and every rate in the flight model is per frame, so that is a slower
        # drone, not just a slower picture.
        spent = int((time.monotonic() - started) * 1000)
        self.root.after(max(1, self.frame_ms - spent), self.tick)

    def status(self):
        cam = self.flight.cam
        heading, lat, lon, _, _ = self.flight.readout()
        names = ("SLO", "NRM", "SPT")
        axis = genmap.DEFAULT_SIZE // convmap.CELLS   # what a mark is in
        self.line1.set(
            f"ALT {cam.height:4d}   HDG {heading:3d}   "
            f"LAT {lat:.3f}N  LON {lon:.3f}E   SPD {names[self.flight.speed_mode]}")
        self.line2.set(
            f"map x {(cam.x * axis) >> 8:4d}  y {(cam.y * axis) >> 8:4d}   "
            f"gimbal {cam.horizon - self.c.TILT_LEVEL:+4d}   "
            f"{self.fps:4.1f} fps   {self.note}")

    def run(self):
        import tkinter as tk

        self.root = tk.Tk()
        self.root.title(f"{os.path.basename(self.args.mapfile)} — "
                        f"map {self.spec['id']:02d}, {self.spec['type']}")
        self.root.configure(bg="black")
        self.thumb = self.maps.overview(self.c.FB_HEIGHT)
        self.note = ""

        width = self.c.FB_WIDTH + (8 + self.c.FB_HEIGHT if self.args.overview else 0)
        self.canvas = tk.Canvas(self.root, width=width * self.args.scale,
                                height=self.c.FB_HEIGHT * self.args.scale,
                                highlightthickness=0, bg="black")
        self.canvas.pack()
        self.item = self.canvas.create_image(0, 0, anchor="nw")

        self.line1, self.line2 = tk.StringVar(), tk.StringVar()
        for var in (self.line1, self.line2):
            tk.Label(self.root, textvariable=var, font=("monospace", 11),
                     fg="#dddddd", bg="black", anchor="w").pack(fill="x")
        tk.Label(self.root, text=HELP, font=("monospace", 9),
                 fg="#8899aa", bg="black", anchor="w").pack(fill="x")

        self.root.bind("<KeyPress>", self.press)
        self.root.bind("<KeyRelease>", self.release)
        self.root.after(self.frame_ms, self.tick)
        self.root.mainloop()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("mapfile", metavar="map.yaml", nargs="?",
                    default="maps/island.yaml")
    ap.add_argument("--scale", type=int, default=2, help="window pixels per pixel")
    ap.add_argument("--fps", type=float, default=DEFAULT_FPS,
                    help="frame rate; the default is the MEGA65's, and every "
                         "rate in the flight model is per frame")
    ap.add_argument("--hgt-size", type=int, default=DEFAULT_HGT_SIZE)
    ap.add_argument("--col-size", type=int, default=DEFAULT_COL_SIZE)
    ap.add_argument("--src", default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src"),
        help="the game's src/, which the renderer's constants are read from")
    ap.add_argument("--no-overview", dest="overview", action="store_false",
                    help="just the 3D view, without the map beside it")
    ap.add_argument("--shot", metavar="FILE",
                    help="render one frame to a PNG and exit, for a check "
                         "against a screenshot from the machine")
    ap.add_argument("--at", metavar="X,Y,ANGLE,ALT",
                    help="where to put the camera for --shot; map cells and "
                         "height units, e.g. 128,128,0,79")
    args = ap.parse_args()

    preview = Preview(args)
    if args.shot:
        cam = preview.flight.cam
        if args.at:
            x, y, angle, alt = (int(v) for v in args.at.split(","))
            cam.x, cam.y, cam.angle, cam.height = x << 8, y << 8, angle, alt
        preview.thumb = preview.maps.overview(preview.c.FB_HEIGHT)
        preview.note = ""
        preview.compose().save(args.shot)
        print(f"{args.shot}: map {preview.spec['id']:02d} from "
              f"{cam.x >> 8},{cam.y >> 8} at {cam.height}, heading {cam.angle}")
        return
    preview.run()


if __name__ == "__main__":
    main()
