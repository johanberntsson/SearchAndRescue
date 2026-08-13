TARGET   = --target=mega65
# --no-cross-call and --strong-inline were both tried here and both measured
# slower (615ms a frame against 533ms), so the defaults stay.
# PROFILE=0 compiles out the per-column instrumentation; the FPS counter stays.
PROFILE ?= 1
# WIDE=1 marched all 320 pixels instead of doubling 160. RETIRED: the
# renderer still has the code, but the plane lookup tables now live in the low
# free RAM at $1600 and only fit there when the per-ray tables are half size,
# so src/loader.h #errors on it. See LOW_FREE there.
WIDE    ?= 0
# FLYNOW=n skips the title and menu screens and launches straight into mission
# n: 1 for the first, 2 for the second. The headless profiling run cannot press
# a key, so without it a -dumpmem image has no frames in it at all -- and
# FLYNOW=2 is the only way such a run reaches the second mission's *map*.
# Never for timing comparisons of anything but itself -- it is the same code,
# just entered differently.
FLYNOW  ?= 0
# REPORT=n holds the startup benchmark report on screen for n seconds instead
# of 20. For a session at the real MEGA65, where that report is the only way to
# read the attic RAM figures: `make REPORT=120`.
REPORT  ?= 20
# HOLD=n is the same for *stage one's* report, and it is a separate knob
# because it is a separate pause in a separate program -- and because it is on
# the critical path of every boot, where the game's is not. Four seconds is
# long enough to see that the checksum is right and short enough not to be
# mistaken for generator time, which is exactly what happened when it was
# eight: `make HOLD=60` to actually read it at the machine.
HOLD    ?= 4
# Map resolutions, powers of two from 256 up to the source PNGs' 1024. Above
# 256 the heightmap leaves chip RAM and the inner loop pays for it; the
# colourmap is read once per span and is nearly free at any size. Both maps
# are exomizer-crunched, which is what makes these fit a d81.
HGT_SIZE ?= 512
COL_SIZE ?= 1024
SIZEFLAGS = -DWIDE=$(WIDE) -DHGT_SIZE=$(HGT_SIZE) -DCOL_SIZE=$(COL_SIZE) \
            -DFLYNOW=$(FLYNOW) -DREPORT_SECONDS=$(REPORT) -DSTAGE1_HOLD=$(HOLD)
CFLAGS   = $(TARGET) -O2 --speed -DPROFILE_DETAIL=$(PROFILE) $(SIZEFLAGS)
ASFLAGS  = $(TARGET) -DPROFILE_DETAIL=$(PROFILE) $(SIZEFLAGS)
LDFLAGS  = $(TARGET) --output-format=prg
LINKFILE = mega65-plain.scm

