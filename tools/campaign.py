#!/usr/bin/env python3
"""Turn campaign.yaml and the mission files it names into the disk's campaign.

    python3 tools/campaign.py campaign.yaml build/campaign.bin build/campaign.mk

Two outputs, because a campaign decides two different things:

  * **campaign.bin** goes on the D81 and is read at boot, before anything else,
    because it says how many maps and figures there are to read after it. It is
    a small header, one fixed record per mission, and a pool of strings the
    records point into by offset. src/mission.c turns those offsets into
    pointers once and the rest of the game never learns it came off a disk.

  * **campaign.mk** is included by the Makefile. The maps to generate, the
    sprite sheets to convert, the slot numbers and the name on the disk all
    come out of here, so they stop being three hand-kept lists that could
    disagree with each other and with src/.

**The maps are collected rather than listed.** Each mission names the world it
is flown over; two missions over the same island get one map slot. Same for
figures: a sheet used twice is converted once.

Everything is checked here rather than on the machine, which has no way to
report anything and no room for the code to do it: text is uppercased and
wrapped to the briefing's width and refused if it will not fit, a fix is
refused if it is off the map, and the whole blob is refused if it will not fit
the buffer the game reserves for it.
"""

import os
import sys

import yaml

# ---------------------------------------------------------------------------
# What the game can hold. Each of these is spelled once more in the C -- the
# names in brackets -- and a build that disagrees is caught here rather than by
# overrunning something on the machine.
MISSION_MAX = 8       # MISSION_MAX in src/mission.h
MAP_SLOTS = 3         # MAP_SLOTS in src/loader.h: 2 MB of attic RAM each
SPRITE_MAX = 3        # SPRITE_MAX in src/sprite.h: bank 1 between the
                      # billboards and the overview maps
CAMPAIGN_BYTES = 1024  # CAMPAIGN_BYTES in src/mission.h: the near buffer

# The panel's background artwork. One picture for the whole game rather than
# anything a mission chooses, so it is a constant here and not a campaign
# field; it is named in the generated make because that is where the convmap
# runs are written.
PANEL_ART = "resources/panel.png"

# The briefing draws a brief line at column 2 of a 40-column display and the
# page has three rows for it. Keep BRIEF_WIDTH and BRIEF_LINES in step with
# screens_briefing.
BRIEF_WIDTH = 36
BRIEF_LINES = 3

# A map is 256 cells square and a cell is one millidegree, so a fix outside
# this is not on the map at all. MAP_LAT_SOUTH and MAP_LON_WEST in src/panel.h.
MAP_LAT_SOUTH = 46500
MAP_LON_WEST = 8000
MAP_CELLS = 256

WEATHER = {"clear": 0, "rain": 1}   # WEATHER_CLEAR / WEATHER_RAIN, weather.h

# A mission record, by byte offset. Spelled out because the reader in
# src/mission.c has to agree with it exactly, and once did not:
#
#   0 name   2 brief[0..2]   8 objective   10 cargo   12 done   14 lost
#  16 lat   18 lon           20 figure     21 weather 22 map    23 spare
#
# Every sixteen-bit field but the last two is an offset into the string pool,
# and offset 0 means there is none -- nothing can live at 0, which is where
# the magic is.
RECORD_BYTES = 24
HEADER_BYTES = 8
MAGIC = b"SAR\x01"

# The screen has no lower case and no punctuation beyond this. Anything else
# would come out as a random glyph, so it is refused where it can be read
# about rather than drawn where it cannot.
ALLOWED = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,:;'!?-/()")


class Error(Exception):
    pass


def load_yaml(path):
    try:
        with open(path) as f:
            return yaml.safe_load(f) or {}
    except FileNotFoundError:
        raise Error("no such file: %s" % path)


def text(where, value, width=None):
    """One line, as the screen will show it."""
    if value is None:
        return None
    s = " ".join(str(value).split()).upper()
    bad = sorted(set(s) - ALLOWED)
    if bad:
        raise Error("%s: %s cannot be drawn" % (where, " ".join(repr(c) for c in bad)))
    if width is not None and len(s) > width:
        raise Error("%s: %d characters, and %d is the most that fits\n  %s"
                    % (where, len(s), width, s))
    return s


def wrap(where, value, width, lines):
    """Prose to the fixed number of short lines the briefing has room for."""
    words = text(where, value).split()
    out = []
    for word in words:
        if len(word) > width:
            raise Error("%s: %r is longer than the %d columns there are"
                        % (where, word, width))
        if out and len(out[-1]) + 1 + len(word) <= width:
            out[-1] += " " + word
        else:
            out.append(word)
    if len(out) > lines:
        raise Error("%s: wraps to %d lines and the page has %d\n  %s"
                    % (where, len(out), lines, "\n  ".join(out)))
    return out + [""] * (lines - len(out))


def millidegrees(where, value, low, high):
    if value is None:
        raise Error("%s: missing" % where)
    n = int(round(float(value) * 1000))
    if not low <= n < high:
        raise Error("%s: %s is off the map, which runs %.3f to %.3f"
                    % (where, value, low / 1000.0, (high - 1) / 1000.0))
    return n


