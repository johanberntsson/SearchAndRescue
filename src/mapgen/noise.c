// The terrain noise, on the machine: `fbm_octaves` from tools/genmap.py.
//
// This is the dominant term of the whole generator -- the costing in
// documentation/on-device-maps.md puts the noise at 150-400 cycles a pixel
// against 40-50 for everything else per pass -- so it is the first thing
// ported and the thing worth measuring before any of the rest is written.
//
// **What it computes is the weighted octave sum, normalised, and not the
// stretch that follows it.** genmap.py's `fbm()` is `stretch(fbm_octaves())`,
// and the split is deliberate: the stretch needs a histogram over the whole
// field, so it is a second pass with its own shape, and it raises a
// representation question this does not (see the note at the bottom).
//
// Verified against the PC by checksum, not by eye: `tools/fbmcheck.py` prints
// the same number `noise_report` does. -dumpmem writes chip RAM only and
// cannot see attic RAM, so a checksum is the only channel there is.

#include <stdint.h>

#include "../handover.h"
#include "fixed.h"
#include "noise.h"

// The field, and the parameters it is generated with. These are
// maps/island.yaml's, spelled out rather than read from anywhere: reading a
// map file is `map.bin`, which is a later step, and hard-coding one map is
// what makes this an experiment rather than half a generator.
//
//   scale: medium    -> lattice 4, octaves +0
//   ruggedness: rolling -> 4 octaves, gain 0.45
//
// int(0.45 * 65536) is 29491, which is the number genmap.py passes; deriving
// it here from a decimal would be a different number on a bad day.
#define SIZE      512
#define SIZE_MASK (SIZE - 1)
#define OCT       4
#define BASE      4
#define GAIN      29491UL
#define SEED      12345UL

// The lattice grids for every octave, end to end: 4^2 + 8^2 + 16^2 + 32^2.
// Small enough to keep them all resident, which is what lets the field be
// built a row at a time across all four octaves at once -- and that is the
// arrangement the costing asks for, because the alternative accumulates
// octaves in attic RAM and pays +15 cycles a read for every one of them.
#define CORNERS (BASE * BASE * (1 + 4 + 16 + 64))

static uint16_t corner[CORNERS];
static uint16_t coff[OCT];    // where each octave's grid starts
static uint8_t lshift[OCT];   // log2 of the octave's step, so sx >> this is a corner
static uint16_t offx[OCT], offy[OCT];  // np.roll's two offsets
static uint32_t amps[OCT];

// The interpolation weight, per octave. **It is one lattice cell long, not one
// row**: the weight is smoothstep of how far across a cell the pixel is, so it
// repeats every `step` pixels and the whole octave needs `step` of them --
// 128 + 64 + 32 + 16 entries against four rows of 512. That is 3.6 KB back,
// which is most of what the edge caches below cost.
#define WTAB (SIZE / BASE * 2 - SIZE / (BASE << (OCT - 1)))
static uint16_t wtab[WTAB];
static uint16_t woff[OCT];

// The stretch's histogram: 1024 buckets of the field, six bits of a value to a
// bucket. Keep in step with BUCKETSHIFT in tools/genmap.py -- the bucket width
// lands directly on the map's heights, which is why it is 1024 and not 256.
#define BUCKETSHIFT 6
#define BUCKETS     (int)(ONE >> BUCKETSHIFT)

// **The two lattice rows an output row sits between, interpolated along x and
// kept, and the histogram that shares their memory.** The caches are why the
// octave loop is two multiplies a pixel and not four: `top` and `bot` depend on
// the lattice row, not the pixel row, so they are good for the `step` output
// rows that share a lattice row -- and when it does change, the new top is the
// old bot, so a pointer swap and one rebuild covers it.
//
// The union is the same bargain the loader's staging buffer strikes with the
// sprite's: **the two are never live at the same moment.** The caches are dead
// the instant the field is finished, and the histogram is not read until the
// pass after that. It matters because 8 KB and 4 KB do not both fit -- stage
// one has under 4 KB of its 32 spare with the caches in it.
static union {
  uint16_t edge[2][OCT][SIZE];
  uint32_t hist[BUCKETS + 1];
} work;

static uint16_t *e_top[OCT], *e_bot[OCT];
static uint16_t e_row[OCT];   // which lattice row e_top holds
#define E_NONE 0xFFFF

// One row of the octave sum, at Q8.16 -- the sum reaches several times ONE
// before it is normalised. In chip RAM on purpose: see above.
static uint32_t acc[SIZE];

static uint32_t weight_recip;

// The inner loop's parameters, in zero page where src/mapgen/noise_asm.s can
// reach them -- the same arrangement the renderer's `vx_*` block uses. The
// four scratch words are the assembly's, not this file's.
const uint16_t *__attribute__((zpage)) nz_top;
const uint16_t *__attribute__((zpage)) nz_bot;
uint32_t *__attribute__((zpage)) nz_acc;
__zpage uint16_t nz_wy;
__zpage uint32_t nz_amp;
__zpage uint16_t nz_t, nz_b, nz_d, nz_n;
__zpage uint8_t nz_chunks;