BUILD    = build
SRCS     = $(wildcard src/*.c)
ASRCS    = $(wildcard src/*.s)
OBJS     = $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS)) \
           $(patsubst src/%.s,$(BUILD)/%.o,$(ASRCS))

ELF      = $(BUILD)/sar.elf
# The game is no longer what the ROM boots. **Stage one is**, and it chains to
# this by name -- see GAME_NAME in src/mapgen/mapgen.c, which has to agree with
# the basename here, since diskutil.rb names a file on disk after its host
# file. `SAR` on disk.
PRG      = $(BUILD)/sar
D81      = $(BUILD)/sar.d81

# Stage one: the program that prepares attic RAM and then hands the machine to
# the game. It is a separate program because the game already fills the 32 KB
# at $2001, and because attic RAM survives a program load, which is the whole
# mechanism. src/mapgen/ is deliberately outside the src/*.c glob above so the
# two never link into each other. See documentation/on-device-maps.md.
GEN_SRCS = $(wildcard src/mapgen/*.c)
GEN_ASRCS = $(wildcard src/mapgen/*.s)
# src/profile.c is shared with the game rather than copied: stage one times
# itself with the same raster-calibrated clock the renderer is measured with,
# so the two sets of figures mean the same thing.
GEN_OBJS = $(patsubst src/mapgen/%.c,$(BUILD)/mapgen/%.o,$(GEN_SRCS)) \
           $(patsubst src/mapgen/%.s,$(BUILD)/mapgen/%.o,$(GEN_ASRCS)) \
           $(BUILD)/profile.o
GEN_ELF  = $(BUILD)/mapgen.elf
GEN_PRG  = $(BUILD)/autoboot.c65
# One sprite sheet per figure the game can stand in the world, in the order
# src/sprite.c numbers them: 0 the lost hiker, 1 the pair by the lake. They are
# converted together because they share the one on-screen palette.
SPRITES  = resources/survivor-sprite.png resources/medicalemergency-sprite.png

# The maps on the disk, in slot order: mission 1 flies slot 0 and mission 2
# slot 1 (src/mission.c chooses). They are *generated* from these YAMLs rather
# than drawn, which is what makes several of them fit -- two come to about 500
# KB crunched against 661 KB for the one hand-drawn pair. Keep MAP_COUNT in
# src/loader.h in step with this list.
MAP_YAMLS = maps/island.yaml maps/plains.yaml   # map files, not missions
MAP_IDS   = 03 05
MAP_NUMS  = 0 1   # slot numbers, one per map above

# Generated maps are build products and are not in git; genmap.py names them
# from each map's `id`.
GEN_MAPS = $(foreach id,$(MAP_IDS),maps/hmap$(id).png maps/cmap$(id).png)
MAP_RES  = $(foreach n,$(MAP_NUMS),$(BUILD)/map$(n).hgt $(BUILD)/map$(n).col \
                                    $(BUILD)/map$(n).pal $(BUILD)/map$(n).ovr)
RES      = $(MAP_RES) $(BUILD)/terrain.spr $(BUILD)/terrain.sp2

all: $(D81)

run: $(D81)
	xemu-xmega65 -besure -8 $(D81)

# Quick turnaround for tests that do not need the resource files. This runs
# the *game* on its own, with no stage one in front of it -- which is also the
# case the handover check is meant to report as absent rather than hang on.
prg: $(PRG)
	xemu-xmega65 -besure -prg $(PRG)

$(BUILD):
	mkdir -p $(BUILD)

# Both compiler and assembler see these, so changing either has to force a
# rebuild. Without it, `make PROFILE=0` and then `make` leaves every object
# built against the wrong flag and the counters silently stay off -- and a
# half-rebuilt WIDE change is a memory map that disagrees with itself.
CONFIG_STAMP = $(BUILD)/config-$(PROFILE)-$(WIDE)-$(HGT_SIZE)-$(COL_SIZE)-$(FLYNOW)-$(REPORT)-$(HOLD).stamp

$(CONFIG_STAMP): | $(BUILD)
	rm -f $(BUILD)/config-*.stamp
	touch $@

# Every object depends on every header. Crude, but the alternative is stale
# objects built against a changed memory layout, which fails in ways that look
# like hardware bugs.
$(BUILD)/%.o: src/%.c $(wildcard src/*.h) $(CONFIG_STAMP) | $(BUILD)
	cc6502 $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: src/%.s $(wildcard src/*.h) $(CONFIG_STAMP) | $(BUILD)
	as6502 $(ASFLAGS) -o $@ $<

$(ELF): $(OBJS)
	ln6502 $(LDFLAGS) -o $@ $(LINKFILE) $(OBJS)

# -o names the ELF; ln6502 writes the PRG beside it under the same stem.
# diskutil.rb names the file on disk after the host file, so both programs are
# copied to the basename they should carry there.
$(PRG): $(ELF)
	cp $(BUILD)/sar.prg $@

$(BUILD)/mapgen:
	mkdir -p $@

$(BUILD)/mapgen/%.o: src/mapgen/%.c $(wildcard src/*.h) $(wildcard src/mapgen/*.h) \
                     $(CONFIG_STAMP) | $(BUILD)/mapgen
	cc6502 $(CFLAGS) -c -o $@ $<

$(BUILD)/mapgen/%.o: src/mapgen/%.s $(wildcard src/*.h) $(wildcard src/mapgen/*.h) \
                     $(CONFIG_STAMP) | $(BUILD)/mapgen
	as6502 $(ASFLAGS) -o $@ $<

$(GEN_ELF): $(GEN_OBJS)
	ln6502 $(LDFLAGS) -o $@ $(LINKFILE) $(GEN_OBJS)

# The MEGA65 ROM only autoboots a file called autoboot.c65, and that is stage
# one now.
$(GEN_PRG): $(GEN_ELF)
	cp $(BUILD)/mapgen.prg $@

comma := ,
empty :=
space := $(empty) $(empty)

# The maps, from their map files. One genmap run each; the id in the filename
# is the map's own, so these are spelled out rather than pattern-matched.
maps/hmap03.png maps/cmap03.png &: maps/island.yaml maps/palette.yaml \
                                   tools/genmap.py tools/fixed.py
	python3 tools/genmap.py maps/island.yaml

maps/hmap05.png maps/cmap05.png &: maps/plains.yaml maps/palette.yaml \
                                   tools/genmap.py tools/fixed.py
	python3 tools/genmap.py maps/plains.yaml

# **--shared is what lets more than one map share a disk.** Without it each
# map hands the sprites whatever palette entries its own colours left free, so
# a figure changes colour with the mission; with it every map reserves the
# whole shared ramp and the sprite files come out byte-identical, so one set
# serves them all. See the header of tools/convmap.py.
#
# The sprites are taken from slot 0's conversion for that reason. The rest of
# slot 1's output is used and its sprite files are simply the same bytes.
$(RES) &: $(GEN_MAPS) $(SPRITES) tools/convmap.py maps/palette.yaml \
          $(CONFIG_STAMP) | $(BUILD)
	python3 tools/convmap.py maps/hmap03.png maps/cmap03.png \
	    $(subst $(space),$(comma),$(SPRITES)) \
	    $(BUILD)/map0 $(HGT_SIZE) $(COL_SIZE) --shared maps/palette.yaml
	python3 tools/convmap.py maps/hmap05.png maps/cmap05.png \
	    $(subst $(space),$(comma),$(SPRITES)) \
	    $(BUILD)/map1 $(HGT_SIZE) $(COL_SIZE) --shared maps/palette.yaml
	cp $(BUILD)/map0.spr $(BUILD)/terrain.spr
	cp $(BUILD)/map0.sp2 $(BUILD)/terrain.sp2

# tools/diskutil.rb refuses to overwrite a file that already exists on the image,
# so the image is always built from scratch.
$(D81): $(GEN_PRG) $(PRG) $(RES)
	rm -f $@
	ruby tools/diskutil.rb $@ -name "search and rescue" -id sr \
	    -writeprg -copyf1 $(GEN_PRG) $(PRG) \
	    -writeseq -copyf1 $(RES)

# The disk to hand out. PROFILE=0 drops the per-column instrumentation, which
# is about 7% of a frame; FLYNOW=0 puts the menus back. Both are spelled out
# rather than left to the defaults above, so that a release is the same disk
# however this was last invoked -- but the map sizes
# are not, because those defaults *are* the shipping resolution and pinning
# them here would be two places to change it.
#
# A sub-make, because these flags belong to the config stamp: the release
# build and an interactive one share $(BUILD) and each forces a rebuild of the
# other. That is the stamp doing its job, not waste to work around.
RELEASE = release/sar-latest.d81

release:
	$(MAKE) PROFILE=0 FLYNOW=0 $(D81)
	mkdir -p $(dir $(RELEASE))
	cp $(D81) $(RELEASE)
	@echo "release: $(RELEASE)"

clean:
	rm -rf $(BUILD)

.PHONY: all run prg release clean
