// Stage two: colour, and the planes the renderer reads.
//
// **Why this is its own program.** Stage one's 32 KB is full -- 27.5 of text
// alone -- and colour would not fit beside it. The costing has always had the
// answer in reserve: "the generator splits again -- terrain, then colour, then
// chain to the game". Nothing had to be invented for it. Attic RAM survives a
// program load, so the four fields stage one leaves are simply still there;
// the handover is the same keyboard-queue trick, done twice now instead of
// once; and this program inherits a fresh 32 KB and its own 12 under the
// banked-out ROMs.
//
// **What it does have to inherit is the random stream.** The whole of what
// reproduces a map is the sequence of draws, so the two dithers below cannot
// start a stream of their own -- stage one leaves its state at HANDOVER_RND
// and this carries on from there.

#include <stdint.h>

#include "../dma.h"
#include "../mapgen/fields.h"
#include "../mapgen/fixed.h"
#include "../mapgen/tables.h"
#include "colour.h"

// --- colour ----------------------------------------------------------------
//
// `colourise` from tools/genmap.py: the last pass, and the one the costing
// puts at 250-350 cycles a pixel.
//

// --- what stage two needs of its own ---------------------------------------
//
// The two programs share no code beyond fixed.h and the random stream: stage
// one's helpers live inside a file that is already too big to link here. These
// are the few it wants, in the smallest form that serves one caller each.

// **The buffers go under the banked-out BASIC ROM, as stage one's do.** Stage
// two shares `kernal.s`, so it has been banking BASIC out since it was written
// -- it simply never claimed the 12 KB underneath, because it fitted the
// stock 32 KB without it. Turning cross-calling off cost it that fit, and this
// is the cheaper half of the trade: only BSS moves up, so nothing has to be
// written above $9FFF by a loader with the ROM still mapped.
//
// buf and work come to 11268 bytes of the 12288. The two square-root tables
// were up here too and put it four bytes over, which is the sort of margin
// that says do not add a third tenant without measuring.
#define HIGH_BSS __attribute__((section("highbss")))

// Six rows of the field and a row of finished indices. The three-row window is
// for the neighbours the slope and the sun need.
HIGH_BSS static uint16_t buf[7][SIZE];

// The dither lattices are 32 across, so a cell is 16 pixels and the weight is
// smoothstep of a sixteenth -- sixteen entries, not the 512 a row would need.
#define DITHER_SH   4
#define MOTTLE_SH   5                     // MOTTLE_PER is 32 at SIZE 512
#define MOTTLE_PER  (1 << MOTTLE_SH)
#define LATTICE     (MOTTLE_PER * MOTTLE_PER)

#if MOTTLE_PER != SIZE / 16
#error "the dither lattice is SIZE/16 across; fix MOTTLE_SH"
#endif

static uint16_t dwt[1 << DITHER_SH];

// **The histogram and the two dither lattices are the same 4 KB**, because
// they are never both alive: the histogram is finished the moment the ramp's
// top is known, and the lattices are hashed after that. It is not a space
// trick -- the program has room -- it is what lets the lattices be *near*.
// They were far pointers in bank 1 and the pixel loop read them eight times a
// pixel, at the flat ~15 cycles an attic read costs over a chip one.
HIGH_BSS static union {
  uint32_t hist[BUCKETS + 1];
  uint16_t lattice[2][LATTICE];
} work;

// fixed.sqrt's table, derived here as stage one derives it: round(sqrt(i/256)
// * ONE) is round(sqrt(i << 24)), so an exact integer root gets every entry
// right. The deltas sit beside it so a read is one multiply.
static uint16_t sqrt_tab[256], sqrt_delta[256];

static void row_in(uint16_t *dst, uint32_t src)
{
  dma_copy(src, (uint32_t)(uint16_t)dst, SIZE * 2);
}

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

// The value num/den of the way up whatever is in the histogram, interpolated
// inside the bucket it lands in.
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

