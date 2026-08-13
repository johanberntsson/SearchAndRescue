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
#include "../loader.h"     /* LOW_FREE */
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

// TYPES["island"] in tools/genmap.py: where the type's terrain starts and how
// much of the range it uses, as int(fraction * ONE). The island is not ridged,
// so the fold genmap.py applies between the stretch and these is absent here
// -- a mountains map would need it, and it goes in when a map file is read
// rather than hard-coded.
#define FLOOR     13107UL   // 0.20
#define RANGE     52428UL   // 0.80

// The island mask: ISLAND_EDGE, ISLAND_FADE and ISLAND_WOBBLE from genmap.py,
// as int(fraction * ONE). Full height inside EDGE - FADE of the centre,
// nothing past EDGE, and the coastline pushed about by low-frequency noise so
// that it is not a circle.
#define ISL_EDGE   56360UL  // 0.86
#define ISL_FADE   30146UL  // 0.46, used as its reciprocal
#define ISL_WOBBLE  8519UL  // 0.13

// The square root, as a 257-entry table read at eight bits with the rest
// interpolated -- the one per-pixel root in the generator.
//
// **It is derived at startup, not shipped.** SQRT[i] is round(sqrt(i/256) *
// ONE), which is round(sqrt(i << 24)), so a digit-by-digit integer root gets
// every entry exactly right: checked against tools/fixed.py's table, all 256
// agree. That is 512 bytes of program the disk does not carry, for a few
// thousand cycles once.
//
// The deltas are kept beside it so the read is one multiply and no subtract,
// and because SQRT[256] is ONE and does not fit the sixteen bits the table is
// stored in -- as a delta from SQRT[255] it is 128.
LOW_FREE static uint16_t sqrt_tab[256];
LOW_FREE static uint16_t sqrt_delta[256];

// dx*dx for every column offset, so the radius needs no multiply per pixel.
// The axis is (i * 2 - size) * ONE / size, which for a 512-wide map is exactly
// (i - 256) * 256 -- so r2, which genmap.py writes as (ax^2 + ay^2) >> 16, is
// just dx^2 + dy^2 with dx and dy in cells. No shifts, no rounding.
//
// **Sixteen bits, and only 256 entries, which is a decision and not a
// saving.** An offset of 256 squares to 65536, one past what a word holds --
// but a pixel that far off the axis is outside the unit circle whatever the
// other axis does, so the mask is zero there and the entry would never be
// used. The row and the column at that offset are answered before the table
// is reached. The alternative, 257 four-byte entries, cost twice the space
// and put a 32-bit indexed store into this section, which is where an hour
// went: it wrote every entry one byte high and read back as i*i << 8.
LOW_FREE static uint16_t sq_tab[SIZE / 2];

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
__zpage uint16_t nz_sum_a, nz_sum_b, nz_lo, nz_ptr, nz_floor, nz_range;

// ... and the mask pass's.
uint16_t *__attribute__((zpage)) nz_sqrt;
uint16_t *__attribute__((zpage)) nz_delta;
uint32_t *__attribute__((zpage)) nz_sq;
__zpage uint16_t nz_dy2;
__zpage uint16_t nz_edge, nz_wobble;
__zpage uint8_t nz_neg;

void noise_blend(void);
void noise_store(void);
void stretch_hist(void);
void stretch_apply(void);
void mask_row(void);

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
// `stretch` from tools/genmap.py, and `base_terrain`'s floor and range with
// it: rescale the field so that its 0.5th percentile is 0 and its 99.5th is
// 1.0 -- both found by histogram, because the machine cannot sort a quarter of
// a million values and would not want to, since a histogram gives both cut
// points in one pass -- and then put it on the type's own elevation range.
//
// **The floor and range are folded into the paint pass** rather than given one
// of their own. They are a per-pixel function of one value, and a pass over the
// field costs about a second before it does any arithmetic.
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
  nz_floor = FLOOR;
  nz_range = RANGE;
  nz_sum_a = 0;
  nz_sum_b = 0;

  for (y = 0; y < SIZE; y++) {
    nz_out = (uint8_t __far *)(NOISE_FIELD + (uint32_t)y * (SIZE * 2));
    stretch_apply();
  }

  return (uint32_t)nz_sum_b << 16 | nz_sum_a;
}

