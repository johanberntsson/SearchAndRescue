#!/usr/bin/env python3
"""Integer arithmetic for the map generator, in the shapes a 45GS02 can run.

`tools/genmap.py` was written in floats and numpy, which is fine for a build
time tool and impossible on the MEGA65. This module is the arithmetic it has to
be rewritten against so that the same YAML gives the same map on both machines
-- see documentation/on-device-maps.md for why that has to happen on the PC
first, and what it costs if it does not.

The rules everything here obeys, because they are what makes a routine
portable rather than merely integer:

- **one representation.** A field value is Q0.16: an unsigned 16-bit fraction
  of 1.0, so 65535 is 0.99998 and ONE (65536) is only ever an intermediate.
  That is one 45GS02 word, one attic RAM read, and 1024x1024 of them is 2 MB
  of the 8 available.
- **every operation is a shift, an add, or the hardware multiplier.** No
  divides: a division by a constant is a multiply by its reciprocal, and a
  division by a variable is a reciprocal lookup. `$D770` does 32x32 in 16
  cycles, so a 16x16 -> 32 product is one instruction's worth of work; a
  divide is not.
- **no transcendentals.** sqrt, tanh and the ramp's gamma are 257-entry
  tables with linear interpolation between the entries, which is exact enough
  for a field that ends up quantised to 120 height units or 21 colour steps,
  and is a `lda (ptr),y` on the machine.
- **numpy is a convenience, never a semantic.** Every function here is written
  so that the scalar C version is the obvious transliteration: int64 appears
  only to stop numpy wrapping where C would use a 32-bit intermediate, and
  every result is masked or shifted back to the width it claims.

The self-test at the bottom (`python3 tools/fixed.py`) checks each routine
against the float it replaces, and prints the worst error in units of the last
place of the eventual output -- height units and colour steps -- rather than in
abstract fractions.
"""

import numpy as np

FRACBITS = 16
ONE = 1 << FRACBITS          # 1.0
HALF = ONE >> 1
MASK = ONE - 1

# The tables. 257 entries rather than 256 so that interpolating the last
# interval needs no special case: entry[256] is the value at 1.0.
TABLE = 256


def _i64(a):
    """A numpy array (or scalar) as int64, which is where products live.

    C would use int32 for a 16x16 product and int64 only where this module
    says so; numpy would silently wrap an int32 array, so everything wide is
    made wide on purpose here and narrowed back deliberately.
    """
    return np.asarray(a, dtype=np.int64)


def mul(a, b):
    """Q0.16 * Q0.16 -> Q0.16, truncating. One $D770 multiply and a shift."""
    return (_i64(a) * _i64(b)) >> FRACBITS


def scale(v, w):
    """A value of any width by a Q0.16 weight, truncating.

    The same operation as mul() and a different intent: `v` is a height, a
    count or a coordinate rather than a fraction, so the result keeps v's
    units. Kept separate because the C versions will differ in type.
    """
    return (_i64(v) * _i64(w)) >> FRACBITS


def lerp(a, b, w):
    """a + (b - a) * w, with w in Q0.16. The one interpolation in the file."""
    return _i64(a) + (((_i64(b) - _i64(a)) * _i64(w)) >> FRACBITS)


def smoothstep(t):
    """3t^2 - 2t^3 on Q0.16, the weight curve value noise interpolates with.

    Two multiplies, one shift, one subtract. The float version is
    t * t * (3 - 2t); written that way in fixed point the inner term reaches
    3.0 and needs 18 bits, so it is factored as t^2 * (3 - 2t) with the 3
    carried as 3 * ONE.
    """
    t = _i64(t)
    tt = (t * t) >> FRACBITS
    return (tt * (3 * ONE - 2 * t)) >> FRACBITS


def _sqrt_table():
    """sqrt over 0..1 at 257 points, in Q0.16."""
    x = np.arange(TABLE + 1, dtype=np.float64) / TABLE
    return np.round(np.sqrt(x) * ONE).astype(np.int64)


def _tanh_table(limit):
    """tanh over 0..`limit` at 257 points, in Q0.16. Odd, so only one side."""
    x = np.arange(TABLE + 1, dtype=np.float64) * limit / TABLE
    return np.round(np.tanh(x) * ONE).astype(np.int64)


def gamma_table(gamma, top=1.0):
    """x**gamma over 0..`top` at 257 points, in Q0.16.

    **`top` is not decoration.** The ramp's input runs past 1.0 -- it is a
    height over the map's 99th percentile, so the top one per cent of the
    terrain is above it by construction -- and a table that stops at 1.0
    saturates exactly the pixels that should be reaching the top of the ramp.
    Read at 0..2 instead, and a summit goes where the float put it; read at
    0..1 and a flatland's hilltops come out the colour of its middle slopes,
    which is what they did until this was found.

    `top` must be a power of two so the caller's normalisation is a shift.
    The output passes ONE for x > 1 and is int32 on the machine.
    """
    x = np.arange(TABLE + 1, dtype=np.float64) / TABLE * top
    return np.round(x ** gamma * ONE).astype(np.int64)