static void tables_build(void)
{
  uint16_t i;

  for (i = 0; i < 256; i++) {
    uint32_t n = (uint32_t)i << 24;
    uint32_t r = isqrt32(n);

    sqrt_tab[i] = (uint16_t)((n - r * r) > r ? r + 1 : r);
  }
  for (i = 0; i < 255; i++)
    sqrt_delta[i] = (uint16_t)(sqrt_tab[i + 1] - sqrt_tab[i]);
  sqrt_delta[255] = (uint16_t)(ONE - sqrt_tab[255]);

  for (i = 0; i < (1 << DITHER_SH); i++)
    dwt[i] = smoothstep16((uint16_t)(i << (FRACBITS - DITHER_SH)));
}

#define SHADES     6
#define RAMP_LEN   21
#define RAMP_BASE  24
#define DEEP_SH    3
#define DEEP_LEN   (1 << DEEP_SH)
#define DEEP_BASE  16
#define STONE_LEN  2
#define STONE_BASE 150

#define CEILING    60293UL   // TYPES["island"], 0.92
#define SLOPE_REF  4587UL    // 0.07 at feature 1.0
#define SLOPE_PUSH 14417UL   // 0.22
#define MOTTLE     91750UL   // 1.4 -- past ONE, so a 32-bit operand
#define SUN_MOTTLE 36044UL   // 0.55
#define SUN_REF    1310UL    // 0.02
#define DEPTH_RCP  655420UL  // ONE * ONE / (0.1 * ONE)
#define GAMMA_TOP  2
#define GAMMA_SH   1        // ... and dividing by it is a shift

// **The pixel loop's working set, in zero page.** These were locals of
// `colour_build`, which puts them on Calypsi's software stack: every `mid[x]`
// then loads a pointer through the stack base before it can index at all, and
// there are seven such rows and four lattice pointers read on every pixel.
// The renderer's `vx_*` block is here for exactly this reason and stage one's
// `nz_*` block after it; this is the third time it has been worth it.
//
// 36 bytes of the 126 the linker is allowed, and `zp_preserve` in
// src/mapgen/kernal.s carries whatever is in `zzpage` across the handover, so
// growing this does not put BASIC at risk.
// `__zpage` after the `*` does not parse; the attribute form does, and it is
// the one the manual uses for "a pointer *stored in* zero page" as against
// "a pointer *to* zero page", which is what the keyword before the type means.
#define ZP __attribute__((zpage))

uint16_t *ZP cl_up;
uint16_t *ZP cl_mid;
uint16_t *ZP cl_dn;
uint16_t *ZP cl_lv;
uint16_t *ZP cl_bt;
uint16_t *ZP cl_bed;
uint8_t *ZP cl_out;
const uint16_t *ZP cl_m0;
const uint16_t *ZP cl_m1;
const uint16_t *ZP cl_s0;
const uint16_t *ZP cl_s1;
__zpage uint16_t cl_wy;

// (ONE * ONE) / SLOPE_REF and the same for SUN_REF and for the ramp's top,
// worked out once rather than per pixel. See sunlight_at for what the second
// one replaces. Up here too: each is one operand of a multiply per pixel.
__zpage uint32_t recip_slope, recip_sun, recip_top;

// **The dither is in assembly now**; see src/mapgen2/dither_asm.s for what it
// does and why. These are its half of the contract: the lattice columns as
// byte offsets, the x weight, the two amplitudes, the two signed results, and
// the scratch it works in. Declared here because zero page is allocated by the
// C compiler and the assembler only externs what it is given.
extern void cl_dither(void);

__zpage uint8_t cl_ox0, cl_ox1;
__zpage uint16_t cl_wx;
__zpage uint16_t cl_p0, cl_p1, cl_a, cl_b, cl_w, cl_r, cl_t0;
__zpage uint32_t cl_v;

uint32_t cl_amps[2];
int32_t cl_dith[2];

