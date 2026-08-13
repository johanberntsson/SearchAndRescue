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

// **The product's windows, addressed directly.** `MATH.multout[]` is a byte
// array, so `multout[2] | (multout[3] << 8) | ...` compiles to four loads,
// three shifts and three ors -- around twenty instructions to read a number
// the 45GS02 can fetch in one `ldq`, because the eight product bytes at $D778
// are consecutive and the machine has 32-bit loads. Naming the windows as
// words and longwords of their own is the whole difference.
//
// Volatile, and read *after* both operands are written: the multiplier is
// combinational, so the product is whatever the inputs currently say.
#define MATH_OUT16   (*(volatile uint16_t *)0xD778UL)  // (a * b), low word
#define MATH_OUT_H16 (*(volatile uint16_t *)0xD77AUL)  // (a * b) >> 16, word
#define MATH_OUT_H32 (*(volatile uint32_t *)0xD77AUL)  // (a * b) >> 16, long
#define MATH_OUT_T16 (*(volatile uint16_t *)0xD77CUL)  // (a * b) >> 32, word

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
  return MATH_OUT_H16;
}

// The same product, kept to thirty-two bits. ONE is a legal *intermediate*
// even though a stored field value is sixteen bits -- the hill profile is
// smoothstep(ONE - d), which is exactly 1.0 at the centre of every hill -- so
// the few places that reach it need the wider window.
static inline uint32_t mulhi32(uint32_t a, uint32_t b)
{
  MATH.multina32 = a;
  MATH.multinb32 = b;
  return MATH_OUT_H32;
}

// The low thirty-two bits: a *plain* multiply, not a Q0.16 one, for the places
// that scale by a small whole number rather than by a fraction.
//
// **`*` on two 32-bit operands is a 2203-cycle library call**, and the colour
// pass had five of them in its pixel loop -- two squares and three by
// constants of 21, 8 and 6. This is the same 85 cycles the fractional forms
// above cost. The caller has to know its product fits: everything here does,
// because these are heights and step counts, not the 64-bit intermediates
// mulhi32 exists for.
static inline uint32_t mul32(uint32_t a, uint32_t b)
{
  MATH.multina32 = a;
  MATH.multinb32 = b;
  return MATH.multout32;
}

// (a * b) >> 32: bytes 4 and 5 of the product. The box blur's reciprocal is a
// Q0.32 value, so its window sum comes back out of the top half.
static inline uint16_t mulhi32top(uint32_t a, uint32_t b)
{
  MATH.multina32 = a;
  MATH.multinb32 = b;
  return MATH_OUT_T16;
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
    hi = MATH_OUT_H16;
    return (uint16_t)(a + hi);
  }

  MATH.multina32 = (uint32_t)(uint16_t)(a - b);
  MATH.multinb32 = w;
  lo = MATH_OUT16;
  hi = MATH_OUT_H16;
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

// Where the stream has got to, so one program can hand it to the next.
uint32_t rnd_state(void);

// A uniform value in 0..n-1, as (rnd * n) >> 32 rather than a modulo: the top
// word of one multiply, where a remainder would be a divide.
uint16_t rnd_below(uint16_t n);

#endif
