#include "weather.h"

#include "loader.h"
#include "vic4.h"
#include "voxel.h"

// The overcast sky, top of the screen down to the horizon. Light rather than
// storm-dark on purpose: only the sky changes here, and under a black sky the
// unchanged terrain would look lit by a sun that is plainly not there.
#define OVERCAST_TOP_R 110
#define OVERCAST_TOP_G 114
#define OVERCAST_TOP_B 120
#define OVERCAST_HOR_R 180
#define OVERCAST_HOR_G 184
#define OVERCAST_HOR_B 190

// The two greys falling weather is drawn in. Palette entries 240 and 241 are
// the ones tools/convmap.py keeps clear of the terrain for a pixel-drawn
// overlay over the 3D view (HUD_INK and HUD_PAPER there), and rain and snow
// are exactly that. They are written when the weather is armed rather than
// shipped in the palette, so a clear mission leaves them for whatever wants
// them next -- the overview crosshair is drawn in 240 and gets them back.
//
// **Rain and snow share the pair**, which is what settles the old question
// about spending two more entries on snow: a flight has one weather, so there
// is never a frame with both kinds on it.
#define FALL_NEAR 240
#define FALL_FAR  241

#define FALL_DROPS 48

// How far right a rain streak leans over its whole length: one pixel every
// second row, and the longest streak is six rows. Spawning is kept this far
// from the right edge, because leaning off the end of the last strip would
// write past the framebuffer and into the sky template at $1C000.
#define FALL_LEAN 3
#define FALL_COLS ((FB_WIDTH - FALL_LEAN - 1) / VX_STEP)
// The smallest all-ones mask that covers FALL_COLS, so the fold above needs
// at most a couple of turns.
#if VX_STEP == 1
#define FALL_MASK 511
#else
#define FALL_MASK 255
#endif

// Four layers, by the low bits of the drop's index rather than by stored
// fields: it costs nothing, it cannot get out of step with itself, and it
// buys the depth that a single sheet of identical drops does not have.
//
// **These two tables are the whole difference between rain and snow.** Same
// forty-eight drops, same four layers, same state, same loop -- snow falls at
// about a third of the rate and a flake is a dot where a raindrop is a
// streak. What is not in a table is the sideways drift below, which rain does
// not have at all.
#define FALL_KINDS 2   // indexed by weather - 1: 0 rain, 1 snow
static const uint8_t fall_speed[FALL_KINDS][4]  = {{4, 7, 10, 13},
                                                   {1, 2,  3,  4}};
static const uint8_t fall_length[FALL_KINDS][4] = {{3, 4,  5,  6},
                                                   {1, 1,  2,  2}};
#define DROP_COLOUR(i) (((i) & 2) ? FALL_NEAR : FALL_FAR)

// In the low free RAM at $1600 with the renderer's own tables -- rebuilt
// every frame it rains and never touched while the disk is being read, which
// is what LOW_FREE requires.
//
// Two bytes a drop: a ray column, which fits one now that VX_COLS is fixed at
// 160, and the row it has fallen to. The framebuffer offset is not stored
// because voxel_column_offset already has it in a table, and a lookup a frame
// is cheaper than 96 more bytes of a budget this tight.
LOW_FREE static uint8_t drop_col[FALL_DROPS];
LOW_FREE static uint8_t drop_y[FALL_DROPS];

// WEATHER_CLEAR, _RAIN or _SNOW: what is falling, if anything.
static uint8_t falling;

// Whether the thermal camera is armed, which falling weather has to know
// about -- see fall_colours.
static uint8_t cold;

// How far a flake is carried sideways for every row it falls, in 8.8 screen
// columns, signed. The flight sets it every frame because it depends on where
// the camera is pointing quite as much as on the wind.
static int16_t drift;

// xorshift16. The state must never be zero, which is the one value this
// generator cannot leave.
static uint16_t rng_state = 1;

void weather_seed(uint16_t seed)
{
  rng_state = seed | 1;
}

uint16_t weather_rnd(void)
{
  uint16_t x = rng_state;

  x ^= (uint16_t)(x << 7);
  x ^= x >> 9;
  x ^= (uint16_t)(x << 8);
  rng_state = x;
  return x;
}

static uint8_t lerp(uint8_t a, uint8_t b, uint8_t i)
{
  return (uint8_t)(a + ((int16_t)b - a) * i / (SKY_SHADES - 1));
}

// The sky's colours are the only thing that has to change for an overcast
// one: voxel_init bakes the *shape* of the gradient into the template it DMAs
// across the buffer, and the shades it names are these palette entries. So
// this costs sixteen register writes once a flight and nothing per frame.
//
// The endpoints are constants here rather than parameters because there is
// only ever one ramp to draw -- a clear sky is restored from the palette, not
// recomputed -- and six arguments onto Calypsi's software stack is real code
// at a call site that exists once.
static void sky_overcast(void)
{
  uint8_t i;

  for (i = 0; i < SKY_SHADES; i++)
    vic4_set_entry((uint8_t)(SKY_BASE + i),
                   lerp(OVERCAST_TOP_R, OVERCAST_HOR_R, i),
                   lerp(OVERCAST_TOP_G, OVERCAST_HOR_G, i),
                   lerp(OVERCAST_TOP_B, OVERCAST_HOR_B, i));
}