// The C version it replaces, kept for reference and no longer called: it is
// what dither_asm.s was written from, and the thing to read first if the
// colour checksum ever moves.
#if 0
// value noise at MOTTLE_PER, scaled and signed: scale(2n - ONE, amp), the same
// shape the mask's wobble and the river's meander take.
// **Everything the row already knows is passed in.** `c0`/`c1` are the two
// lattice rows and `wy` their weight, which change once per map row; `ix0`,
// `ix1` and `wx` change once per pixel but are the *same* for both lattices,
// so the pixel loop works them out once and both calls take them. What was
// left inside was two shifts, two masks, two adds and a table read, done twice
// a pixel for a number that was already in hand.
//
// The interpolation order is x first and then y, which is the order
// `value_noise` in tools/genmap.py takes. Bilinear in fixed point is not
// order-independent -- the roundings differ -- so this cannot be turned inside
// out to hoist more of it, however much it looks like it could.
static int32_t dither_at(const uint16_t *c0, const uint16_t *c1, uint16_t wy,
                         uint16_t ix0, uint16_t ix1, uint16_t wx, uint32_t amp)
{
  uint16_t n = lerp16(lerp16(c0[ix0], c0[ix1], wx),
                      lerp16(c1[ix0], c1[ix1], wx), wy);
  uint32_t mag, v;

  // The magnitude of 2n - ONE, which is what genmap.py scales by `amp`.
  mag = n >= 0x8000 ? (uint32_t)(n - 0x8000) << 1
                    : ((uint32_t)0x8000 - n) << 1;
  MATH.multina32 = mag;
  MATH.multinb32 = amp;
  // Bytes 2..5 in one read. The top byte of that window is always zero here --
  // mag is at most ONE and amp at most MOTTLE, so the shifted product is
  // eighteen bits -- which is why this is the same number the old three-byte
  // assembly of it produced.
  v = MATH_OUT_H32;
  if (n >= 0x8000)
    return (int32_t)v;
  // numpy floors a negative product, so any bits the shift discarded round the
  // magnitude *up* before it is negated.
  if (MATH_OUT16)
    v++;
  return -(int32_t)v;
}
#endif

// A 257-entry table read at eight bits with the rest interpolated, which is
// fixed.lookup. The gamma's entries pass ONE, so this one is 32-bit
// throughout.
static uint32_t lookup32(const uint32_t *tab, uint32_t x)
{
  uint16_t i, w;
  uint32_t lo, hi;

  if (x > ONE)
    x = ONE;
  i = (uint16_t)(x >> 8);
  w = (uint16_t)(x & 0xFF);                 // the weight is one byte, shifted
  w <<= 8;                                  // up in a word rather than in four
  lo = tab[i];
  hi = tab[i < 256 ? i + 1 : 256];
  return lo + mulhi32(hi - lo, w);
}

static uint16_t lookup16(const uint16_t *tab, uint32_t x)
{
  uint16_t i, w;

  if (x > ONE)
    x = ONE;
  i = (uint16_t)(x >> 8);
  w = (uint16_t)(x & 0xFF);                 // the weight is one byte, shifted
  w <<= 8;                                  // up in a word rather than in four
  return lerp16(tab[i], tab[i < 256 ? i + 1 : 256], w);
}

// fixed.sqrt in C: the same normalised table read the mask's assembly does,
// for the one caller that is not in a per-pixel loop tight enough to need it
// inline. The tables were built by mask_init and are still standing.
static uint32_t sqrt16(uint32_t x32)
{
  uint8_t k = 0;
  uint16_t x, i, w;

  if (!x32)
    return 0;
  if (x32 >= ONE)
    return ONE;                    // and sqrt(1) is 1, which is seventeen bits

  // **Sixteen bits from here down.** Past the test above the value fits a
  // word, and the normalising loop runs up to eight times: as 32-bit it was
  // eight compares and eight shifts of four bytes each, for a number whose top
  // half is known to be zero.
  x = (uint16_t)x32;
  while (x < (uint16_t)(ONE >> 2)) {
    x <<= 2;
    k++;
  }
  i = x >> 8;
  w = (uint16_t)(x << 8);
  return (uint32_t)(sqrt_tab[i] + mulhi(sqrt_delta[i], w)) >> k;
}