// ... and the store and stretch passes', in store_asm.s and stretch_asm.s. The
// two checksum accumulators live here rather than in a caller because they
// carry across every row of the field. nz_recip is the weight reciprocal for
// the store pass and the stretch's own afterwards: the two never overlap, and
// zero page has nothing to spare.
uint8_t __far *__attribute__((zpage)) nz_out;
uint32_t *__attribute__((zpage)) nz_hist;
__zpage uint32_t nz_recip;
__zpage uint16_t nz_sum_a, nz_sum_b, nz_lo, nz_ptr;

void noise_blend(void);
void noise_store(void);
void stretch_hist(void);
void stretch_apply(void);

// The lattice hash. genmap.py does this in integers already -- it is the one
// part of the generator that was portable from the start -- so this is the
// same multiply and shift chain with $D770 doing the multiplies.
static uint32_t hash32(uint16_t x, uint16_t y, uint32_t salt)
{
  uint32_t h;

  MATH.multina32 = x;
  MATH.multinb32 = 0x1F1F1F1FUL;
  h = MATH.multout32;
  MATH.multina32 = y;
  MATH.multinb32 = 0x2545F491UL;
  h ^= MATH.multout32;
  h ^= salt;
  h ^= h >> 13;
  MATH.multina32 = h;
  MATH.multinb32 = 0x5BD1E995UL;
  h = MATH.multout32;
  return h ^ (h >> 15);
}

// Everything that is per octave rather than per pixel. **The draws happen
// here, in genmap.py's order** -- salt, then the y offset, then the x offset,
// octave by octave -- because the sequence of calls is the whole of what
// reproduces a map.
void noise_init(void)
{
  uint16_t off = 0, wo = 0;
  uint32_t amp = ONE;
  uint32_t weight = 0;
  uint8_t o;

  rnd_seed(SEED);

  for (o = 0; o < OCT; o++) {
    uint16_t period = (uint16_t)BASE << o;
    uint16_t step = SIZE / period;
    uint32_t salt;
    uint8_t sh;
    uint16_t x, y, i;

    for (sh = 0; (uint16_t)(1 << sh) < step; sh++)
      ;
    lshift[o] = sh;
    coff[o] = off;

    salt = rnd_next();
    i = 0;
    for (y = 0; y < period; y++) {
      for (x = 0; x < period; x++) {
        corner[off + i] = (uint16_t)hash32(x, y, salt);
        i++;
      }
    }
    off = (uint16_t)(off + i);

    offy[o] = rnd_below(SIZE);
    offx[o] = rnd_below(SIZE);

    woff[o] = wo;
    for (x = 0; x < step; x++)
      wtab[wo + x] = smoothstep16((uint16_t)(x << (FRACBITS - sh)));
    wo = (uint16_t)(wo + step);

    e_top[o] = work.edge[0][o];
    e_bot[o] = work.edge[1][o];
    e_row[o] = E_NONE;

    amps[o] = amp;
    weight += amp;
    amp = mulhi(amp, GAIN);
  }

  weight_recip = recip32(weight);
}

// One lattice row of an octave, interpolated along x into `dst`: the `top` or
// `bot` of the two an output row sits between. Rebuilt only when the output
// row crosses into a new lattice cell, which is every `step` rows.
static void edge_build(uint8_t o, uint16_t row, uint16_t *dst)
{
  uint16_t period = (uint16_t)BASE << o;
  uint8_t sh = lshift[o];
  uint16_t pmask = period - 1;
  uint16_t stepmask = (uint16_t)((1 << sh) - 1);
  uint16_t ox = offx[o];
  const uint16_t *c = corner + coff[o] + row * period;
  const uint16_t *w = wtab + woff[o];
  uint16_t x;

  for (x = 0; x < SIZE; x++) {
    uint16_t sx = (uint16_t)(x - ox) & SIZE_MASK;
    uint16_t i0 = sx >> sh;

    dst[x] = lerp16(c[i0], c[(uint16_t)(i0 + 1) & pmask], w[sx & stepmask]);
  }
}