class Pool:
    """The strings, each written once however many missions use it."""

    def __init__(self, start):
        self.start = start
        self.data = bytearray()
        self.seen = {}

    def add(self, s):
        if s is None:
            return 0                      # nothing can live at offset 0
        if s not in self.seen:
            self.seen[s] = self.start + len(self.data)
            self.data += s.encode("ascii") + b"\0"
        return self.seen[s]


def read_mission(path, maps, figures):
    doc = load_yaml(path)
    who = os.path.basename(path)

    def need(key):
        if key not in doc or doc[key] is None:
            raise Error("%s: no `%s`" % (who, key))
        return doc[key]

    m = {}
    m["name"] = text("%s name" % who, need("name"), 24)
    m["brief"] = wrap("%s brief" % who, need("brief"), BRIEF_WIDTH, BRIEF_LINES)
    m["objective"] = text("%s objective" % who, need("objective"), 34)
    # The one field the rest hangs off. Absent means an empty bay and a camera
    # mission; present means a delivery, and `lost` is then the debrief's line
    # for putting it down in the wrong place.
    m["cargo"] = text("%s cargo" % who, doc.get("cargo"), 12)
    m["done"] = text("%s done" % who, need("done"), 38)
    m["lost"] = text("%s lost" % who, doc.get("lost"), 38)
    if m["cargo"] and not m["lost"]:
        raise Error("%s: a mission with cargo needs `lost`, for the debrief "
                    "when it goes down in the wrong place" % who)
    if m["lost"] and not m["cargo"]:
        raise Error("%s: `lost` with no `cargo` -- there is nothing to lose"
                    % who)

    fix = need("fix")
    m["lat"] = millidegrees("%s fix lat" % who, fix.get("lat"),
                            MAP_LAT_SOUTH, MAP_LAT_SOUTH + MAP_CELLS)
    m["lon"] = millidegrees("%s fix lon" % who, fix.get("lon"),
                            MAP_LON_WEST, MAP_LON_WEST + MAP_CELLS)

    weather = str(need("weather")).lower()
    if weather not in WEATHER:
        raise Error("%s: weather `%s` is not one of %s"
                    % (who, weather, ", ".join(sorted(WEATHER))))
    m["weather"] = WEATHER[weather]

    # Collected in the order they are first used, which is the order the disk
    # carries them in and the slot number the game flies.
    m["map"] = slot(maps, os.path.normpath(need("map")), MAP_SLOTS,
                    "%s: more than %d maps" % (who, MAP_SLOTS))
    m["figure"] = slot(figures, os.path.normpath(need("figure")), SPRITE_MAX,
                       "%s: more than %d figures -- bank 1 holds one 1028-byte "
                       "slot each between SPRITE_STORE and OVERVIEW_STORE"
                       % (who, SPRITE_MAX))
    return m


def slot(table, path, limit, message):
    if path not in table:
        if len(table) >= limit:
            raise Error(message)
        if not os.path.exists(path):
            raise Error("no such file: %s" % path)
        table[path] = len(table)
    return table[path]


def build_bin(missions):
    pool = Pool(HEADER_BYTES + len(missions) * RECORD_BYTES)
    records = bytearray()

    for m in missions:
        offs = [pool.add(m["name"])]
        offs += [pool.add(line) for line in m["brief"]]
        offs += [pool.add(m["objective"]), pool.add(m["cargo"]),
                 pool.add(m["done"]), pool.add(m["lost"])]
        offs += [m["lat"], m["lon"]]
        for n in offs:
            records += n.to_bytes(2, "little")
        records += bytes([m["figure"], m["weather"], m["map"], 0])

    assert len(records) == len(missions) * RECORD_BYTES, len(records)
    header = MAGIC + bytes([len(missions), 0, 0, 0])   # counts patched below
    blob = bytearray(header) + records + pool.data
    return blob


MK = """\
# Generated by tools/campaign.py from %(campaign)s -- do not edit.
#
# The maps, the sprite sheets and the disk's name all come out of the campaign
# now, so there is one place to change them. `make` regenerates this whenever
# campaign.yaml or anything in missions/ moves.

DISK_NAME = %(disk)s
MISSION_YAMLS = %(missions)s
MAP_YAMLS = %(map_yamls)s
MAP_IDS   = %(map_ids)s
MAP_NUMS  = %(map_nums)s
SPRITES   = %(sprites)s
GEN_MAPS  = %(gen_maps)s
SPR_RES   = %(spr_res)s
# The information panel's background. Not a campaign file at all -- it is the
# game's own furniture, the way the character set is -- but it goes through
# convmap.py with the maps, because its palette entries have to be reserved in
# every map's palette. One file for the whole disk, like the figures.
PANEL_ART = %(panel_art)s
PNL_RES   = $(BUILD)/panel.pnl

"""

