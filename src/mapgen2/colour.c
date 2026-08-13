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

// Six rows of the field and a row of finished indices. The three-row window is
// for the neighbours the slope and the sun need.
//
// **No banking here.** Stage one needs the 12 KB under the ROMs and stage two
// does not: with only colour and the planes in it, this program's whole
// working set is under 12 KB and its code under 8, so it fits the 32 KB the
// stock linker script gives. The banking stays in kernal.s because both
// programs share the file, and it costs a register write either way.
static uint16_t buf[7][SIZE];
static uint32_t hist[BUCKETS + 1];

// The dither lattices are 32 across, so a cell is 16 pixels and the weight is
// smoothstep of a sixteenth -- sixteen entries, not the 512 a row would need.
#define DITHER_SH 4
static uint16_t dwt[1 << DITHER_SH];

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
    cum += hist[b];
    if (cum >= want)
      break;
  }
  inside = hist[b];
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
#define DEEP_LEN   8
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

// (ONE * ONE) / SLOPE_REF, worked out once rather than per pixel.
static uint32_t recip_slope;

// The two dither lattices, at the map's own size over 16.
#define MOTTLE_PER  (SIZE / 16)

static uint16_t __far *mottle_corner, *smottle_corner;

// value noise at MOTTLE_PER, scaled and signed: scale(2n - ONE, amp), the same
// shape the mask's wobble and the river's meander take.
static int32_t dither_at(const uint16_t __far *corner, uint32_t amp,
                         uint16_t y, uint16_t x)
{
  uint16_t stepmask = (uint16_t)((1 << DITHER_SH) - 1);
  uint16_t iy0 = y >> DITHER_SH, ix0 = x >> DITHER_SH;
  uint16_t iy1 = (uint16_t)(iy0 + 1) & (MOTTLE_PER - 1);
  uint16_t ix1 = (uint16_t)(ix0 + 1) & (MOTTLE_PER - 1);
  const uint16_t __far *c0 = corner + (uint16_t)iy0 * MOTTLE_PER;
  const uint16_t __far *c1 = corner + (uint16_t)iy1 * MOTTLE_PER;
  uint16_t wx = dwt[x & stepmask];
  uint16_t n = lerp16(lerp16(c0[ix0], c0[ix1], wx),
                      lerp16(c1[ix0], c1[ix1], wx), dwt[y & stepmask]);
  uint32_t mag;
  uint16_t lw, hw;

  if (n >= 0x8000) {
    mag = (uint32_t)(n - 0x8000) << 1;
    MATH.multina32 = mag;
    MATH.multinb32 = amp;
    return (int32_t)(uint32_t)((uint32_t)MATH.multout[2]
                               | ((uint32_t)MATH.multout[3] << 8)
                               | ((uint32_t)MATH.multout[4] << 16));
  }
  mag = ((uint32_t)0x8000 - n) << 1;
  MATH.multina32 = mag;
  MATH.multinb32 = amp;
  lw = (uint16_t)MATH.multout[0] | ((uint16_t)MATH.multout[1] << 8);
  hw = (uint16_t)MATH.multout[2] | ((uint16_t)MATH.multout[3] << 8);
  {
    uint32_t v = (uint32_t)hw | ((uint32_t)MATH.multout[4] << 16);

    if (lw)
      v++;
    return -(int32_t)v;
  }
}

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
  w = (uint16_t)((x << 8) & 0xFFFF);
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
  w = (uint16_t)((x << 8) & 0xFFFF);
  return lerp16(tab[i], tab[i < 256 ? i + 1 : 256], w);
}

// fixed.sqrt in C: the same normalised table read the mask's assembly does,
// for the one caller that is not in a per-pixel loop tight enough to need it
// inline. The tables were built by mask_init and are still standing.
static uint32_t sqrt16(uint32_t x)
{
  uint8_t k = 0;
  uint16_t i, w;

  if (!x)
    return 0;
  if (x >= ONE)
    return ONE;                    // and sqrt(1) is 1, which is seventeen bits
  while (x < (ONE >> 2)) {
    x <<= 2;
    k++;
  }
  i = (uint16_t)(x >> 8);
  w = (uint16_t)((x << 8) & 0xFFFF);
  return (uint32_t)(sqrt_tab[i] + mulhi(sqrt_delta[i], w)) >> k;
}

// tanh of a Q16.16 value as a signed Q0.16, odd about zero -- fixed.tanh.
static int32_t tanh16(int32_t x)
{
  uint32_t m = (uint32_t)(x < 0 ? -x : x);

  if (m > 4UL * ONE)
    m = 4UL * ONE;
  m >>= 2;                                  // * TABLE / (LIMIT * TABLE)
  return x < 0 ? -(int32_t)lookup16(tanh_tab, m)
               : (int32_t)lookup16(tanh_tab, m);
}

// How brightly the sun catches a pixel, 0..ONE across the shades.
//
// **The sun is due west and on the horizon**, so its two components are -1 and
// 0 exactly and the dot product is one negation -- no sine, no second
// multiply. See the note in tools/genmap.py for how that bearing was measured
// off the hand-drawn map.
static uint16_t sunlight_at(int32_t dx)
{
  int32_t rise = -dx;                       // scale(hx, -ONE) is exactly -hx
  uint32_t mag = (uint32_t)(rise < 0 ? -rise : rise) << FRACBITS;
  int32_t q;

  // floor division, which is what numpy's // does on the negative side
  if (rise < 0)
    q = -(int32_t)((mag + SUN_REF - 1) / SUN_REF);
  else
    q = (int32_t)(mag / SUN_REF);

  {
    int32_t t = (int32_t)ONE - tanh16(q);

    return (uint16_t)(t >> 1);              // // 2, and t is never negative
  }
}

