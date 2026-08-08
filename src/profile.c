#include <mega65.h>

#include "loader.h"
#include "profile.h"
#include "vic4.h"

#define BENCH_ITERATIONS 2000

// A PAL frame is 312 raster lines at 50 Hz, so a line is 1/15600 s. Calibrating
// over 16 of them is long enough to measure accurately and short enough that
// the 16-bit clock cannot wrap whatever the timer's real rate turns out to be.
#define CAL_LINES     16
#define LINES_PER_SEC 15600L

// Anything longer than this in a single profile_add is a bad reading, not a
// slow column, and is counted rather than added.
#define SANE_TICKS 32768U

// Left in RAM for tools/profread.py to find in a memory dump.
struct results {
  uint8_t magic[4];
  uint32_t us[P_SLOTS];
  uint32_t count[C_SLOTS];
  uint16_t iterations;
  uint16_t cal_ticks;   // ticks measured over CAL_LINES raster lines
  uint16_t cal_lines;
  uint32_t hwmul_check;
  uint32_t frame_ticks; // whole-frame total from the 32-bit clock, as a check
};

volatile struct results profile_results = {{0xDE, 0xAD, 0xBE, 0xEF}};

static uint32_t ticks_per_second;

void profile_init(void)
{
  uint8_t i;

  for (i = 0; i < P_SLOTS; i++)
    profile_results.us[i] = 0;
  for (i = 0; i < C_SLOTS; i++)
    profile_results.count[i] = 0;
  profile_results.iterations = BENCH_ITERATIONS;
  profile_results.cal_lines = CAL_LINES;

  // Timer A free-runs off phi2; timer B counts timer A's underflows. Together
  // they are one 32-bit down counter. Neither is used by anything else -- the
  // Kernal only drives CIA2's timers for RS232.
  CIA2.ta_lo = 0xFF;
  CIA2.ta_hi = 0xFF;
  CIA2.tb_lo = 0xFF;
  CIA2.tb_hi = 0xFF;
  CIA2.cra = 0x11;  // force load, start, continuous, phi2 input
  CIA2.crb = 0x51;  // force load, start, continuous, counts timer A underflows
}

uint16_t profile_now(void)
{
  uint8_t hi, lo, again;

  // The counter runs between the two byte reads, so re-read the high byte and
  // try again if it moved.
  do {
    hi = CIA2.ta_hi;
    lo = CIA2.ta_lo;
    again = CIA2.ta_hi;
  } while (again != hi);

  return (uint16_t)hi << 8 | lo;
}

uint32_t profile_now32(void)
{
  uint16_t hi, lo, again;

  do {
    hi = (uint16_t)CIA2.tb_hi << 8 | CIA2.tb_lo;
    lo = profile_now();
    again = (uint16_t)CIA2.tb_hi << 8 | CIA2.tb_lo;
  } while (again != hi);

  return (uint32_t)hi << 16 | lo;
}

void profile_calibrate(void)
{
  uint8_t last = VICII.rasterline;
  uint8_t seen = 0;
  uint16_t t = profile_now();

  while (seen < CAL_LINES) {
    uint8_t r = VICII.rasterline;
    if (r != last) {
      last = r;
      seen++;
    }
  }
  profile_results.cal_ticks = (uint16_t)(t - profile_now());

  ticks_per_second =
      (uint32_t)profile_results.cal_ticks * (LINES_PER_SEC / CAL_LINES);
}

uint32_t profile_ticks_per_second(void)
{
  return ticks_per_second;
}

void profile_add(uint8_t slot, uint16_t start)
{
  uint16_t elapsed = (uint16_t)(start - profile_now());

  if (elapsed >= SANE_TICKS) {
    profile_results.count[C_LONG]++;
    return;
  }
  profile_results.us[slot] += elapsed;
}

void profile_add32(uint8_t slot, uint32_t start)
{
  profile_results.us[slot] += start - profile_now32();
}

void profile_count(uint8_t counter, uint16_t n)
{
  profile_results.count[counter] += n;
}

uint16_t profile_fps10(uint32_t frame_ticks)
{
  static uint32_t window_ticks;
  static uint16_t window_frames;
  static uint16_t fps10;

  profile_results.frame_ticks += frame_ticks;
  window_ticks += frame_ticks;
  window_frames++;

  // Average over a couple of seconds: long enough for a steady reading, short
  // enough to respond when the view gets heavier.
  if (window_ticks >= ticks_per_second * 2) {
    fps10 = (uint16_t)((uint32_t)window_frames * 10 * ticks_per_second / window_ticks);
    window_ticks = 0;
    window_frames = 0;
  }
  return fps10;
}

// Micro-benchmarks for the primitives the inner loop is built from. Each runs
// the same iteration count, and P_BENCH0 is an empty loop so the loop overhead
// can be subtracted from the rest.
void profile_bench(void)
{
  const uint8_t __far *map = (const uint8_t __far *)(HEIGHTMAP + 0x8000);
  uint8_t __far *fb = (uint8_t __far *)FB_A;
  volatile uint16_t sink16 = 0;
  volatile int32_t sink32 = 0;
  uint16_t i;
  uint32_t t;

  t = profile_now32();
  for (i = 0; i < BENCH_ITERATIONS; i++)
    sink16 = i;
  profile_add32(P_BENCH0, t);

  // A heightmap sample the way voxel.c does it: build the biased signed index
  // and read through a far pointer.
  t = profile_now32();
  for (i = 0; i < BENCH_ITERATIONS; i++)
    sink16 = map[(int16_t)(((i & 0xFF00) | (i >> 8)) ^ 0x8000)];
  profile_add32(P_BENCH1, t);

  // The projection: 16x16 into 32 bits, shifted back down.
  t = profile_now32();
  for (i = 0; i < BENCH_ITERATIONS; i++)
    sink32 = ((int32_t)(int16_t)i * (int16_t)(i + 1)) >> 8;
  profile_add32(P_BENCH2, t);

  // The same thing kept entirely in 16 bits.
  t = profile_now32();
  for (i = 0; i < BENCH_ITERATIONS; i++)
    sink16 = (uint16_t)((i & 0xFF) * (uint8_t)(i >> 8));
  profile_add32(P_BENCH3, t);

  // The 45GS02 hardware multiplier, driven directly.
  t = profile_now32();
  for (i = 0; i < BENCH_ITERATIONS; i++) {
    MATH.multina32 = i;
    MATH.multinb32 = i + 1;
    sink32 = (int32_t)MATH.multout32;
  }
  profile_add32(P_BENCH4, t);
  MATH.multina32 = 1234;
  MATH.multinb32 = 5678;
  profile_results.hwmul_check = MATH.multout32;

  // Eight pixels of a terrain span: byte writes at a stride of 8.
  t = profile_now32();
  for (i = 0; i < BENCH_ITERATIONS; i++) {
    uint8_t __far *p = fb;
    uint8_t n;
    for (n = 0; n < 8; n++) {
      *p = (uint8_t)i;
      p += 8;
    }
  }
  profile_add32(P_BENCH5, t);

  (void)sink16;
  (void)sink32;
}