// tanh of a Q16.16 value as a signed Q0.16, odd about zero -- fixed.tanh.
static int32_t tanh16(int32_t x)
{
  uint32_t m = (uint32_t)(x < 0 ? -x : x);

  if (m > 4UL * ONE)
    m = 4UL * ONE;
  m >>= 2;                                  // * TABLE / (LIMIT * TABLE)
  {
    uint16_t v = lookup16(tanh_tab, m);     // once, not once per branch

    return x < 0 ? -(int32_t)v : (int32_t)v;
  }
}

// How brightly the sun catches a pixel, 0..ONE across the shades.
//
// **The sun is due west and on the horizon**, so its two components are -1 and
// 0 exactly and the dot product is one negation -- no sine, no second
// multiply. See the note in tools/genmap.py for how that bearing was measured
// off the hand-drawn map.
// **The one divide in the pixel loop, done on the multiplier instead.** It was
// `(|rise| << 16) / SUN_REF`, a 32-bit library divide once per land pixel.
//
// The trick is the saturation. `tanh16` clamps its argument to 4.0, so any
// |rise| at or past 4 * SUN_REF gives the same answer and can be returned
// without dividing at all. That bounds the rest: below the clamp the numerator
// is under 2^30, and for numerators that small the reciprocal estimate
// `(mag * floor(2^32/d)) >> 32` is exact or one low -- never more. So one
// hardware multiply gives the estimate, one more prices `(e+1) * d` to decide
// whether to bump it, and a third gives the remainder the negative side's
// ceiling needs. Three multiplies at 85 cycles for one library divide.
//
// The estimate is `mulhi32(m, R)` rather than a >>32 of `mag * R`, because
// `mag` is `m << 16` and the shift cancels against the one mulhi32 already
// does.
static uint16_t sunlight_at(int32_t dx)
{
  int32_t rise = -dx;                       // scale(hx, -ONE) is exactly -hx
  uint32_t m = (uint32_t)(rise < 0 ? -rise : rise);
  int32_t q;

  if (m >= 4UL * SUN_REF) {
    q = 4L * (int32_t)ONE;                  // past the tanh table's flat end
  } else {
    uint32_t mag = mul32(m, ONE);
    uint32_t e = mulhi32(m, recip_sun);

    if (mul32(e + 1, SUN_REF) <= mag)
      e++;
    // floor division, which is what numpy's // does on the negative side: the
    // magnitude rounds *up* before it is negated.
    if (rise < 0 && mag != mul32(e, SUN_REF))
      e++;
    q = (int32_t)e;
  }
  if (rise < 0)
    q = -q;

  {
    int32_t t = (int32_t)ONE - tanh16(q);

    return (uint16_t)(t >> 1);              // // 2, and t is never negative
  }
}

