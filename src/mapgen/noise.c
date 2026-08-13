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

// The x weight per octave, precomputed: it depends only on the column, and
// working it out per pixel would put two more multiplies in the inner loop
// for a number that repeats every row. The y weight is one per row and is
// worked out there.
static uint16_t wx[OCT][SIZE];

// One row of the octave sum, at Q8.16 -- the sum reaches several times ONE
// before it is normalised. In chip RAM on purpose: see above.
static uint32_t acc[SIZE];

static uint32_t weight_recip;

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
  uint16_t off = 0;
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

    for (x = 0; x < SIZE; x++) {
      uint16_t sx = (uint16_t)(x - offx[o]) & SIZE_MASK;
      wx[o][x] = smoothstep16((uint16_t)((sx & (step - 1)) << (FRACBITS - sh)));
    }

    amps[o] = amp;
    weight += amp;
    amp = mulhi(amp, GAIN);
  }

  weight_recip = recip32(weight);
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
  uint16_t a = 0, b = 0;
  uint16_t x, y;

  for (y = 0; y < SIZE; y++) {
    uint16_t __far *out =
        (uint16_t __far *)(NOISE_FIELD + (uint32_t)y * (SIZE * 2));
    uint8_t o;

    for (x = 0; x < SIZE; x++)
      acc[x] = 0;

    for (o = 0; o < OCT; o++) {
      uint16_t period = (uint16_t)BASE << o;
      uint8_t sh = lshift[o];
      uint16_t pmask = period - 1;
      uint16_t stepmask = (uint16_t)((1 << sh) - 1);
      uint16_t ox = offx[o];
      uint16_t sy = (uint16_t)(y - offy[o]) & SIZE_MASK;
      uint16_t iy0 = sy >> sh;
      uint16_t iy1 = (uint16_t)(iy0 + 1) & pmask;
      uint16_t wy = smoothstep16((uint16_t)((sy & stepmask) << (FRACBITS - sh)));
      const uint16_t *c0 = corner + coff[o] + iy0 * period;
      const uint16_t *c1 = corner + coff[o] + iy1 * period;
      const uint16_t *w = wx[o];
      uint32_t amp = amps[o];

      for (x = 0; x < SIZE; x++) {
        uint16_t sx = (uint16_t)(x - ox) & SIZE_MASK;
        uint16_t i0 = sx >> sh;
        uint16_t i1 = (uint16_t)(i0 + 1) & pmask;
        uint16_t top = lerp16(c0[i0], c0[i1], w[x]);
        uint16_t bot = lerp16(c1[i0], c1[i1], w[x]);

        acc[x] += mulhi(lerp16(top, bot, wy), amp);
      }
    }

    for (x = 0; x < SIZE; x++) {
      uint16_t v = mulhi(acc[x], weight_recip);

      out[x] = v;
      a = (uint16_t)(a + v);
      b = (uint16_t)(b + a);
    }
  }

  return (uint32_t)b << 16 | a;
}

// **A note for whoever ports `stretch` next.** It ends with
// `np.clip(..., 0, ONE)`, and ONE is 65536, which does not fit the uint16 a
// field value is stored in -- the top half per cent of the map lands exactly
// there. tools/fixed.py's own rule says ONE is only ever an intermediate, so
// the fix is probably to clip to 65535 on both sides rather than to widen the
// field; but it re-rolls every map by up to one part in 65536, so it is a
// decision to take deliberately and to check the PNGs against, not a detail
// to settle in the C.
