// The generator's arithmetic, in the shapes a 45GS02 runs: the C half of
// tools/fixed.py.
//
// **Every routine here has a Python twin and has to agree with it exactly**,
// because that is how the port is verified -- the device prints a checksum of
// what it generated and `tools/fbmcheck.py` prints the same number from the
// PC. Rounding is therefore not a matter of taste: numpy's `>>` on a signed
// value floors, and so must this.
//
// One representation throughout: Q0.16, an unsigned 16-bit fraction of 1.0,
// so 65535 is 0.99998. ONE is only ever an intermediate. See tools/fixed.py
// for the rules and for the self-test that prices each routine against the
// float it replaced.
#ifndef MAPGEN_FIXED_H
#define MAPGEN_FIXED_H

#include <mega65.h>
#include <stdint.h>

#define FRACBITS 16
#define ONE      65536UL

// (a * b) >> 16, on the 45GS02's multiplier at $D770.
//
// It is a 32x32 -> 64 multiply, so bytes 2 and 3 of the result *are* the
// shift and there is nothing to do afterwards -- and, usefully, the product
// cannot overflow: a 32-bit intermediate would wrap where several of the
// callers here reach exactly 2^32. The C compiler's own 32-bit multiply is
// 2203 cycles against 85 for this, and this is the whole inner loop of the
// generator, so nothing in it may use `*`.
static inline uint16_t mulhi(uint32_t a, uint32_t b)
{
  MATH.multina32 = a;
  MATH.multinb32 = b;
  return (uint16_t)MATH.multout[2] | ((uint16_t)MATH.multout[3] << 8);
}

// a + ((b - a) * w >> 16), with w in Q0.16 -- and with the shift *arithmetic*,
// which is the part that cannot be got wrong.
//
// numpy shifts a negative product towards minus infinity; truncating towards
// zero instead would differ by one wherever b < a and the product is not a
// multiple of 65536, which is most of the map. The multiplier is unsigned, so
// the two directions are separate: downwards, floor(-P/65536) is
// -(P >> 16) - 1 whenever any low bit of P survives, which is what the low
// word is read for.
static inline uint16_t lerp16(uint16_t a, uint16_t b, uint16_t w)
{
  uint16_t hi, lo;

  if (b >= a) {
    MATH.multina32 = (uint32_t)(uint16_t)(b - a);
    MATH.multinb32 = w;
    hi = (uint16_t)MATH.multout[2] | ((uint16_t)MATH.multout[3] << 8);
    return (uint16_t)(a + hi);
  }

  MATH.multina32 = (uint32_t)(uint16_t)(a - b);
  MATH.multinb32 = w;
  lo = (uint16_t)MATH.multout[0] | ((uint16_t)MATH.multout[1] << 8);
  hi = (uint16_t)MATH.multout[2] | ((uint16_t)MATH.multout[3] << 8);
  return (uint16_t)(a - hi - (lo != 0));
}

// 3t^2 - 2t^3, the weight curve value noise interpolates with, factored as
// t^2 * (3 - 2t) so the inner term's 18 bits stay out of a 16-bit variable.
// The largest product it can form is 65534 * 65538 = 2^32 - 4, which is why
// mulhi's 64-bit result matters here and not only in principle.
static inline uint16_t smoothstep16(uint16_t t)
{
  return mulhi(mulhi(t, t), 3UL * ONE - 2UL * (uint32_t)t);
}

// floor(2^32 / d), which is `(ONE * ONE) // d` on the Python side. The
// numerator does not fit a uint32, so it is taken as 2^32 - 1 and corrected:
// floor((N + 1) / d) is floor(N / d) + 1 exactly when N mod d is d - 1.
// Called twice in a generation, so the two library divides do not matter.
static inline uint32_t recip32(uint32_t d)
{
  uint32_t q = 0xFFFFFFFFUL / d;

  if (0xFFFFFFFFUL % d == d - 1)
    q++;
  return q;
}

// xorshift32, the stream tools/fixed.py's Stream draws from. The *sequence of
// calls* is what reproduces a map, so anything that draws has to draw in the
// same order on both machines.
void rnd_seed(uint32_t seed);
uint32_t rnd_next(void);

// A uniform value in 0..n-1, as (rnd * n) >> 32 rather than a modulo: the top
// word of one multiply, where a remainder would be a divide.
uint16_t rnd_below(uint16_t n);

#endif
