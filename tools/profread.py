#!/usr/bin/env python3
"""Format the profiler's results out of a memory dump.

    xemu-xmega65 -besure -headless -sleepless -8 build/sar.d81 -dumpmem mem.bin
    tools/profread.py mem.bin

See src/profile.h for what the slots mean.
"""

import struct
import sys

MAGIC = b"\xDE\xAD\xBE\xEF"
TIMES = ["other", "column", "sky", "sprite"] + ["bench%d" % i for i in range(15)]
COUNTS = ["frames", "samples", "spans", "span pixels", "sky pixels", "bad readings"]

# Two groups, each with its own empty-loop baseline: the compiler's loop
# overhead is not the assembly loop's, and subtracting one from the other
# produced negative readings. See src/profile.h.
BENCH0 = 4  # first micro-benchmark slot, see src/profile.h
C_BENCH = [("32-bit multiply + shift", 1),
           ("16-bit multiply", 2),
           ("hardware multiplier", 3)]
ASM_BENCH = [("map sample, scattered", 5, 6),
             ("map read, sequential", 7, 8),
             ("span pixel, one byte write", 9, 10)]
# Timed per block rather than per iteration, and reported as throughput.
DMA_NAMES = [("copy, chip to chip", 11), ("copy, attic to chip", 12),
             ("copy, chip to attic", 13), ("fill, chip RAM", 14)]

CPU_HZ = 40_500_000


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    mem = open(sys.argv[1], "rb").read()

    at = mem.find(MAGIC)
    if at < 0:
        sys.exit("no profiler results in this dump")
    off = at + 4

    ticks = list(struct.unpack_from("<%dI" % len(TIMES), mem, off))
    off += 4 * len(TIMES)
    counts = list(struct.unpack_from("<%dI" % len(COUNTS), mem, off))
    off += 4 * len(COUNTS)
    iterations, cal_ticks, cal_lines = struct.unpack_from("<3H", mem, off)
    off += 6
    hwmul, frame_ticks = struct.unpack_from("<2I", mem, off)
    off += 8
    dma_iterations, dma_bytes = struct.unpack_from("<2H", mem, off)
    off += 4
    attic_check, = struct.unpack_from("<I", mem, off)

    frames = counts[0]
    if not frames:
        sys.exit("no frames were rendered")

    # The clock's real rate, measured against PAL raster lines at 15625 Hz.
    if not cal_ticks:
        sys.exit("clock was not calibrated")
    tick_hz = cal_ticks * (15625.0 / cal_lines)
    print("clock %.3f MHz (%d ticks over %d raster lines)"
          % (tick_hz / 1e6, cal_ticks, cal_lines))
    print()

    us = [t * 1e6 / tick_hz for t in ticks]
    bench = us[BENCH0:]
    render, sky, sprite, other = us[1], us[2], us[3], us[0]
    frame_us = (render + sky + sprite + other) / frames

    whole_us = frame_ticks * 1e6 / tick_hz / frames
    print("frames rendered      %d" % frames)
    print("frame time           %.1f ms  (%.1f fps)   [32-bit clock]"
          % (whole_us / 1000, 1e6 / whole_us))
    # Everything below the whole-frame clock comes from the detailed
    # instrumentation, which `make PROFILE=0` compiles out.
    total = render + sky + sprite + other
    if not total:
        print("\nbuilt with PROFILE=0: no detailed instrumentation in this run")
        return
    print("frame time           %.1f ms  (%.1f fps)   [sum of parts]"
          % (frame_us / 1000, 1e6 / frame_us))
    print("  terrain columns    %.1f ms  (%.0f%%)"
          % (render / frames / 1000, 100 * render / total))
    print("  sky DMA            %.2f ms  (%.0f%%)"
          % (sky / frames / 1000, 100 * sky / total))
    print("  survivor sprite    %.2f ms  (%.0f%%)"
          % (sprite / frames / 1000, 100 * sprite / total))
    print("  everything else    %.2f ms" % (other / frames / 1000))
    print()

    print("per frame:")
    for name, total in zip(COUNTS[1:], counts[1:]):
        if total:
            print("  %-18s %8.0f" % (name, total / frames))
    print()

    for label, total in (("heightmap sample", counts[1]),
                         ("pixel written", counts[3] + counts[4])):
        if total:
            per = (render + sky) / frames / (total / frames)
            print("cost per %-17s %6.2f us  (%5.0f cycles at 40.5 MHz)"
                  % (label, per, per * CPU_HZ / 1e6))
    print()

    if counts[5]:
        print("WARNING: %d readings discarded as implausibly long\n" % counts[5])

    def cycles(total):
        return total / iterations * CPU_HZ / 1e6

    print("arithmetic, %d iterations in C, empty loop removed:" % iterations)
    for name, slot in C_BENCH:
        net = bench[slot] - bench[0]
        print("  %-28s %7.2f us/iter  (%5.0f cycles)"
              % (name, net / iterations, cycles(net)))
    print()

    print("memory, %d iterations in assembly, empty loop removed:" % iterations)
    print("  %-28s %11s %11s %8s" % ("", "chip RAM", "attic RAM", "ratio"))
    for name, chip, attic in ASM_BENCH:
        c, a = bench[chip] - bench[4], bench[attic] - bench[4]
        print("  %-28s %7.0f cyc %7.0f cyc %7.1fx"
              % (name, cycles(c), cycles(a), a / c if c > 0 else 0))
    print()

    if dma_iterations and dma_bytes:
        moved = dma_iterations * dma_bytes
        print("bulk moves by DMA, %d x %d bytes:" % (dma_iterations, dma_bytes))
        for name, slot in DMA_NAMES:
            if bench[slot] <= 0:
                continue
            print("  %-28s %7.2f MB/s  (%4.2f cycles/byte)"
                  % (name, moved / bench[slot], bench[slot] * CPU_HZ / 1e6 / moved))
        print()

    print("hardware multiplier check: 1234 * 5678 = %d (expected %d)"
          % (hwmul, 1234 * 5678))
    print("attic RAM readback:        $%08X (expected $11223344)%s"
          % (attic_check,
             "" if attic_check == 0x11223344 else "  -- ATTIC RAM NOT ADDRESSED"))
    print()
    print("NOTE: xemu does not model the attic RAM bus, so in the emulator the")
    print("      attic column comes out at chip RAM speed and means nothing.")
    print("      Only a run on real hardware answers that question.")


if __name__ == "__main__":
    main()