// --- the island mask -------------------------------------------------------
//
// `island_mask` from tools/genmap.py: a radial falloff with a noisy coastline,
// multiplied into the terrain. It is the last of base_terrain, and the only
// pass so far that needs a square root per pixel.

// Exact integer square root, digit by digit -- fixed.py's isqrt. No multiply
// and no divide, one shift and a conditional subtract per output bit, which is
// as 6502 as arithmetic gets. Used here only to build the table, 256 times.
static uint32_t isqrt32(uint32_t n)
{
  uint32_t root = 0, bit = 1UL << 30;

  while (bit) {
    uint32_t step = root + bit;

    if (n >= step) {
      n -= step;
      root = (root >> 1) + bit;
    } else {
      root >>= 1;
    }
    bit >>= 2;
  }
  return root;
}

// The mask's own lattice: one value-noise octave at the map's coarsest period,
// drawn from the salt that follows the terrain's. **The draw order is the
// whole of what reproduces a map**, so this must happen after noise_init's
// twelve and before anything else asks the stream for anything.
#define MASK_PERIOD BASE
#define MASK_STEP   (SIZE / MASK_PERIOD)

static uint16_t mask_corner[MASK_PERIOD * MASK_PERIOD];
static uint16_t *m_top, *m_bot;
static uint16_t m_row;

static void mask_edge(uint16_t row, uint16_t *dst)
{
  const uint16_t *c = mask_corner + row * MASK_PERIOD;
  const uint16_t *w = wtab + woff[0];   // octave 0's period is the mask's
  uint8_t sh = lshift[0];
  uint16_t stepmask = (uint16_t)((1 << sh) - 1);
  uint16_t x;

  for (x = 0; x < SIZE; x++) {
    uint16_t i0 = x >> sh;              // no roll: the mask's noise is unshifted

    dst[x] = lerp16(c[i0], c[(i0 + 1) & (MASK_PERIOD - 1)], w[x & stepmask]);
  }
}

void mask_init(void)
{
  uint32_t salt = rnd_next();
  uint16_t i, x, y;

  for (i = 0; i < 256; i++) {
    uint32_t n = (uint32_t)i << 24;
    uint32_t r = isqrt32(n);

    // round rather than floor: an exact half cannot occur, since (r + 0.5)^2
    // is never a whole number, so this is np.round without its tie rule.
    sqrt_tab[i] = (uint16_t)((n - r * r) > r ? r + 1 : r);
  }
  for (i = 0; i < 255; i++)
    sqrt_delta[i] = (uint16_t)(sqrt_tab[i + 1] - sqrt_tab[i]);
  sqrt_delta[255] = (uint16_t)(ONE - sqrt_tab[255]);

  for (i = 0; i < SIZE / 2; i++)
    sq_tab[i] = (uint16_t)(i * i);

  i = 0;
  for (y = 0; y < MASK_PERIOD; y++)
    for (x = 0; x < MASK_PERIOD; x++)
      mask_corner[i++] = (uint16_t)hash32(x, y, salt);

  m_top = work.edge[0][0];
  m_bot = work.edge[1][0];
  m_row = E_NONE;
}

uint32_t mask_apply(void)
{
  uint16_t y;

  nz_sum_a = 0;
  nz_sum_b = 0;
  nz_sqrt = sqrt_tab;
  nz_delta = sqrt_delta;
  nz_edge = ISL_EDGE;
  nz_wobble = ISL_WOBBLE;
  nz_recip = recip32(ISL_FADE);

  for (y = 0; y < SIZE; y++) {
    uint16_t iy0 = y >> lshift[0];
    int16_t dy = (int16_t)y - SIZE / 2;

    // The one row whose distance from the axis squares past a word. It is
    // outside the circle from end to end, so it is zeros -- written here
    // rather than given a branch in the inner loop.
    if (dy <= -SIZE / 2) {
      uint16_t __far *row =
          (uint16_t __far *)(NOISE_FIELD + (uint32_t)y * (SIZE * 2));
      uint16_t x;

      for (x = 0; x < SIZE; x++) {
        row[x] = 0;
        nz_sum_b = (uint16_t)(nz_sum_b + nz_sum_a);
      }
      continue;
    }

    if (m_row != iy0) {
      uint16_t next = (uint16_t)(iy0 + 1) & (MASK_PERIOD - 1);

      if (m_row != E_NONE) {
        uint16_t *swap = m_top;

        m_top = m_bot;
        m_bot = swap;
      } else {
        mask_edge(iy0, m_top);
      }
      mask_edge(next, m_bot);
      m_row = iy0;
    }

    nz_top = m_top;
    nz_bot = m_bot;
    nz_wy = wtab[woff[0] + (y & ((1 << lshift[0]) - 1))];
    nz_dy2 = sq_tab[dy < 0 ? -dy : dy];
    nz_out = (uint8_t __far *)(NOISE_FIELD + (uint32_t)y * (SIZE * 2));
    nz_sq = sq_tab;
    mask_row();
  }

  return (uint32_t)nz_sum_b << 16 | nz_sum_a;
}