uint32_t colour_build(void)
{
  uint16_t *win0, *win1, *win2, *bedrow, *lvrow, *btrow;
  uint8_t *out;
  uint32_t count = 0, top;
  uint16_t a = 0, b = 0, x, y, i;

  win0 = buf[0];
  win1 = buf[1];
  win2 = buf[2];
  bedrow = buf[3];
  lvrow = buf[4];
  btrow = buf[5];
  out = (uint8_t *)buf[6];

  tables_build();

  // **The ramp's top is the 99th percentile of dry, unbuilt land**, so the
  // colour is normalised to the country rather than to the number 1.0 -- a
  // flatland would otherwise come out one flat green.
  for (i = 0; i <= BUCKETS; i++)
    work.hist[i] = 0;
  for (y = 0; y < SIZE; y++) {
    row_in(win0, NOISE_FIELD + (uint32_t)y * (SIZE * 2));
    row_in(lvrow, LEVEL_FIELD + (uint32_t)y * (SIZE * 2));
    row_in(btrow, BUILT_FIELD + (uint32_t)y * (SIZE * 2));
    for (x = 0; x < SIZE; x++) {
      if (lvrow[x] == DRY && btrow[x] == DRY) {
        work.hist[win0[x] >> BUCKETSHIFT]++;
        count++;
      }
    }
  }
  {
    // **Ninety-nine hundredths, in that order.** Dividing first to dodge an
    // overflow that cannot happen -- 78126 * 99 is well inside 32 bits --
    // moved the cut by 44 units and every colour boundary with it.
    uint32_t want = count;

    want *= 99;
    want /= 100;
    top = count ? percentile(want) : ONE;
  }
  recip_top = recip32(top > SEA ? top - SEA : 1);
  recip_slope = recip32(SLOPE_REF);
  recip_sun = recip32(SUN_REF);
  cl_amps[0] = MOTTLE;
  cl_amps[1] = SUN_MOTTLE;

  // The two dither lattices, in genmap.py's draw order: the ramp's, then the
  // sun's. Over the histogram, which has just been read for the last time.
  for (i = 0; i < 2; i++) {
    uint32_t salt = rnd_next();
    uint16_t *c = work.lattice[i];
    uint16_t k = 0, cy, cx;

    // Spelled out rather than `c[k++] = hash32(...)`: that shape is an
    // internal compiler error in this toolchain, and it is the second time it
    // has come up -- see the same loop in src/mapgen/noise.c.
    for (cy = 0; cy < MOTTLE_PER; cy++) {
      for (cx = 0; cx < MOTTLE_PER; cx++) {
        c[k] = (uint16_t)hash32(cx, cy, salt);
        k++;
      }
    }
  }

  row_in(win0, NOISE_FIELD + (uint32_t)(SIZE - 1) * (SIZE * 2));
  row_in(win1, NOISE_FIELD);
  row_in(win2, NOISE_FIELD + (uint32_t)1 * (SIZE * 2));

  for (y = 0; y < SIZE; y++) {
    uint16_t *up = y % 3 == 0 ? win0 : (y % 3 == 1 ? win1 : win2);
    uint16_t *mid = y % 3 == 0 ? win1 : (y % 3 == 1 ? win2 : win0);
    uint16_t *dn = y % 3 == 0 ? win2 : (y % 3 == 1 ? win0 : win1);
    uint16_t iy0 = y >> DITHER_SH;
    uint16_t iy1 = (uint16_t)(iy0 + 1) & (MOTTLE_PER - 1);

    row_in(bedrow, BED_FIELD + (uint32_t)y * (SIZE * 2));
    row_in(lvrow, LEVEL_FIELD + (uint32_t)y * (SIZE * 2));
    row_in(btrow, BUILT_FIELD + (uint32_t)y * (SIZE * 2));

    cl_up = up;
    cl_mid = mid;
    cl_dn = dn;
    cl_lv = lvrow;
    cl_bt = btrow;
    cl_bed = bedrow;
    cl_out = out;
    cl_wy = dwt[y & ((1 << DITHER_SH) - 1)];
    cl_m0 = work.lattice[0] + (iy0 << MOTTLE_SH);
    cl_m1 = work.lattice[0] + (iy1 << MOTTLE_SH);
    cl_s0 = work.lattice[1] + (iy0 << MOTTLE_SH);
    cl_s1 = work.lattice[1] + (iy1 << MOTTLE_SH);

    // **Decided cheapest first, which genmap.py cannot be.** The Python writes
    // the land colour for every pixel and then paints water and masonry over
    // the top, because a numpy `where` costs the same either way. Here the
    // overwritten work is real: this island is two thirds water, and every one
    // of those 167745 pixels was paying for a ramp, a square root, a sun and
    // two dithers that the next line threw away. Water needs a depth and
    // nothing else; masonry needs the sun and nothing else. The order of the
    // tests is the order of the overwrites, so the answer is identical.
    for (x = 0; x < SIZE; x++) {
      uint16_t idx;

      if (cl_lv[x] != DRY) {
        uint32_t depth = cl_lv[x] > cl_bed[x]
                       ? mulhi32(cl_lv[x] - cl_bed[x], DEPTH_RCP) : 0;
        uint32_t wsh;

        if (depth > ONE)
          depth = ONE;
        // (v * 8) >> 16 is v >> 13, and DEEP_LEN has always been a power of
        // two -- a shift where there was a multiply and a shift.
        wsh = (ONE - depth) >> (FRACBITS - DEEP_SH);
        if (wsh >= DEEP_LEN)
          wsh = DEEP_LEN - 1;
        idx = (uint16_t)(DEEP_BASE + wsh);
      } else {
        uint16_t xl = (uint16_t)(x - 1) & SIZE_MASK;
        uint16_t xr = (uint16_t)(x + 1) & SIZE_MASK;
        int32_t ddx = (int32_t)cl_mid[xr] - cl_mid[xl];
        uint32_t lit = sunlight_at(ddx);
        int32_t sun = (int32_t)mul32(lit, (uint32_t)SHADES);

        if (cl_bt[x] != DRY) {
          uint16_t dressed = (uint16_t)(sun >> FRACBITS);
          uint16_t course = cl_bt[x];

          if (dressed >= SHADES)
            dressed = SHADES - 1;
          idx = (uint16_t)(STONE_BASE + (course << 2) + (course << 1)
                           + dressed);
        } else {
          int32_t ddy = (int32_t)cl_dn[x] - cl_up[x];
          uint16_t hh = cl_mid[x];
          uint16_t ix0 = x >> DITHER_SH;
          uint16_t ix1 = (uint16_t)(ix0 + 1) & (MOTTLE_PER - 1);
          uint32_t t, sq, slope;
          uint16_t step, face;

          // Both lattices, in one call, before anything else touches the
          // multiplier.
          cl_ox0 = (uint8_t)(ix0 << 1);
          cl_ox1 = (uint8_t)(ix1 << 1);
          cl_wx = dwt[x & ((1 << DITHER_SH) - 1)];
          cl_dither();

          // the ramp: height over the country's own top, through the gamma
          t = hh > (uint16_t)SEA ? mulhi32(hh - (uint16_t)SEA, recip_top) : 0;
          t = lookup32(gamma_tab, (t > 2UL * ONE ? 2UL * ONE : t) >> GAMMA_SH);

          // the slope pushes a pixel up it, so a cliff wears rock and a beach
          // only forms where the shore is flat
          {
            uint32_t a2 = (uint32_t)(ddy < 0 ? -ddy : ddy);
            uint32_t b2 = (uint32_t)(ddx < 0 ? -ddx : ddx);
            uint32_t s2;

            a2 = mul32(a2, a2);
            b2 = mul32(b2, b2);
            s2 = a2 + b2;
            if (s2 < a2) {                  // the sum of two squares carried
              sq = ONE;
            } else {
              sq = s2 >> FRACBITS;
              if (sq > ONE)
                sq = ONE;
            }
          }
          slope = sqrt16(sq);             // scale by (cell * ONE) / 2 is ONE
          slope = mulhi32(slope, recip_slope);
          if (slope > ONE)
            slope = ONE;
          t += mulhi32(slope, SLOPE_PUSH);
          t = mulhi32(t, CEILING);

          {
            int32_t sc = (int32_t)mul32(t, (uint32_t)RAMP_LEN) + cl_dith[0];

            sc >>= FRACBITS;
            step = sc < 0 ? 0 : (sc >= RAMP_LEN ? RAMP_LEN - 1 : (uint16_t)sc);
          }
          {
            int32_t sf = sun + cl_dith[1];

            sf >>= FRACBITS;
            face = sf < 0 ? 0 : (sf >= SHADES ? SHADES - 1 : (uint16_t)sf);
          }
          idx = (uint16_t)(RAMP_BASE + (step << 2) + (step << 1) + face);
        }
      }
      cl_out[x] = (uint8_t)idx;
      a = (uint16_t)(a + idx);
      b = (uint16_t)(b + a);
    }

    dma_copy((uint32_t)(uint16_t)out,
             COLOUR_FIELD + (uint32_t)y * SIZE, SIZE);
    row_in(y % 3 == 0 ? win0 : (y % 3 == 1 ? win1 : win2),
           NOISE_FIELD + (uint32_t)((y + 2) & SIZE_MASK) * (SIZE * 2));
  }
  return (uint32_t)b << 16 | a;
}

