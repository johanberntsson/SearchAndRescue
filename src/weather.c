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

// The rain's two greys. Palette entries 240 and 241 are the ones
// tools/convmap.py keeps clear of the terrain for a pixel-drawn overlay over
// the 3D view (HUD_INK and HUD_PAPER there), and rain is exactly that. They
// are written when the rain is armed rather than shipped in the palette, so a
// clear mission leaves them for whatever wants them next.
#define RAIN_NEAR 240
#define RAIN_FAR  241

#define RAIN_DROPS 48

// How far right a streak leans over its whole length: one pixel every second
// row, and the longest streak is six rows. Spawning is kept this far from the
// right edge, because leaning off the end of the last strip would write past
// the framebuffer and into the sky template at $1C000.
#define RAIN_LEAN 3
#define RAIN_COLS ((FB_WIDTH - RAIN_LEAN - 1) / VX_STEP)
// The smallest all-ones mask that covers RAIN_COLS, so the fold above needs
// at most a couple of turns.
#if VX_STEP == 1
#define RAIN_MASK 511
#else
#define RAIN_MASK 255
#endif

// Four layers of rain, by the low bits of the drop's index rather than by
// stored fields: it costs nothing, it cannot get out of step with itself, and
// it buys the depth that a single sheet of identical drops does not have.
#define DROP_SPEED(i)  (4 + ((i) & 3) * 3)   // rows a frame
#define DROP_LENGTH(i) (3 + ((i) & 3))       // pixels
#define DROP_COLOUR(i) (((i) & 2) ? RAIN_NEAR : RAIN_FAR)

// In the low free RAM at $1600 with the renderer's own tables -- rebuilt
// every frame it rains and never touched while the disk is being read, which
// is what LOW_FREE requires.
//
// Two bytes a drop: a ray column, which fits one now that VX_COLS is fixed at
// 160, and the row it has fallen to. The framebuffer offset is not stored
// because voxel_column_offset already has it in a table, and a lookup a frame
// is cheaper than 96 more bytes of a budget this tight.
LOW_FREE static uint8_t drop_col[RAIN_DROPS];
LOW_FREE static uint8_t drop_y[RAIN_DROPS];

static uint8_t raining;

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
  uint16_t c = weather_rnd() & RAIN_MASK;

  while (c >= RAIN_COLS)
    c -= RAIN_COLS;

  drop_col[i] = (uint8_t)c;
  drop_y[i] = y;
}

void weather_sky(void)
{
  if (!raining) {
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
  raining = weather == WEATHER_RAIN;

  weather_sky();
  if (!raining)
    return;

  vic4_set_entry(RAIN_NEAR, 220, 226, 235);
  vic4_set_entry(RAIN_FAR, 150, 158, 170);

  // Scattered down the screen, or the first frame is one horizontal band of
  // rain falling in step.
  {
    uint8_t i;

    for (i = 0; i < RAIN_DROPS; i++)
      drop_place(i, (uint8_t)(weather_rnd() & 127));
  }
}

void weather_rain_draw(uint32_t base)
{
  uint8_t i;

  if (!raining)
    return;

  for (i = 0; i < RAIN_DROPS; i++) {
    int16_t y = (int16_t)drop_y[i] + DROP_SPEED(i);
    uint8_t xin, len, colour, k;
    uint8_t __far *p;
    int16_t at;

    if (y >= FB_HEIGHT)
      drop_place(i, (uint8_t)(y - FB_HEIGHT));  // off the bottom, start again
    else
      drop_y[i] = (uint8_t)y;

    y = drop_y[i];
    len = DROP_LENGTH(i);
    if (y + len > FB_HEIGHT)
      len = (uint8_t)(FB_HEIGHT - y);  // clipped at the bottom of the view

    xin = (uint8_t)((drop_col[i] * VX_STEP) & 7);
    colour = DROP_COLOUR(i);
    p = (uint8_t __far *)(base + voxel_column_offset(drop_col[i]));
    at = y * 8;

    for (k = 0; k < len; k++) {
      p[at] = colour;
      at += 8;
      // The lean. Column strips put the next pixel across in the very next
      // byte until the strip ends, which is the same step src/sprite.c makes
      // along a figure's width.
      if (k & 1) {
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