// --- hills -----------------------------------------------------------------
//
// `add_hills` from tools/genmap.py: a few domed bumps dropped on dry land,
// roughened by a fine noise field.
//
// **This one is C, and that is not a lapse.** The rule the port has been
// following -- structural work in C, per-pixel loops in assembly -- is about
// loops over the *field*. Six hills of radius 12 is 3750 pixels against the
// quarter of a million every earlier pass touched, so the whole thing costs
// less than a tenth of a second even at the compiler's usual rate, and the
// arithmetic is fiddly enough to be worth writing in the language that can be
// read.
//
// What is delicate here is not speed but **the draw order**. Hills are placed
// by rejection: a position is drawn, and if it lands in the sea it is thrown
// away and another is drawn. So the number of values taken from the stream
// depends on the terrain -- which is fine, and only fine because the terrain
// is already identical to the PC's. The first pass whose *stream* position is
// data-dependent.
#define HILL_COUNT   6
#define HILL_RADIUS  12
#define HILL_HEIGHT  13107UL   // 0.25 of the type's range
#define HILL_ROUGH   26214UL   // 0.4: how far the texture may vary a dome
#define SEA          17039UL   // TYPES["island"]["sea"], 0.26
#define TEX_OCTAVE   (OCT - 1) // the texture's period is lattice << 3, which
#define TEX_PERIOD   (BASE << 3)  // ... is the finest octave's

static uint16_t *tex_corner;   // 32x32, in the work union: see mask_init

// **The dome is the same shape every time**, so its profile is worked out once
// rather than six times: the distance, its exact root, the divide by the
// radius and the smoothstep all depend on the offset from the centre and
// nothing else. Only the texture and the ground underneath differ per hill.
// A pixel outside the disc is stored as zero and added anyway -- which is what
// genmap.py does, since it scales the whole box by the texture and adds it.
// It lives in the work union too, past the texture's lattice: 625 entries
// against the 2048 bytes tex_corner takes of the 8192 there. Nothing else is
// live while hills are being stamped.
#define HILL_SPAN (HILL_RADIUS * 2 + 1)
static uint16_t *hill_profile;

static uint16_t texture_at(uint16_t y, uint16_t x)
{
  uint8_t sh = lshift[TEX_OCTAVE];
  uint16_t stepmask = (uint16_t)((1 << sh) - 1);
  const uint16_t *w = wtab + woff[TEX_OCTAVE];
  uint16_t iy0 = y >> sh, ix0 = x >> sh;
  uint16_t iy1 = (uint16_t)(iy0 + 1) & (TEX_PERIOD - 1);
  uint16_t ix1 = (uint16_t)(ix0 + 1) & (TEX_PERIOD - 1);
  const uint16_t *c0 = tex_corner + (uint16_t)iy0 * TEX_PERIOD;
  const uint16_t *c1 = tex_corner + (uint16_t)iy1 * TEX_PERIOD;
  uint16_t wx = w[x & stepmask];

  return lerp16(lerp16(c0[ix0], c0[ix1], wx),
                lerp16(c1[ix0], c1[ix1], wx), w[y & stepmask]);
}

// smoothstep over a domain that includes 1.0 exactly. The hill's centre is
// smoothstep(ONE), which is ONE, and neither fits the sixteen bits the field
// itself is stored in.
static uint32_t smoothstep32(uint32_t t)
{
  return mulhi32(mulhi32(t, t), 3UL * ONE - 2UL * t);
}

static uint16_t field_get(uint16_t y, uint16_t x)
{
  const uint16_t __far *row =
      (const uint16_t __far *)(NOISE_FIELD + (uint32_t)y * (SIZE * 2));

  return row[x];
}