MAP_RULE = """\
maps/hmap%(id)s.png maps/cmap%(id)s.png &: %(yaml)s maps/palette.yaml \\
                                   tools/genmap.py tools/fixed.py
\tpython3 tools/genmap.py %(yaml)s

"""

CONV_HEAD = """\
# **--shared is what lets more than one map share a disk.** Without it each map
# hands the sprites whatever palette entries its own colours left free, so a
# figure changes colour with the mission; with it every map reserves the whole
# shared ramp and the sprite files come out byte-identical, so one set serves
# them all. See the header of tools/convmap.py.
#
# The sprites are therefore taken from slot 0's conversion. The rest of every
# other slot's output is used and its sprite files are simply the same bytes.
$(CONV_RES) &: $(GEN_MAPS) $(SPRITES) $(PANEL_ART) tools/convmap.py \\
          maps/palette.yaml $(CONFIG_STAMP) | $(BUILD)
"""

CONV_LINE = """\
\tpython3 tools/convmap.py maps/hmap%(id)s.png maps/cmap%(id)s.png \\
\t    $(subst $(space),$(comma),$(SPRITES)) \\
\t    $(BUILD)/map%(num)d $(HGT_SIZE) $(COL_SIZE) --shared maps/palette.yaml \\
\t    --panel $(PANEL_ART)
"""


def sprite_ext(n):
    """What tools/convmap.py calls figure n, and src/sprite.c looks for."""
    return "spr" if n == 0 else "sp%d" % (n + 1)


def build_mk(campaign, disk, mission_files, maps, figures):
    ids = [str(load_yaml(p)["general"]["id"]).zfill(2) for p in maps]
    out = MK % {
        "campaign": campaign,
        "disk": disk.lower(),
        "missions": " ".join(mission_files),
        "map_yamls": " ".join(maps),
        "map_ids": " ".join(ids),
        "map_nums": " ".join(str(n) for n in range(len(maps))),
        "sprites": " ".join(figures),
        "gen_maps": " ".join("maps/hmap%s.png maps/cmap%s.png" % (i, i)
                             for i in ids),
        "spr_res": " ".join("$(BUILD)/terrain." + sprite_ext(n)
                            for n in range(len(figures))),
        "panel_art": PANEL_ART,
    }
    for path, i in zip(maps, ids):
        out += MAP_RULE % {"id": i, "yaml": path}
    out += CONV_HEAD
    for num, i in enumerate(ids):
        out += CONV_LINE % {"id": i, "num": num}
    # The figures are converted with every map and come out identical; slot
    # zero's are the ones that go on the disk, under the names src/sprite.c
    # asks for.
    for n in range(len(figures)):
        ext = sprite_ext(n)
        out += "\tcp $(BUILD)/map0.%s $(BUILD)/terrain.%s\n" % (ext, ext)
    out += "\tcp $(BUILD)/map0.pnl $(BUILD)/panel.pnl\n"
    return out


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: campaign.py <campaign.yaml> <out.bin> <out.mk>")
    campaign, out_bin, out_mk = sys.argv[1:]

    doc = load_yaml(campaign)
    # Not length-checked: this goes to tools/diskutil.rb as the disk's name,
    # which does whatever it already does with one longer than a CBM directory
    # header holds. It is the same string the Makefile used to spell out.
    disk = text("campaign name", doc.get("name") or "SEARCH AND RESCUE")
    files = doc.get("missions") or []
    if not files:
        raise Error("%s: no missions" % campaign)
    if len(files) > MISSION_MAX:
        raise Error("%s: %d missions, and the game holds %d"
                    % (campaign, len(files), MISSION_MAX))

    maps, figures = {}, {}
    missions = [read_mission(p, maps, figures) for p in files]

    blob = build_bin(missions)
    blob[5] = len(maps)
    blob[6] = len(figures)
    if len(blob) > CAMPAIGN_BYTES:
        raise Error("the campaign is %d bytes and the game reserves %d. Shorten "
                    "the text, or raise CAMPAIGN_BYTES in src/mission.h and "
                    "here together." % (len(blob), CAMPAIGN_BYTES))

    # Padded to the buffer the game reads, and then by another 512, because
    # the Kernal reports EOF on a SEQ file 256 bytes early whatever its size:
    # every resource on this disk is read to a known length and padded past
    # the tail that cannot be reached. See src/loader.c.
    with open(out_bin, "wb") as f:
        f.write(blob + bytes(CAMPAIGN_BYTES - len(blob) + 512))
    with open(out_mk, "w") as f:
        f.write(build_mk(campaign, disk, files,
                         list(maps), list(figures)))

    print("campaign: %d missions, %d maps, %d figures, %d of %d bytes"
          % (len(missions), len(maps), len(figures), len(blob), CAMPAIGN_BYTES))
    for n, m in enumerate(missions):
        print("  %d %-24s map %d  figure %d  %s"
              % (n + 1, m["name"], m["map"], m["figure"],
                 "cargo: " + m["cargo"] if m["cargo"] else "camera"))


if __name__ == "__main__":
    try:
        main()
    except Error as exc:
        sys.exit("campaign: %s" % exc)