// Put a drop somewhere along the top of the view. `y` is passed in rather
// than picked here so that arming can scatter the first drops down the screen
// while a respawn starts them at the row the last one ran off the bottom to.
static void drop_place(uint8_t i, uint8_t y)
{
  // Masked and folded rather than taken modulo RAIN_COLS: a 16-bit divide is
  // a library call, and this runs for every drop that reaches the bottom. The
  // fold leaves the low columns very slightly likelier than the high ones,
  // which is not a thing anybody can see in falling rain.
  uint16_t c = weather_rnd() & FALL_MASK;

  while (c >= FALL_COLS)
    c -= FALL_COLS;

  drop_col[i] = (uint8_t)c;
  drop_y[i] = y;
}

// The two colours, which depend on what is falling and on whether the pilot is
// looking through the thermal camera. **Snow under that camera is the thing
// this exists for**: left in its own near-white it is the brightest thing on a
// screen whose whole point is that a body is the brightest thing on it. Cold
// weather reads cold.
static void fall_colours(void)
{
  if (!falling)
    return;
  if (cold) {
    vic4_set_entry(FALL_NEAR, 60, 72, 104);
    vic4_set_entry(FALL_FAR, 36, 44, 68);
  } else if (falling == WEATHER_SNOW) {
    vic4_set_entry(FALL_NEAR, 252, 252, 255);
    vic4_set_entry(FALL_FAR, 196, 200, 210);
  } else {
    vic4_set_entry(FALL_NEAR, 220, 226, 235);
    vic4_set_entry(FALL_FAR, 150, 158, 170);
  }
}

void weather_thermal(uint8_t on)
{
  cold = on;
  fall_colours();
}

void weather_drift(int16_t sideways)
{
  drift = sideways;
}

void weather_sky(void)
{
  if (!falling) {
    // Straight out of the loaded palette rather than a blue written again
    // here: the shipped gradient comes from SKY_TOP and SKY_HORIZON in
    // tools/convmap.py, and a second copy of those numbers in C would drift
    // from them the first time anybody changed the sky.
    vic4_set_range(loaded_palette(map_current), SKY_BASE, SKY_SHADES);
    return;
  }
  sky_overcast();
}

void weather_set(uint8_t weather)
{
  falling = weather;
  // A flight starts on the optical camera whatever the last one ended on, and
  // the colours below have to agree with that.
  cold = 0;
  drift = 0;

  weather_sky();
  if (!falling)
    return;

  fall_colours();

  // Scattered down the screen, or the first frame is one horizontal band
  // falling in step.
  {
    uint8_t i;

    for (i = 0; i < FALL_DROPS; i++)
      drop_place(i, (uint8_t)(weather_rnd() & 127));
  }
}

void weather_draw(uint32_t base)
{
  uint8_t i, kind;

  if (!falling)
    return;
  kind = (uint8_t)(falling - 1);

  for (i = 0; i < FALL_DROPS; i++) {
    uint8_t layer = (uint8_t)(i & 3);
    int16_t y = (int16_t)drop_y[i] + fall_speed[kind][layer];
    uint8_t xin, len, colour, k, col;
    uint8_t __far *p;
    int16_t at;

    if (y >= FB_HEIGHT)
      drop_place(i, (uint8_t)(y - FB_HEIGHT));  // off the bottom, start again
    else
      drop_y[i] = (uint8_t)y;

    y = drop_y[i];
    len = fall_length[kind][layer];
    if (y + len > FB_HEIGHT)
      len = (uint8_t)(FB_HEIGHT - y);  // clipped at the bottom of the view

    col = drop_col[i];
    // **Snow is blown sideways and rain is not**, which is the one thing here
    // that is not a table. A flake is carried `drift` columns for every row it
    // has fallen, so its column is a function of its row and needs no state of
    // its own -- and with a length of one or two pixels there is nothing to
    // lean *within*, which is why the streak lean below stays rain's.
    //
    // Wrapped rather than dropped at the edges: a snowfall that is only blown
    // off one side leaves that side of the picture empty within a few seconds.
    // The fold is a loop for the same reason drop_place's is: at the drift
    // scale the flight sets it is one turn at most, and it stays right if
    // anybody changes the scale.
    if (kind) {
      int16_t moved = (int16_t)col + voxel_mul_shift8(drift, y);

      while (moved < 0)
        moved += FALL_COLS;
      while (moved >= FALL_COLS)
        moved -= FALL_COLS;
      col = (uint8_t)moved;
    }

    xin = (uint8_t)((col * VX_STEP) & 7);
    colour = DROP_COLOUR(i);
    p = (uint8_t __far *)(base + voxel_column_offset(col));
    at = y * 8;

    for (k = 0; k < len; k++) {
      p[at] = colour;
      at += 8;
      // The lean. Column strips put the next pixel across in the very next
      // byte until the strip ends, which is the same step src/sprite.c makes
      // along a figure's width.
      if (!kind && (k & 1)) {
        if (xin == 7) {
          at += FB_STRIDE - 7;
          xin = 0;
        } else {
          at++;
          xin++;
        }
      }
    }
  }
}