// Build the field into attic RAM and return a checksum of it.
//
// Row by row across all four octaves, then normalise and store: the field is
// touched in attic RAM exactly once, on the way out, and the octaves are
// summed in a chip RAM row buffer. Writing straight up there with the CPU is
// deliberate too -- a posted attic write costs +3 cycles where a DMA out of a
// row buffer costs 9.54 a byte, so buffering and blitting would be three
// times the price of storing each value as it is computed.
uint32_t noise_run(void)
{
  uint16_t x, y;

  // Once, and only here: noise_store clears each row as it reads it, so from
  // the second row on the accumulator arrives empty.
  for (x = 0; x < SIZE; x++)
    acc[x] = 0;
  nz_sum_a = 0;
  nz_sum_b = 0;
  nz_recip = weight_recip;

  for (y = 0; y < SIZE; y++) {
    uint8_t o;

    for (o = 0; o < OCT; o++) {
      uint16_t period = (uint16_t)BASE << o;
      uint8_t sh = lshift[o];
      uint16_t pmask = period - 1;
      uint16_t stepmask = (uint16_t)((1 << sh) - 1);
      uint16_t sy = (uint16_t)(y - offy[o]) & SIZE_MASK;
      uint16_t iy0 = sy >> sh;
      uint16_t wy = wtab[woff[o] + (sy & stepmask)];
      const uint16_t *top, *bot;
      uint32_t amp = amps[o];

      // The lattice row the output row sits on. When it moves it moves by
      // exactly one -- sy walks up by a row at a time -- so the new top is the
      // bot that is already built, and one rebuild covers the change. The
      // general path is there because a wrong assumption here would be a
      // wrong map rather than a crash.
      if (e_row[o] != iy0) {
        uint16_t next = (uint16_t)(iy0 + 1) & pmask;

        if (e_row[o] != E_NONE
            && ((uint16_t)(e_row[o] + 1) & pmask) == iy0) {
          uint16_t *swap = e_top[o];

          e_top[o] = e_bot[o];
          e_bot[o] = swap;
        } else {
          edge_build(o, iy0, e_top[o]);
        }
        edge_build(o, next, e_bot[o]);
        e_row[o] = iy0;
      }

      top = e_top[o];
      bot = e_bot[o];

      nz_top = top;
      nz_bot = bot;
      nz_acc = acc;
      nz_wy = wy;
      nz_amp = amp;
      noise_blend();
    }

    nz_acc = acc;
    nz_out = (uint8_t __far *)(NOISE_FIELD + (uint32_t)y * (SIZE * 2));
    noise_store();
  }

  return (uint32_t)nz_sum_b << 16 | nz_sum_a;
}

// **A note for whoever ports `stretch` next.** It ends with
// `np.clip(..., 0, ONE)`, and ONE is 65536, which does not fit the uint16 a
// field value is stored in -- the top half per cent of the map lands exactly
// there. tools/fixed.py's own rule says ONE is only ever an intermediate, so
// the fix is probably to clip to 65535 on both sides rather than to widen the
// field; but it re-rolls every map by up to one part in 65536, so it is a
// decision to take deliberately and to check the PNGs against, not a detail
// to settle in the C.

// --- the percentile stretch ------------------------------------------------
//
// `stretch` from tools/genmap.py: rescale the field so that its 0.5th
// percentile is 0 and its 99.5th is 1.0, both found by histogram because the
// machine cannot sort a quarter of a million values -- and neither would want
// to, since a histogram gives both cut points in one pass.
//
// **This is the "measure" of build, measure, paint**, and it is why the
// pipeline cannot be one streaming pass: nothing can be painted until the
// whole field has been looked at. Three passes over the field is the shape,
// and this is the second and third of them.

// The value num/den of the way up the field, in Q0.16. Interpolating inside
// the bucket is not a nicety: this number scales the whole field, so a bucket
// of error moves every pixel. One divide, and only twice a map.
static uint32_t percentile(uint32_t want)
{
  uint32_t cum = 0, below = 0, inside;
  uint16_t b;

  for (b = 0; b < BUCKETS; b++) {
    below = cum;
    cum += work.hist[b];
    if (cum >= want)
      break;
  }

  inside = work.hist[b];
  return ((uint32_t)b << BUCKETSHIFT)
       + (inside ? ((want - below) << BUCKETSHIFT) / inside : 0);
}

uint32_t noise_stretch(void)
{
  uint32_t lo, hi, span;
  uint16_t y, b;

  for (b = 0; b <= BUCKETS; b++)
    work.hist[b] = 0;

  nz_hist = work.hist;
  for (y = 0; y < SIZE; y++) {
    nz_out = (uint8_t __far *)(NOISE_FIELD + (uint32_t)y * (SIZE * 2));
    stretch_hist();
  }

  lo = percentile((uint32_t)SIZE * SIZE * 5 / 1000);
  hi = percentile((uint32_t)SIZE * SIZE * 995 / 1000);
  span = hi > lo ? hi - lo : 1;

  nz_lo = (uint16_t)lo;
  nz_recip = recip32(span);
  nz_sum_a = 0;
  nz_sum_b = 0;

  for (y = 0; y < SIZE; y++) {
    nz_out = (uint8_t __far *)(NOISE_FIELD + (uint32_t)y * (SIZE * 2));
    stretch_apply();
  }

  return (uint32_t)nz_sum_b << 16 | nz_sum_a;
}