// --- the planes ------------------------------------------------------------
//
// The last thing the generator does: write the two finished maps in the layout
// src/voxel_asm.s addresses. **A map ships as (size/256)^2 planes of 256x256,
// one per sub-cell corner**, each on a 64K boundary -- so the march addresses a
// cell by dropping the two high coordinate bytes into a pointer and picks the
// plane with the bank byte. One big array would cost a shift and an OR on
// every sample. tools/convmap.py builds exactly this from the PNGs; here there
// are no PNGs and no files, which is the whole point of generating on the
// machine.
//
// The heightmap is bytes, not the field's words: a height is `(h * 120) >> 16`,
// the 120 being what a map's full scale is worth in the renderer's quarter-cell
// units.
#define HEIGHT_MAX 120
#define PLANE_AXIS (SIZE / CELLS)

static uint8_t even[CELLS], odd[CELLS];

static void plane_row(uint32_t base, uint16_t sy, uint16_t row)
{
  dma_copy((uint32_t)(uint16_t)even,
           base + (uint32_t)sy * 0x20000UL + (uint32_t)row * CELLS, CELLS);
  dma_copy((uint32_t)(uint16_t)odd,
           base + 0x10000UL + (uint32_t)sy * 0x20000UL
           + (uint32_t)row * CELLS, CELLS);
}

