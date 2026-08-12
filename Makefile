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
# FLYNOW=1 skips the title and menu screens and launches straight into the
# flight. The headless profiling run cannot press a key, so without it a
# -dumpmem image has no frames in it at all. Never for timing comparisons of
# anything but itself -- it is the same code, just entered differently.
FLYNOW  ?= 0
# REPORT=n holds the startup benchmark report on screen for n seconds instead
# of 20. For a session at the real MEGA65, where that report is the only way to
# read the attic RAM figures: `make REPORT=120`.
REPORT  ?= 20
# Map resolutions, powers of two from 256 up to the source PNGs' 1024. Above
# 256 the heightmap leaves chip RAM and the inner loop pays for it; the
# colourmap is read once per span and is nearly free at any size. Both maps
# are exomizer-crunched, which is what makes these fit a d81.
HGT_SIZE ?= 512
COL_SIZE ?= 1024
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

ELF      = $(BUILD)/sar.elf
PRG      = $(BUILD)/autoboot.c65
D81      = $(BUILD)/sar.d81
# One sprite sheet per figure the game can stand in the world, in the order
# src/sprite.c numbers them: 0 the lost hiker, 1 the pair by the lake. They are
# converted together because they share the one on-screen palette.
SPRITES  = resources/survivor-sprite.png resources/medicalemergency-sprite.png
RES      = $(BUILD)/terrain.hgt $(BUILD)/terrain.col $(BUILD)/terrain.pal \
           $(BUILD)/terrain.ovr $(BUILD)/terrain.spr $(BUILD)/terrain.sp2

all: $(D81)

run: $(D81)
	xemu-xmega65 -besure -8 $(D81)

# Quick turnaround for tests that do not need the resource files.
prg: $(PRG)
	xemu-xmega65 -besure -prg $(PRG)

$(BUILD):
	mkdir -p $(BUILD)

# Both compiler and assembler see these, so changing either has to force a
# rebuild. Without it, `make PROFILE=0` and then `make` leaves every object
# built against the wrong flag and the counters silently stay off -- and a
# half-rebuilt WIDE change is a memory map that disagrees with itself.
CONFIG_STAMP = $(BUILD)/config-$(PROFILE)-$(WIDE)-$(HGT_SIZE)-$(COL_SIZE)-$(FLYNOW)-$(REPORT).stamp

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
# The MEGA65 ROM only autoboots a file called autoboot.c65, and diskutil.rb
# names the file on disk after the host file, so rename it here.
$(PRG): $(ELF)
	cp $(BUILD)/sar.prg $@

comma := ,
empty :=
space := $(empty) $(empty)

$(RES) &: resources/D1.png resources/C1W.png $(SPRITES) \
          tools/convmap.py $(CONFIG_STAMP) | $(BUILD)
	python3 tools/convmap.py resources/D1.png resources/C1W.png \
	    $(subst $(space),$(comma),$(SPRITES)) \
	    $(BUILD)/terrain $(HGT_SIZE) $(COL_SIZE)

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
