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
# REPORT=n holds the startup benchmark report on screen for n seconds. It is 0
# now, because with the map generator gone the boot is short enough that a
# twenty-second pause was most of it -- the report still prints, it just does
# not wait. **For a session at the real MEGA65 pass a number**, because that
# report is the only way to read the attic RAM figures there: there is no
# -dumpmem on the hardware. `make REPORT=120`. Any key ends the wait early.
REPORT  ?= 0
# The C stack. The toolchain defaults to 4096 and the game comes nowhere near:
# src/bank.s fills it with a canary and main prints how much survived.
# 144 bytes is the measured high-water mark, at the boot rather than in the
# flight. 512 is three and a half times that, and it hands 3.5 KB back to a
# program that had 44 bytes free.
CSTACK_GAME ?= 512
# Map resolutions, powers of two from 256 up to the source PNGs' 1024. Above
# 256 the heightmap leaves chip RAM and the inner loop pays for it; the
# colourmap is read once per span and is nearly free at any size. Both maps
# are exomizer-crunched, which is what makes these fit a d81.
#
# **COL_SIZE is 512 and it used to be 1024**, which cost 316 KB of disk and,
# because the resources are almost the whole boot, 41 seconds of every floppy
# load. Flown side by side the difference is slight -- called on 14 Aug 2026
# after playing both -- so the seconds win. `make COL_SIZE=1024` still builds
# the finer one, and `tools/checkview.py`'s reference screenshot is of
# whichever is the default.
HGT_SIZE ?= 512
COL_SIZE ?= 512
SIZEFLAGS = -DWIDE=$(WIDE) -DHGT_SIZE=$(HGT_SIZE) -DCOL_SIZE=$(COL_SIZE) \
            -DFLYNOW=$(FLYNOW) -DREPORT_SECONDS=$(REPORT)
CFLAGS   = $(TARGET) -O2 --speed -DPROFILE_DETAIL=$(PROFILE) $(SIZEFLAGS)
ASFLAGS  = $(TARGET) -DPROFILE_DETAIL=$(PROFILE) $(SIZEFLAGS)
LDFLAGS  = $(TARGET) --output-format=prg
LINKFILE = mega65-plain.scm

BUILD    = build
SRCS     = $(wildcard src/*.c)
ASRCS    = $(wildcard src/*.s)
OBJS     = $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS)) \
           $(patsubst src/%.s,$(BUILD)/%.o,$(ASRCS))

# The SID player and its tune are written in ACME under music/, and
# tools/acme2calypsi.py turns them into the assembler the rest of this build
# speaks. Generated into build/ rather than checked into src/, so music/ holds
# the only copy of the tune there is. The converter is Python and nothing
# else; ACME itself is needed only by tools/checkmusic.py, which proves the
# two assemblers agree byte for byte.
MUSIC_SRC = music/player.asm music/music.asm
MUSIC_ASM = $(BUILD)/music_asm.s
OBJS     += $(BUILD)/music_asm.o

ELF      = $(BUILD)/sar.elf
# The MEGA65 ROM autoboots a file called autoboot.c65 and nothing else, so the
# game is written to the disk under that name.
PRG      = $(BUILD)/autoboot.c65
D81      = $(BUILD)/sar.d81

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
CONFIG_STAMP = $(BUILD)/config-$(PROFILE)-$(WIDE)-$(HGT_SIZE)-$(COL_SIZE)-$(FLYNOW)-$(REPORT)-$(CSTACK_GAME).stamp

# **Depends on this file.** Adding a per-directory compiler flag below and
# rebuilding changed nothing at all, because no object lists the Makefile as a
# prerequisite -- the measurement came back byte-identical and looked like the
# flag doing nothing.
$(CONFIG_STAMP): Makefile | $(BUILD)
	rm -f $(BUILD)/config-*.stamp
	touch $@

# Every object depends on every header. Crude, but the alternative is stale
# objects built against a changed memory layout, which fails in ways that look
# like hardware bugs.
$(BUILD)/%.o: src/%.c $(wildcard src/*.h) $(CONFIG_STAMP) | $(BUILD)
	cc6502 $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: src/%.s $(wildcard src/*.h) $(CONFIG_STAMP) | $(BUILD)
	as6502 $(ASFLAGS) -o $@ $<

# The tune. --zp is the one thing the converter changes rather than
# translates: the ACME player picks its two zero page pointers by hand, which
# a program sharing zero page with a C compiler and a live Kernal may not.
$(MUSIC_ASM): $(MUSIC_SRC) tools/acme2calypsi.py | $(BUILD)
	python3 tools/acme2calypsi.py music/player.asm $@ --zp ZP_PTR:2,ZP_ARP:2

$(BUILD)/music_asm.o: $(MUSIC_ASM) $(CONFIG_STAMP) | $(BUILD)
	as6502 $(ASFLAGS) -o $@ $<

# Both assemblers over the same tune, byte for byte. Needs acme on PATH.
checkmusic:
	python3 tools/checkmusic.py
.PHONY: checkmusic

$(ELF): $(OBJS)
	ln6502 $(LDFLAGS) --cstack-size $(CSTACK_GAME) -o $@ $(LINKFILE) $(OBJS)

# -o names the ELF; ln6502 writes the PRG beside it under the same stem.
# diskutil.rb names the file on disk after the host file, so both programs are
# copied to the basename they should carry there.
$(PRG): $(ELF)
	cp $(BUILD)/sar.prg $@

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
$(D81): $(PRG) $(RES)
	rm -f $@
	ruby tools/diskutil.rb $@ -name "search and rescue" -id sr \
	    -writeprg -copyf1 $(PRG) \
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