static void hill_profile_build(void)
{
  int16_t dy, dx;
  uint16_t i = 0;

  for (dy = -HILL_RADIUS; dy <= HILL_RADIUS; dy++) {
    for (dx = -HILL_RADIUS; dx <= HILL_RADIUS; dx++) {
      // The distance is exact -- the digit-by-digit root, not the table --
      // because this is where a rounding error becomes a stair on a hillside.
      // Measured at eight fractional bits and shifted up afterwards, which is
      // what keeps isqrt inside its 32-bit domain.
      uint32_t dd = (uint32_t)(dy * dy + dx * dx) << 16;
      uint32_t d = (isqrt32(dd) << 8) / HILL_RADIUS;

      hill_profile[i++] = d > ONE
                        ? 0
                        : mulhi(smoothstep32(ONE - d), HILL_HEIGHT);
    }
  }
}

static void hill_stamp(uint16_t cy, uint16_t cx)
{
  int16_t dy, dx;
  uint16_t i = 0;

  for (dy = -HILL_RADIUS; dy <= HILL_RADIUS; dy++) {
    uint16_t yy = (uint16_t)(cy + dy) & SIZE_MASK;
    uint16_t __far *row =
        (uint16_t __far *)(NOISE_FIELD + (uint32_t)yy * (SIZE * 2));

    for (dx = -HILL_RADIUS; dx <= HILL_RADIUS; dx++) {
      uint16_t patch = hill_profile[i++];
      uint16_t xx;
      uint32_t factor;

      if (!patch)
        continue;

      xx = (uint16_t)(cx + dx) & SIZE_MASK;
      factor = ONE - HILL_ROUGH
             + 2UL * mulhi(texture_at(yy, xx), HILL_ROUGH);
      row[xx] = (uint16_t)(row[xx] + mulhi(patch, factor));
    }
  }
}

// The field's Fletcher checksum, read back out of attic RAM. **Verification
// scaffolding**, not part of generating a map: it is a quarter of a million
// far reads and costs about three seconds, which is more than every pass it
// checks. The passes that write the field checksum it as they go and pay
// nothing; this exists for the ones that do not, and goes when the pipeline
// is finished.
uint32_t field_checksum(void)
{
  uint16_t a = 0, b = 0;
  uint16_t x, y;

  for (y = 0; y < SIZE; y++) {
    const uint16_t __far *row =
        (const uint16_t __far *)(NOISE_FIELD + (uint32_t)y * (SIZE * 2));

    for (x = 0; x < SIZE; x++) {
      a = (uint16_t)(a + row[x]);
      b = (uint16_t)(b + a);
    }
  }
  return (uint32_t)b << 16 | a;
}

void hills_apply(void)
{
  uint32_t salt = rnd_next();
  uint16_t i, x, y, placed = 0;

  tex_corner = work.edge[0][0];      // 32 x 32, the first 2 KB
  hill_profile = work.edge[0][2];    // and the dome past it
  i = 0;
  for (y = 0; y < TEX_PERIOD; y++)
    for (x = 0; x < TEX_PERIOD; x++)
      tex_corner[i++] = (uint16_t)hash32(x, y, salt);

  hill_profile_build();

  for (i = 0; i < HILL_COUNT * 8 && placed < HILL_COUNT; i++) {
    uint16_t cy = rnd_below(SIZE);
    uint16_t cx = rnd_below(SIZE);

    if (field_get(cy, cx) <= SEA)
      continue;
    hill_stamp(cy, cx);
    placed++;
  }
}

// --- water, part one: where a lake could go --------------------------------
//
// `local_minima` and the median cut from `fill_lakes`: the cells no higher
// than any of their eight neighbours and not already wet, then the lower half
// of those by height.
//
// **Two passes, because the median cannot be known until every minimum has
// been seen** -- the same build-measure-paint shape the stretch has, and for
// the same reason. The first counts them into the histogram; the second keeps
// the ones under the cut. Neither stores every minimum, which is what makes
// the memory affordable: the island has sixteen of them and eight candidates,
// so the list is bytes rather than the tens of thousands the costing feared.
//
// A three-row window in chip RAM is what makes this bearable at all. The eight
// neighbours of every cell would otherwise be eight reads out of attic RAM,
// which is the expensive direction; streamed a row at a time they are chip
// reads, and the field is touched once.
// The island has sixteen minima and eight candidates. 128 is a cap with room
// to spare rather than a budget, and a map that overran it would lose the
// tail of its candidate list, not break -- but it would stop matching the PC,
// which is what the checksum is for.
#define MINIMA_MAX 128

