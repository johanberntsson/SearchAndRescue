// xorshift32, the stream tools/fixed.py's Stream draws from. Its own file
// because both the noise and everything after it draws from the one stream,
// and the *sequence of calls* is the whole of what reproduces a map.

#include "fixed.h"

static uint32_t state;

void rnd_seed(uint32_t seed)
{
  // The Python side substitutes a constant for a zero seed, because xorshift
  // cannot leave it.
  state = seed ? seed : 0x1D872B41UL;
}

uint32_t rnd_next(void)
{
  uint32_t x = state;

  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  state = x;
  return x;
}

uint16_t rnd_below(uint16_t n)
{
  MATH.multina32 = rnd_next();
  MATH.multinb32 = n;
  // The top word of a 32x32 product is (rnd * n) >> 32. Bytes 4 and 5 are
  // enough for every n this generator asks for -- a map is 512 or 1024 across
  // -- and reading two rather than four is the point of doing it this way.
  return (uint16_t)MATH.multout[4] | ((uint16_t)MATH.multout[5] << 8);
}

// **Out by pointer, not by return.** `return state;` compiled to a partial
// load: the caller got the low half of the word and 0xFFFF in the high one,
// whichever of the three ways it tried to store it. `rnd_next` returns a
// uint32_t perfectly well, so it is returning *this* static that the compiler
// gets wrong, and writing through a pointer takes the question away. It cost
// stage two the whole colour pass -- the two dither lattices are hashed off
// this word, so every pixel came out a shade or a step wrong while the
// terrain, which is stage one's own and never crosses the gap, stayed exact.
void rnd_state(uint32_t *out)
{
  *out = state;
}
