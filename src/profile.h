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

// A frame is longer than the 16-bit clock can span, so it is measured in
// parts that add up: P_COLUMN + P_SKY + P_OTHER.
enum {
  P_OTHER,   // input, camera, HUD and the buffer flip
  P_COLUMN,  // all of voxel_render's per-column work, summed over the frame
  P_SKY,     // the sky DMA at the top of each frame
  // Micro-benchmarks, see profile_bench(). Two groups with a baseline each:
  // the arithmetic ones are C and subtract P_C_EMPTY, the memory ones are
  // assembly (src/bench_asm.s) and subtract P_ASM_EMPTY. They cannot share a
  // baseline -- the compiler's loop overhead is not the assembly loop's.
  P_C_EMPTY,
  P_MUL32,     // the 32-bit multiply the compiler emits
  P_MUL16,
  P_HWMUL,     // the 45GS02 multiplier, driven directly
  P_ASM_EMPTY,
  P_READ_CHIP, // map sample, scattered, chip RAM
  P_READ_ATTIC,
  P_SEQ_CHIP,  // the same read walking straight up memory
  P_SEQ_ATTIC,
  P_WRITE_CHIP, // span pixel, byte write at a stride of 8
  P_WRITE_ATTIC,
  P_DMA_CHIP,   // bulk moves, timed per block rather than per iteration
  P_DMA_ATTIC,
  P_DMA_FILL,
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

// Run the micro-benchmarks and record them in the bench slots.
void profile_bench(void);

// Print the memory benchmarks to the text screen and wait for a key, or for
// `seconds` to pass. For real hardware, which has no -dumpmem: it must be
// called before vic4_init, while there is still a text screen to print on.
void profile_report(uint8_t seconds);

#endif