void planes_write(void)
{
  uint16_t sy, row, x;

  // The colour map: a byte a pixel already, so this only de-interleaves it.
  for (sy = 0; sy < PLANE_AXIS; sy++) {
    for (row = 0; row < CELLS; row++) {
      uint8_t *src = (uint8_t *)buf[0];

      dma_copy(COLOUR_FIELD + (uint32_t)(row * PLANE_AXIS + sy) * SIZE,
               (uint32_t)(uint16_t)src, SIZE);
      for (x = 0; x < CELLS; x++) {
        even[x] = src[x * 2];
        odd[x] = src[x * 2 + 1];
      }
      plane_row(MAP_COLOURMAP(0), sy, row);
    }
  }

  // The height map, converted from Q0.16 to the renderer's 0..255 on the way.
  for (sy = 0; sy < PLANE_AXIS; sy++) {
    for (row = 0; row < CELLS; row++) {
      row_in(buf[0], NOISE_FIELD
             + (uint32_t)(row * PLANE_AXIS + sy) * (SIZE * 2));
      for (x = 0; x < CELLS; x++) {
        uint32_t a = (uint32_t)buf[0][x * 2] * HEIGHT_MAX >> FRACBITS;
        uint32_t b = (uint32_t)buf[0][x * 2 + 1] * HEIGHT_MAX >> FRACBITS;

        even[x] = (uint8_t)(a > 255 ? 255 : a);
        odd[x] = (uint8_t)(b > 255 ? 255 : b);
      }
      plane_row(MAP_HEIGHTMAP(0), sy, row);
    }
  }
}