uint32_t colour_build(void)
{
  uint16_t *win0, *win1, *win2, *bedrow, *lvrow, *btrow;
  uint8_t *out;
  uint32_t recip_top, count = 0, top;
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
    hist[i] = 0;
  for (y = 0; y < SIZE; y++) {
    row_in(win0, NOISE_FIELD + (uint32_t)y * (SIZE * 2));
    row_in(lvrow, LEVEL_FIELD + (uint32_t)y * (SIZE * 2));
    row_in(btrow, BUILT_FIELD + (uint32_t)y * (SIZE * 2));
    for (x = 0; x < SIZE; x++) {
      if (lvrow[x] == DRY && btrow[x] == DRY) {
        hist[win0[x] >> BUCKETSHIFT]++;
        count++;
      }
    }
  }
  {
    uint32_t want = count / 100;

    want *= 99;
    top = count ? percentile(want) : ONE;
  }
  recip_top = recip32(top > SEA ? top - SEA : 1);
  recip_slope = recip32(SLOPE_REF);

  // The two dither lattices, in genmap.py's draw order: the ramp's, then the
  // sun's. In bank 1's safe span above the meander's.
  mottle_corner = (uint16_t __far *)0x1B000UL;
  smottle_corner = (uint16_t __far *)0x1C000UL;
  for (i = 0; i < 2; i++) {
    uint32_t salt = rnd_next();
    uint16_t __far *c = i ? smottle_corner : mottle_corner;
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

    row_in(bedrow, BED_FIELD + (uint32_t)y * (SIZE * 2));
    row_in(lvrow, LEVEL_FIELD + (uint32_t)y * (SIZE * 2));
    row_in(btrow, BUILT_FIELD + (uint32_t)y * (SIZE * 2));

    for (x = 0; x < SIZE; x++) {
      uint16_t xl = (uint16_t)(x - 1) & SIZE_MASK;
      uint16_t xr = (uint16_t)(x + 1) & SIZE_MASK;
      int32_t ddy = (int32_t)dn[x] - up[x];
      int32_t ddx = (int32_t)mid[xr] - mid[xl];
      uint16_t hh = mid[x];
      uint32_t t, sq, slope;
      int32_t sun;
      uint16_t step, face, idx;

      // the ramp: height over the country's own top, through the gamma
      t = hh > (uint16_t)SEA ? mulhi32(hh - (uint16_t)SEA, recip_top) : 0;
      t = lookup32(gamma_tab, (t > 2UL * ONE ? 2UL * ONE : t) / GAMMA_TOP);

      // the slope pushes a pixel up it, so a cliff wears rock and a beach
      // only forms where the shore is flat
      {
        uint32_t a2 = (uint32_t)(ddy < 0 ? -ddy : ddy);
        uint32_t b2 = (uint32_t)(ddx < 0 ? -ddx : ddx);
        uint32_t s2;

        a2 *= a2;
        b2 *= b2;
        s2 = a2 + b2;
        sq = (s2 < a2 || (s2 >> FRACBITS) > ONE) ? ONE : s2 >> FRACBITS;
      }
      slope = sqrt16(sq);                 // scale by (cell * ONE) / 2 is ONE
      slope = mulhi32(slope, recip_slope);
      if (slope > ONE)
        slope = ONE;
      t += mulhi32(slope, SLOPE_PUSH);
      t = mulhi32(t, CEILING);

      {
        int32_t sc = (int32_t)(t * RAMP_LEN)
                   + dither_at(mottle_corner, MOTTLE, y, x);

        sc >>= FRACBITS;
        step = sc < 0 ? 0 : (sc >= RAMP_LEN ? RAMP_LEN - 1 : (uint16_t)sc);
      }

      sun = (int32_t)sunlight_at(ddx) * SHADES;
      {
        int32_t sf = sun + dither_at(smottle_corner, SUN_MOTTLE, y, x);

        sf >>= FRACBITS;
        face = sf < 0 ? 0 : (sf >= SHADES ? SHADES - 1 : (uint16_t)sf);
      }

      idx = (uint16_t)(RAMP_BASE + SHADES * step + face);

      if (btrow[x] != DRY) {
        uint16_t dressed = (uint16_t)(sun >> FRACBITS);

        if (dressed >= SHADES)
          dressed = SHADES - 1;
        idx = (uint16_t)(STONE_BASE + SHADES * btrow[x] + dressed);
      }
      if (lvrow[x] != DRY) {
        uint32_t depth = lvrow[x] > bedrow[x]
                       ? mulhi32(lvrow[x] - bedrow[x], DEPTH_RCP) : 0;
        uint32_t wsh;

        if (depth > ONE)
          depth = ONE;
        wsh = ((ONE - depth) * DEEP_LEN) >> FRACBITS;
        if (wsh >= DEEP_LEN)
          wsh = DEEP_LEN - 1;
        idx = (uint16_t)(DEEP_BASE + wsh);
      }
      out[x] = (uint8_t)idx;
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