def lookup(table, x):
    """A 257-entry Q0.16 table read at Q0.16 `x`, interpolated.

    The index is the top eight bits and the weight is the rest, so this is a
    shift, two table reads, a subtract and one multiply -- and on the 45GS02
    the two reads are consecutive bytes of the same page.
    """
    x = np.clip(_i64(x), 0, ONE)
    i = x >> (FRACBITS - 8)
    w = (x << 8) & MASK
    lo = table[np.minimum(i, TABLE)]
    hi = table[np.minimum(i + 1, TABLE)]
    return lerp(lo, hi, w)


SQRT = _sqrt_table()
TANH_LIMIT = 4                       # tanh(4) is 0.9993; past it the table is flat
TANH = _tanh_table(TANH_LIMIT)


def sqrt(x):
    """sqrt of a Q0.16 fraction in 0..1, as Q0.16. Table, for per-pixel work.

    **The input is normalised into 0.25..1 first**, and that is the whole
    trick: a square root's slope is infinite at zero, so a table read straight
    at small x is wrong by up to 1.5% of full scale -- two height units, which
    is a visible terrace on a hillside. Shifting x up in pairs of bits until
    its top is set puts every input on the flat part of the curve, where
    linear interpolation between table entries is worth 1e-5, and shifting the
    result back down by half as many bits is exact because sqrt(4^k) is 2^k.

    At most eight shift-and-test rounds, then one table read: call it 40
    cycles on the 45GS02 against the ~200 an exact digit-by-digit root costs.
    That is the trade -- this one is for the megapixel loops, isqrt() below is
    for the thousands.
    """
    x = np.clip(_i64(x), 0, ONE)
    k = np.zeros_like(x)
    for _ in range(FRACBITS // 2):
        low = (x != 0) & (x < (ONE >> 2))
        x = np.where(low, x << 2, x)
        k = np.where(low, k + 1, k)
    return lookup(SQRT, x) >> k


def isqrt(n):
    """Exact integer square root, digit by digit. For setup and for discs.

    The schoolbook binary method: no multiply, no divide, one shift and a
    conditional subtract per output bit, which is as 6502 as arithmetic gets.
    Used where a pixel's *radius* has to be exactly right -- the disc stamps,
    where a rounding error moves a shoreline -- and never per map pixel.

    **The domain is 32 bits and the check is not decoration.** The first
    quotient bit tried is 2^30, so a larger input silently returns a root of
    whatever fits below that -- which is exactly what happened the first time
    hypot() asked for sixteen fractional bits and handed this 2^45. The disc
    profiles came out as garbage, the hills grew twenty height units past the
    map's range, and the arithmetic comparison blamed rounding.
    """
    n = _i64(n)
    if n.max(initial=0) >= (1 << 32):
        raise ValueError(f"isqrt: {n.max()} is past the 32-bit domain; "
                         f"scale the input down or ask for fewer bits")
    root = np.zeros_like(n)
    rem = n.copy()
    bit = np.int64(1) << 30
    while bit > 0:
        step = root + bit
        take = rem >= step
        rem = np.where(take, rem - step, rem)
        root = np.where(take, (root >> 1) + bit, root >> 1)
        bit >>= 2
    return root


def hypot(dy, dx, shift):
    """sqrt(dy^2 + dx^2) for integer offsets, to `shift` fractional bits.

    Used by everything that stamps a disc, so it is exact: the squares are
    integers, the root is isqrt(), and the fractional bits come from scaling
    the squares up by 4^shift before the root rather than the root afterwards.

    `shift` is what keeps this inside isqrt's 32 bits: at 8 fractional bits --
    a 256th of a pixel, finer than anything here can use -- a disc of radius
    360 still fits. Asking for 16 does not, and fails loudly rather than
    quietly returning nonsense.
    """
    dy, dx = _i64(dy), _i64(dx)
    return isqrt((dy * dy + dx * dx) << (2 * shift))


DISCBITS = 8                 # fractional bits a disc distance is measured in


def tanh(x):
    """tanh of a Q16.16 value, as a signed Q0.16. Odd about zero."""
    x = _i64(x)
    s = np.sign(x)
    q = np.minimum(np.abs(x), TANH_LIMIT * ONE) * TABLE // (TANH_LIMIT * TABLE)
    return s * lookup(TANH, q)


# --- the one random stream ----------------------------------------------

class Stream:
    """xorshift32, and the three draws the generator makes from it.

    numpy's PCG64 cannot move to the MEGA65 and this can: three shifts and
    three exclusive-ors on a 32-bit word, which the 45GS02 does natively.
    `src/weather.c` already runs the 16-bit version of exactly this.

    The state must never be zero, which is the one value the seed is fixed
    up for. Draw order is part of the map: the same seed and the same
    sequence of calls is the whole of what reproduces it.
    """

    def __init__(self, seed):
        self.state = (int(seed) & 0xFFFFFFFF) or 0x1D872B41

    def next(self):
        x = self.state
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        self.state = x
        return x

    def salt(self):
        """A lattice salt: the raw word."""
        return self.next()

    def below(self, n):
        """A uniform integer in 0..n-1, by multiply-shift rather than modulo.

        `(rnd * n) >> 32` is one $D770 multiply and the top word of the
        result; a modulo would be a divide, which the machine does not have.
        The bias is one part in 2^32 / n and nothing here counts pixels that
        finely.
        """
        return (self.next() * int(n)) >> 32

    def pick(self, n, k):
        """k distinct values from 0..n-1, in draw order.

        By rejection rather than by shuffling an array of n: the caller may be
        choosing eight lakes out of nine thousand candidate minima, and the
        array would be the largest allocation in the generator for no reason.
        Rejection costs one extra draw per collision, and with k << n there
        are almost none.
        """
        k = min(int(k), int(n))
        seen, out = set(), []
        while len(out) < k:
            v = self.below(n)
            if v not in seen:
                seen.add(v)
                out.append(v)
        return out


# --- self-test ----------------------------------------------------------

def _selftest():
    """Each routine against the float it replaces, in units of the output.

    A height unit is 1/120 of the map's range and a colour step is 1/21 of the
    land ramp, so an error of a tenth of one of those is invisible by
    construction rather than by hope.
    """
    rng = np.random.default_rng(7)
    a = rng.integers(0, ONE, 10000)
    b = rng.integers(0, ONE, 10000)

    def worst(got, want, name, unit, per):
        err = np.abs(np.asarray(got, float) / ONE - want).max()
        print(f"  {name:<28} worst {err * per:7.4f} {unit}"
              f"   ({err:.2e} of full scale)")
        return err

    print("fixed-point routines against the floats they replace:")
    worst(mul(a, b), (a / ONE) * (b / ONE), "mul", "height units", 120)
    worst(lerp(a, b, 30000), (a / ONE) + ((b - a) / ONE) * (30000 / ONE),
          "lerp", "height units", 120)
    worst(smoothstep(a), (lambda t: t * t * (3 - 2 * t))(a / ONE),
          "smoothstep", "height units", 120)
    worst(sqrt(a), np.sqrt(a / ONE), "sqrt", "height units", 120)

    x = rng.integers(-3 * ONE, 3 * ONE, 10000)
    worst(tanh(x), np.tanh(x / ONE), "tanh", "colour shades", 6)

    gamma = gamma_table(1.8)
    worst(lookup(gamma, a), (a / ONE) ** 1.8, "gamma 1.8 by table",
          "colour steps", 21)

    span = np.arange(-64, 65)
    d = hypot(span[:, None], span[None, :], 8)
    want = np.hypot(span[:, None], span[None, :])
    err = np.abs(np.asarray(d, float) / 256 - want).max()
    print(f"  {'hypot over a 129x129 disc':<28} worst {err:7.4f} pixels"
          f"   (exact to the last bit of 1/256)")
    big = np.array([1, 2, 3, 5, 99, 65535, 1 << 20, (1 << 30) - 1])
    assert (isqrt(big) == np.floor(np.sqrt(big.astype(float)))).all()
    print(f"  {'isqrt to 2^30':<28} exact")

    # The stream: distribution and the promise that pick() gives no duplicates.
    s = Stream(12345)
    draws = [s.below(1000) for _ in range(100000)]
    counts = np.bincount(draws, minlength=1000)
    print(f"  {'xorshift32 below(1000)':<28} mean {np.mean(draws):7.2f} "
          f"(want 499.50), bucket spread {counts.min()}..{counts.max()} of 100")
    picks = Stream(999).pick(9000, 8)
    assert len(set(picks)) == 8
    print(f"  {'pick(9000, 8)':<28} {picks}, all distinct")

    # And that a stream is reproducible from its seed alone, which is the
    # whole promise the mission file makes.
    assert [Stream(4242).next() for _ in range(5)] == \
           [Stream(4242).next() for _ in range(5)]
    print("  a stream repeats exactly from its seed")


if __name__ == "__main__":
    _selftest()
