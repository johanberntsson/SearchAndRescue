// Timing and event counts, for finding out where the frame goes instead of
// guessing. Detailed results are left in a marked structure in RAM; read them
// with tools/profread.py on a `xemu-xmega65 -dumpmem` image.
//
// The clock is CIA2's two timers chained into one 32-bit down counter running
// off phi2. profile_ticks_per_second() says how fast that actually is, worked
// out at startup against the raster rather than assumed.
#ifndef PROFILE_H
#define PROFILE_H

#include <stdint.h>

// Per-column timing and the event counters cost around 0.6% of a frame, so
// they can be compiled out with `make PROFILE=0`. The frame clock and the FPS
// counter are always on: they cost two clock reads a frame.
#ifndef PROFILE_DETAIL
#define PROFILE_DETAIL 1
#endif

#if PROFILE_DETAIL
#define PROF_NOW()       profile_now()
#define PROF_ADD(s, t)   profile_add((s), (t))
#define PROF_COUNT(c, n) profile_count((c), (n))
#else
#define PROF_NOW()       0
#define PROF_ADD(s, t)   ((void)(t))
#define PROF_COUNT(c, n) ((void)0)
#endif

// A frame is longer than the 16-bit clock can span, so it is measured as two
// parts that add up: P_COLUMN + P_OTHER.
enum {
  P_OTHER,   // input, camera, HUD and the buffer flip
  P_COLUMN,  // all of voxel_render's per-column work, summed over the frame
  P_BENCH0,  // micro-benchmark slots, see profile_bench()
  P_BENCH1,
  P_BENCH2,
  P_BENCH3,
  P_BENCH4,
  P_BENCH5,
  P_SLOTS
};

enum {
  C_FRAMES,  // frames rendered
  C_SAMPLE,  // heightmap samples taken
  C_SPAN,    // terrain spans drawn
  C_SPANPIX, // pixels written by terrain spans
  C_SKYPIX,  // pixels written by the sky fill
  C_LONG,    // profile_add calls whose interval looked implausibly long
  C_SLOTS
};

void profile_init(void);

// Work out the clock's real rate against the raster, rather than assuming it.
// Must run before profile_fps10.
void profile_calibrate(void);

// Short intervals, cheap to read. Only valid across spans below 65536 ticks.
uint16_t profile_now(void);

// Whole frames. Costs more to read, so keep it out of inner loops.
uint32_t profile_now32(void);

void profile_add(uint8_t slot, uint16_t start);

// For intervals that may exceed the 16-bit clock's range.
void profile_add32(uint8_t slot, uint32_t start);
void profile_count(uint8_t counter, uint16_t n);

uint32_t profile_ticks_per_second(void);

// Feed each frame's duration in ticks; returns frames per second times ten,
// recalculated every couple of seconds and unchanged in between.
uint16_t profile_fps10(uint32_t frame_ticks);

// Run the micro-benchmarks and record them in the P_BENCH slots.
void profile_bench(void);

#endif
