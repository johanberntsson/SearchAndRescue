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

# **The campaign is what goes on the disk.** campaign.yaml names the mission
# files, each mission names the world it is flown over and the figure that
# stands in it, and tools/campaign.py collects the lot: the map list, their
# ids and slot numbers, the sprite sheets, the disk's name, and the rules that
# generate and convert them all come out of $(BUILD)/campaign.mk below. They
# used to be three hand-kept lists here that had to agree with src/loader.h
# and src/mission.c, and could not be checked.
#
# campaign.bin is the missions themselves, read off the disk at boot. It is
# first in $(RES) because the loader reads it first: it says how many maps and
# figures there are to read after it.
CAMPAIGN = campaign.yaml
CAMPAIGN_MK = $(BUILD)/campaign.mk

MAP_RES  = $(foreach n,$(MAP_NUMS),$(BUILD)/map$(n).hgt $(BUILD)/map$(n).col \
                                    $(BUILD)/map$(n).pal $(BUILD)/map$(n).ovr)
# What one convmap run per map produces, and then the whole disk. Split
# because campaign.bin is built by the campaign itself, above, and must not be
# a target of the conversion rule as well. $(PNL_RES) is the panel artwork:
# map-independent, but converted with the maps because every map's palette has
# to reserve its entries.
CONV_RES = $(MAP_RES) $(SPR_RES) $(PNL_RES)
RES      = $(BUILD)/campaign.bin $(CONV_RES)

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

# The list file is the memory map, and it is worth having for its own sake --
# but also because the object ORDER decides where every symbol lands, so a map
# from a hand-run ln6502 over build/*.o describes a different program. An
# afternoon went into reading a struct at an address that came from such a
# link; take addresses from here.
$(ELF): $(OBJS)
	ln6502 $(LDFLAGS) --cstack-size $(CSTACK_GAME) \
	    --list-file $(BUILD)/sar.lst -o $@ $(LINKFILE) $(OBJS)

# -o names the ELF; ln6502 writes the PRG beside it under the same stem.
# diskutil.rb names the file on disk after the host file, so both programs are
# copied to the basename they should carry there.
$(PRG): $(ELF)
	cp $(BUILD)/sar.prg $@

comma := ,
empty :=
space := $(empty) $(empty)

# The campaign, and the make it generates. GNU make notices that an included
# file is out of date, remakes it and starts again, so this bootstraps itself
# on a clean tree -- and `missions/*.yaml` rather than $(MISSION_YAMLS),
# because that variable is defined by the file being built.
$(BUILD)/campaign.bin $(CAMPAIGN_MK) &: $(CAMPAIGN) $(wildcard missions/*.yaml) \
                                        $(wildcard maps/*.yaml) \
                                        tools/campaign.py | $(BUILD)
	python3 tools/campaign.py $(CAMPAIGN) $(BUILD)/campaign.bin $(CAMPAIGN_MK)

-include $(CAMPAIGN_MK)

# tools/diskutil.rb refuses to overwrite a file that already exists on the image,
# so the image is always built from scratch.
$(D81): $(PRG) $(RES)
	rm -f $@
	ruby tools/diskutil.rb $@ -name "$(DISK_NAME)" -id sr \
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
