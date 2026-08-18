#include "thermal.h"

#include "loader.h"
#include "sprite.h"
#include "vic4.h"
#include "voxel.h"
#include "weather.h"

// Everything the terrain can be painted in: the shared ramp's water, land,
// masonry, roof and road, then every figure's fifteen, then whatever is still
// free. Sweeping the whole run rather than the bands means maps/palette.yaml
// can grow a band without this file hearing about it -- the alternative was a
// copy of its boundaries here, which is exactly the drift the clear sky is
// restored from the palette to avoid.
//
// It stops one short of the sky, at THERMAL_HOT, and never reaches the rain's
// pair at 240/241 or the panel artwork's above them: the panel is not
// something the camera is pointed at.
//
// **Stopping short of THERMAL_HOT is not tidiness.** It sits at the top of
// this run, so a sweep that included it painted the figure's own colour cold
// along with the ground -- and the figure came out the same blue as the
// hillside it was lying on, drawn correctly and invisible. Leaving it alone is
// also what makes the restore below one call: it never changed.
#define TERRAIN_FIRST 16
#define TERRAIN_COUNT (THERMAL_HOT - TERRAIN_FIRST)

// A cold sky, from the top of the screen down to the horizon. Not black:
// black would put a hard line across the picture at the horizon, and the
// faint lift is what says the ground is nearer than the sky.
#define COLD_TOP_R 0
#define COLD_TOP_G 0
#define COLD_TOP_B 8
#define COLD_HOR_R 16
#define COLD_HOR_G 20
#define COLD_HOR_B 40

static uint8_t armed;

// The palette in attic RAM holds each channel with its nybbles reversed, for
// the registers -- see tools/convmap.py. The swap is its own inverse, so this
// is the same operation vic4_set_entry does on the way back out.
static uint8_t unswap(uint8_t v)
{
  return (uint8_t)((v & 0x0F) << 4 | v >> 4);
}

static uint8_t lerp(uint8_t a, uint8_t b, uint8_t i)
{
  return (uint8_t)(a + ((int16_t)b - a) * i / (SKY_SHADES - 1));
}

// Every terrain colour becomes a cold monochrome of *itself*, which is what
// keeps the ground readable: the shape a pilot flies by is the sun's shading,
// and taking the luminance carries all of it across. A flat blue-grey
// silhouette was the alternative and there is nothing to fly at in one.
//
// Dim on purpose, and blue-heavy. The channels are four bits on the machine,
// so what this actually spends is seven steps of blue and three of green on
// the ground -- against the near-white the figure is drawn in, which is
// fifteen and thirteen. That gap is the whole picture, and it is a gap in hue
// as much as in brightness: cold ground is blue and a body is white.
static void terrain_cold(void)
{
  const uint8_t __far *pal = loaded_palette(map_current);
  uint16_t n;

  for (n = TERRAIN_FIRST; n < TERRAIN_FIRST + TERRAIN_COUNT; n++) {
    uint8_t r = unswap(pal[n]);
    uint8_t g = unswap(pal[256 + n]);
    uint8_t b = unswap(pal[512 + n]);
    // (r + 2g + b) / 4: the usual weights rounded to shifts, because the
    // honest ones are three 16-bit multiplies and this runs 208 times.
    uint8_t lum = (uint8_t)(((uint16_t)r + g + g + b) >> 2);

    vic4_set_entry((uint8_t)n, (uint8_t)(lum >> 3), (uint8_t)(lum >> 2),
                   (uint8_t)(20 + (lum >> 2) + (lum >> 3)));
  }
}

static void sky_cold(void)
{
  uint8_t i;

  for (i = 0; i < SKY_SHADES; i++)
    vic4_set_entry((uint8_t)(SKY_BASE + i),
                   lerp(COLD_TOP_R, COLD_HOR_R, i),
                   lerp(COLD_TOP_G, COLD_HOR_G, i),
                   lerp(COLD_TOP_B, COLD_HOR_B, i));
}

void thermal_set(uint8_t on)
{
  if (!on == !armed)
    return;
  armed = on;

  if (on) {
    terrain_cold();
    sky_cold();
  } else {
    // Straight back out of the loaded palette, the whole run in one go. This
    // puts THERMAL_HOT back too, which is in the range and shipped in it.
    vic4_set_range(loaded_palette(map_current), TERRAIN_FIRST, TERRAIN_COUNT);
    // The sky belongs to the weather, not to the palette: a rainy flight is
    // overcast and the file it was loaded from is not.
    weather_sky();
  }
  // Falling weather is the weather's to colour either way -- snow left white
  // under this camera outshines the body it exists to find.
  weather_thermal(on);
  // Last, because it is the only half of this that touches pixels rather than
  // palette entries.
  sprite_thermal(on);
}

uint8_t thermal_on(void)
{
  return armed;
}
