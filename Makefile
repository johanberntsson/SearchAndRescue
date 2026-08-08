TARGET   = --target=mega65
# --no-cross-call and --strong-inline were both tried here and both measured
# slower (615ms a frame against 533ms), so the defaults stay.
# PROFILE=0 compiles out the per-column instrumentation; the FPS counter stays.
PROFILE ?= 1
CFLAGS   = $(TARGET) -O2 --speed -DPROFILE_DETAIL=$(PROFILE)
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
RES      = $(BUILD)/terrain.hgt $(BUILD)/terrain.col $(BUILD)/terrain.pal

all: $(D81)

run: $(D81)
	xemu-xmega65 -besure -8 $(D81)

# Quick turnaround for tests that do not need the resource files.
prg: $(PRG)
	xemu-xmega65 -besure -prg $(PRG)

$(BUILD):
	mkdir -p $(BUILD)

# Every object depends on every header. Crude, but the alternative is stale
# objects built against a changed memory layout, which fails in ways that look
# like hardware bugs.
$(BUILD)/%.o: src/%.c $(wildcard src/*.h) | $(BUILD)
	cc6502 $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: src/%.s | $(BUILD)
	as6502 $(TARGET) -o $@ $<

$(ELF): $(OBJS)
	ln6502 $(LDFLAGS) -o $@ $(LINKFILE) $(OBJS)

# -o names the ELF; ln6502 writes the PRG beside it under the same stem.
# The MEGA65 ROM only autoboots a file called autoboot.c65, and diskutil.rb
# names the file on disk after the host file, so rename it here.
$(PRG): $(ELF)
	cp $(BUILD)/sar.prg $@

$(RES) &: resources/D1.png resources/C1W.png tools/convmap.py | $(BUILD)
	python3 tools/convmap.py resources/D1.png resources/C1W.png $(BUILD)/terrain

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
