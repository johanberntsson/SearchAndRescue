#include <mega65.h>
#include <stdio.h>

#include "dma.h"
#include "loader.h"
#include "profile.h"
#include "vic4.h"

#define BENCH_ITERATIONS 2000

// The DMA benchmarks move real blocks, so they need their own, much smaller
// count. 16 x 4096 is long enough to time and short enough not to stall
// startup on a slow bus.
#define DMA_ITERATIONS 16
#define DMA_BYTES      4096

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
  uint16_t dma_iterations;
  uint16_t dma_bytes;
  uint32_t attic_check;  // pattern written across attic RAM and read back
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
  profile_results.dma_iterations = DMA_ITERATIONS;
  profile_results.dma_bytes = DMA_BYTES;
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

// Zero page parameters for the assembly benchmarks in src/bench_asm.s.
uint8_t __far *__attribute__((zpage)) bn_ptr;
__zpage uint16_t bn_px, bn_py, bn_n;
__zpage uint8_t bn_sink;

void bench_empty(void);
void bench_read_walk(void);
void bench_read_seq(void);
void bench_write_span(void);

// Run one of the assembly loops with the pointer parked at base, and record
// how long it took. They all take the same parameters and carry the same
// counter tail, so P_ASM_EMPTY subtracts out exactly.
static void run_asm_bench(uint8_t slot, void (*fn)(void), uint32_t base)
{
  uint32_t t;

  bn_ptr = (uint8_t __far *)base;
  bn_px = 0;
  bn_py = 0;
  bn_n = BENCH_ITERATIONS;
  t = profile_now32();
  fn();
  profile_add32(slot, t);
}

// Micro-benchmarks for the primitives the inner loop is built from.
//
// The arithmetic group is C and subtracts P_C_EMPTY. The memory group is
// assembly, because the compiler cross-calls loop bodies into shared
// fragments and a chip RAM loop and an attic RAM loop written the same way in
// C come out as different code -- measured that way the attic read looked
// faster than the chip read.
void profile_bench(void)
{
  volatile int32_t sink32 = 0;
  volatile uint16_t sink16 = 0;
  uint16_t i;
  uint32_t t;

  t = profile_now32();
  for (i = 0; i < BENCH_ITERATIONS; i++)
    sink16 = i;
  profile_add32(P_C_EMPTY, t);

  // The projection: 16x16 into 32 bits, shifted back down.
  t = profile_now32();
  for (i = 0; i < BENCH_ITERATIONS; i++)
    sink32 = ((int32_t)(int16_t)i * (int16_t)(i + 1)) >> 8;
  profile_add32(P_MUL32, t);

  // The same thing kept entirely in 16 bits.
  t = profile_now32();
  for (i = 0; i < BENCH_ITERATIONS; i++)
    sink16 = (uint16_t)((i & 0xFF) * (uint8_t)(i >> 8));
  profile_add32(P_MUL16, t);

  // The 45GS02 hardware multiplier, driven directly.
  t = profile_now32();
  for (i = 0; i < BENCH_ITERATIONS; i++) {
    MATH.multina32 = i;
    MATH.multinb32 = i + 1;
    sink32 = (int32_t)MATH.multout32;
  }
  profile_add32(P_HWMUL, t);
  MATH.multina32 = 1234;
  MATH.multinb32 = 5678;
  profile_results.hwmul_check = MATH.multout32;

  // Attic RAM: 8 MB of HyperRAM off the slow device bus. These decide whether
  // the colourmap or a back buffer can leave chip RAM, which is what the
  // whole 320-wide question hangs on.
  //
  // xemu does not model that bus. In the emulator the attic figures come out
  // at roughly chip RAM speed and mean nothing at all; only a run on real
  // hardware answers this.
  // Distinct bytes at spread-out offsets, read back in a different order.
  // Anything other than $11223344 means attic RAM is missing, smaller than
  // it should be, or wrapping -- and a wrap would otherwise show up as a
  // suspiciously fast benchmark rather than as an error.
  {
    volatile uint8_t __far *a0 = (uint8_t __far *)ATTIC_BASE;
    volatile uint8_t __far *a1 = (uint8_t __far *)(ATTIC_BASE + 0x4000);
    volatile uint8_t __far *a2 = (uint8_t __far *)(ATTIC_BASE + 0x100000);
    volatile uint8_t __far *a3 = (uint8_t __far *)(ATTIC_BASE + 0x400000);

    a0[0] = 0x11;
    a1[0] = 0x22;
    a2[0] = 0x33;
    a3[0] = 0x44;
    profile_results.attic_check = (uint32_t)a0[0] << 24 | (uint32_t)a1[0] << 16
                                  | (uint32_t)a2[0] << 8 | a3[0];
  }

  run_asm_bench(P_ASM_EMPTY, bench_empty, HEIGHTMAP);
  run_asm_bench(P_READ_CHIP, bench_read_walk, HEIGHTMAP);
  run_asm_bench(P_READ_ATTIC, bench_read_walk, ATTIC_BASE);
  run_asm_bench(P_SEQ_CHIP, bench_read_seq, HEIGHTMAP);
  run_asm_bench(P_SEQ_ATTIC, bench_read_seq, ATTIC_BASE);
  // The span writes walk 8 bytes an iteration, so they need room for
  // BENCH_ITERATIONS * 8 bytes and must not land on anything that matters.
  run_asm_bench(P_WRITE_CHIP, bench_write_span, FB_A);
  run_asm_bench(P_WRITE_ATTIC, bench_write_span, ATTIC_BASE);

  // Bulk moves. FB_A and FB_B are overwritten by the first frame anyway, so
  // they are free scratch.
  t = profile_now32();
  for (i = 0; i < DMA_ITERATIONS; i++)
    dma_copy(FB_B, FB_A, DMA_BYTES);
  profile_add32(P_DMA_CHIP, t);

  t = profile_now32();
  for (i = 0; i < DMA_ITERATIONS; i++)
    dma_copy(ATTIC_BASE, FB_A, DMA_BYTES);
  profile_add32(P_DMA_ATTIC, t);

  t = profile_now32();
  for (i = 0; i < DMA_ITERATIONS; i++)
    dma_fill(FB_A, 0, DMA_BYTES);
  profile_add32(P_DMA_FILL, t);

  (void)sink16;
  (void)sink32;
}