static uint16_t cand_y[MINIMA_MAX], cand_x[MINIMA_MAX];
static uint16_t cand_n;

// The three rows, and which of them is which. Indexed by (y + 2) % 3 so that
// stepping down the map is one row read and a rotation.
static uint16_t *win[3];

static void win_load(uint8_t slot, uint16_t y)
{
  const uint16_t __far *src =
      (const uint16_t __far *)(NOISE_FIELD + (uint32_t)y * (SIZE * 2));
  uint16_t *dst = win[slot];
  uint16_t x;

  for (x = 0; x < SIZE; x++)
    dst[x] = src[x];
}

// Is (x) of the middle row a minimum? `up`, `mid` and `dn` are the three rows.
static uint8_t is_min(const uint16_t *up, const uint16_t *mid,
                      const uint16_t *dn, uint16_t x)
{
  uint16_t v = mid[x];
  uint16_t l = (uint16_t)(x - 1) & SIZE_MASK;
  uint16_t r = (uint16_t)(x + 1) & SIZE_MASK;

  return v <= up[l] && v <= up[x] && v <= up[r]
      && v <= mid[l] && v <= mid[r]
      && v <= dn[l] && v <= dn[x] && v <= dn[r];
}

// Walk the field with the window, calling back on every dry minimum. `keep`
// says whether to record it or only to count it.
static void minima_scan(uint8_t keep, uint16_t cut)
{
  uint16_t y, x;

  win_load(0, SIZE - 1);      // the row above row 0, which wraps
  win_load(1, 0);
  win_load(2, 1);

  for (y = 0; y < SIZE; y++) {
    const uint16_t *up = win[(y + 0) % 3];
    const uint16_t *mid = win[(y + 1) % 3];
    const uint16_t *dn = win[(y + 2) % 3];

    for (x = 0; x < SIZE; x++) {
      uint16_t v = mid[x];

      if (v <= SEA || !is_min(up, mid, dn, x))
        continue;
      if (keep) {
        if (v <= cut && cand_n < MINIMA_MAX) {
          cand_y[cand_n] = y;
          cand_x[cand_n] = x;
          cand_n++;
        }
      } else {
        work.hist[v >> BUCKETSHIFT]++;
      }
    }

    // the window rolls: the row two below the new middle, wrapping
    win_load((uint8_t)((y + 0) % 3), (uint16_t)(y + 2) & SIZE_MASK);
  }
}

uint32_t minima_find(void)
{
  uint32_t cum = 0;
  uint16_t b, half = 0, total = 0;
  uint16_t a = 0, sum_b = 0, i;

  // The window shares the work union with the histogram, which is the same
  // 1025 four-byte entries the stretch used -- 4100 bytes, so it runs four
  // bytes past the first half of the union. The window starts a row further on
  // than that, which is why these are [1], [2] and [3] and not [0], [1], [2].
  win[0] = work.edge[1][1];
  win[1] = work.edge[1][2];
  win[2] = work.edge[1][3];

  for (b = 0; b <= BUCKETS; b++)
    work.hist[b] = 0;

  minima_scan(0, 0);
  for (b = 0; b <= BUCKETS; b++)
    total = (uint16_t)(total + work.hist[b]);

  {
    uint16_t want = total / 2;

    if (want < 3)             // COUNTS["few"]; a map must get its lakes
      want = 3;
    for (b = 0; b <= BUCKETS; b++) {
      cum += work.hist[b];
      if (cum >= want)
        break;
    }
    half = b;
  }

  cand_n = 0;
  minima_scan(1, (uint16_t)(half << BUCKETSHIFT));

  for (i = 0; i < cand_n; i++) {
    a = (uint16_t)(a + cand_y[i]);
    sum_b = (uint16_t)(sum_b + a);
    a = (uint16_t)(a + cand_x[i]);
    sum_b = (uint16_t)(sum_b + a);
  }
  return (uint32_t)sum_b << 16 | a;
}
