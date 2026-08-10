TARGET   = --target=mega65
# --no-cross-call and --strong-inline were both tried here and both measured
# slower (615ms a frame against 533ms), so the defaults stay.
# PROFILE=0 compiles out the per-column instrumentation; the FPS counter stays.
PROFILE ?= 1
# WIDE=1 renders all 320 pixels instead of stretching 160. See vic4.h.
WIDE    ?= 0
# Map resolutions, powers of two from 256 up to the source PNGs' 1024. Above
# 256 the heightmap leaves chip RAM and the inner loop pays for it; the
# colourmap is read once per span and is nearly free at any size. Both maps
# are exomizer-crunched, which is what makes these fit a d81.
HGT_SIZE ?= 512
COL_SIZE ?= 1024
SIZEFLAGS = -DWIDE=$(WIDE) -DHGT_SIZE=$(HGT_SIZE) -DCOL_SIZE=$(COL_SIZE)
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
RES      = $(BUILD)/terrain.hgt $(BUILD)/terrain.col $(BUILD)/terrain.pal \
           $(BUILD)/terrain.ovr

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
CONFIG_STAMP = $(BUILD)/config-$(PROFILE)-$(WIDE)-$(HGT_SIZE)-$(COL_SIZE).stamp

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

$(RES) &: resources/D1.png resources/C1W.png tools/convmap.py $(CONFIG_STAMP) | $(BUILD)
	python3 tools/convmap.py resources/D1.png resources/C1W.png $(BUILD)/terrain \
	    $(HGT_SIZE) $(COL_SIZE)

# tools/diskutil.rb refuses to overwrite a file that already exists on the image,
# so the image is always built from scratch.
$(D81): $(PRG) $(RES)
	rm -f $@
	ruby tools/diskutil.rb $@ -name "search and rescue" -id sr \
	    -writeprg -copyf1 $(PRG) \
	    -writeseq -copyf1 $(RES)

clean:
	rm -rf $(BUILD)

.PHONY: all run prg clean
