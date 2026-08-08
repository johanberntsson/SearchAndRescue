#!/usr/bin/env python3
"""Format the profiler's results out of a memory dump.

    xemu-xmega65 -besure -headless -sleepless -8 build/sar.d81 -dumpmem mem.bin
    tools/profread.py mem.bin

See src/profile.h for what the slots mean.
"""

import struct
import sys

MAGIC = b"\xDE\xAD\xBE\xEF"
TIMES = ["other", "column", "bench0", "bench1", "bench2", "bench3", "bench4", "bench5"]
COUNTS = ["frames", "samples", "spans", "span pixels", "sky pixels", "bad readings"]
# Index 1 is skipped: that loop optimises away and never measured anything.
BENCH_NAMES = [
    "empty loop",
    None,
    "32-bit multiply + shift",
    "16-bit multiply",
    "hardware multiplier",
    "8-pixel span write",
]

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
    render, other = us[1], us[0]
    frame_us = (render + other) / frames

    whole_us = frame_ticks * 1e6 / tick_hz / frames
    print("frames rendered      %d" % frames)
    print("frame time           %.1f ms  (%.1f fps)   [32-bit clock]"
          % (whole_us / 1000, 1e6 / whole_us))
    print("frame time           %.1f ms  (%.1f fps)   [sum of parts]"
          % (frame_us / 1000, 1e6 / frame_us))
    print("  render             %.1f ms  (%.0f%%)"
          % (render / frames / 1000, 100 * render / (render + other)))
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
            per = render / frames / (total / frames)
            print("cost per %-17s %6.2f us  (%5.0f cycles at 40.5 MHz)"
                  % (label, per, per * CPU_HZ / 1e6))
    print()

    if counts[5]:
        print("WARNING: %d readings discarded as implausibly long\n" % counts[5])

    print("micro-benchmarks, %d iterations, overhead of the empty loop removed:" % iterations)
    base = us[2]
    for i, name in enumerate(BENCH_NAMES):
        if name is None:
            continue
        total = us[2 + i]
        net = total if i == 0 else total - base
        print("  %-28s %7.2f us/iter  (%5.0f cycles)"
              % (name, net / iterations, net / iterations * CPU_HZ / 1e6))
    print()
    print("hardware multiplier check: 1234 * 5678 = %d (expected %d)"
          % (hwmul, 1234 * 5678))


if __name__ == "__main__":
    main()