// ---------------------------------------------------------------------------
// On-screen report, for real hardware. xemu has -dumpmem and tools/profread.py;
// a MEGA65 has neither, and the attic RAM figures are exactly the ones that
// only mean anything on the real machine.

#define CPU_HZ 40500000UL

// The MEGA65's ASCII key register: nonzero when a key is waiting, cleared by
// writing to it. Not in the SDK header.
#define KEY_ASCII (*(volatile uint8_t *)0xD610)

// Cycles per clock tick, times 100, worked out once by profile_report. The
// tick rate was measured against the raster rather than assumed, so this
// follows whatever the machine actually runs at.
//
// It is a variable and not a function on purpose: calling one inside a 32-bit
// expression makes Calypsi 5.18 emit a call to `_FillZPQ`, a runtime helper
// that is not in any of its libraries, and the link fails.
static uint32_t cyc_per_tick100;

static uint16_t per_iteration(uint8_t slot, uint8_t base)
{
  uint32_t net = profile_results.us[slot] - profile_results.us[base];

  return (uint16_t)(net * cyc_per_tick100 / ((uint32_t)BENCH_ITERATIONS * 100));
}

// Cycles per byte, times 100: these are fractions of a cycle.
static uint16_t per_byte100(uint8_t slot)
{
  uint32_t moved = (uint32_t)DMA_ITERATIONS * DMA_BYTES;

  return (uint16_t)(profile_results.us[slot] * cyc_per_tick100 / moved);
}

static void row(const char *name, uint8_t chip, uint8_t attic)
{
  uint16_t c = per_iteration(chip, P_ASM_EMPTY);
  uint16_t a = per_iteration(attic, P_ASM_EMPTY);

  // The ratio is the number that decides anything, so work it out here
  // rather than leaving it to be done by eye.
  printf(" %-18s%5u%6u  %u.%02uX\n", name, c, a,
         c ? a / c : 0, c ? (uint16_t)((uint32_t)a * 100 / c) % 100 : 0);
}

void profile_report(uint8_t seconds)
{
  uint32_t deadline;

  cyc_per_tick100 = CPU_HZ / (ticks_per_second / 100);

  printf("\nCPU %luKHZ  CLOCK %luKHZ\n\n",
         (unsigned long)(CPU_HZ / 1000), (unsigned long)(ticks_per_second / 1000));

  printf("CYCLES PER OP      CHIP ATTIC  RATIO\n");
  row("MAP SAMPLE", P_READ_CHIP, P_READ_ATTIC);
  row("SEQUENTIAL READ", P_SEQ_CHIP, P_SEQ_ATTIC);
  row("SPAN PIXEL WRITE", P_WRITE_CHIP, P_WRITE_ATTIC);

  printf("\nDMA CYCLES PER BYTE\n");
  printf(" COPY CHIP-CHIP    %u.%02u\n",
         per_byte100(P_DMA_CHIP) / 100, per_byte100(P_DMA_CHIP) % 100);
  printf(" COPY ATTIC-CHIP   %u.%02u\n",
         per_byte100(P_DMA_ATTIC) / 100, per_byte100(P_DMA_ATTIC) % 100);
  printf(" FILL CHIP         %u.%02u\n",
         per_byte100(P_DMA_FILL) / 100, per_byte100(P_DMA_FILL) % 100);

  printf("\nATTIC ADDRESSING  %08lX %s\n",
         (unsigned long)profile_results.attic_check,
         profile_results.attic_check == 0x11223344UL ? "OK" : "BAD");
  printf("HW MULTIPLIER     %lu %s\n",
         (unsigned long)profile_results.hwmul_check,
         profile_results.hwmul_check == 1234UL * 5678 ? "OK" : "BAD");

  printf("\nANY KEY TO FLY\n");

  // Wait for a key or for the timeout, so that an unattended run (xemu
  // headless, say) still gets on with rendering.
  KEY_ASCII = 0;
  deadline = profile_now32() - (uint32_t)seconds * ticks_per_second;
  while (KEY_ASCII == 0 && profile_now32() > deadline)
    ;
  KEY_ASCII = 0;
}
