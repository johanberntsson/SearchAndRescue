#include "voxel.h"

#include "loader.h"
#include "vic4.h"

// The ray march walks in bands, doubling the step each band, so near ground is
// sampled finely and the far distance cheaply. All three bands are the same
// length, which keeps the step a shift rather than a multiply.
#define BAND_STEPS 32
#define BANDS      3
#define NSTEPS     (BAND_STEPS * BANDS)
#define Z_NEAR     256 // 8.8 cells
#define Z_STEP0    128 // 8.8 cells, doubled at each band boundary

// Vertical exaggeration. Heights are in original-map units and the colour and
// height maps were downsampled 4x, so a height unit is a quarter of a cell:
// the 4 is already folded in here.
#define SCALE_H 25

// tan(30 degrees) * 256: half of a 60 degree horizontal field of view.
#define TAN_HALF_FOV 148

// __far pointers index with an int16_t, which cannot reach the top half of a
// 64K map. Biasing the base by 32K and the offset by the same amount puts the
// whole map in range of a signed index.
#define MAP_BIAS 0x8000

static const int16_t sin_quarter[65] = {
    0,   6,   13,  19,  25,  31,  38,  44,  50,  56,  62,  68,  74,
    80,  86,  92,  98,  104, 109, 115, 121, 126, 132, 137, 142, 147,
    152, 157, 162, 167, 172, 177, 181, 185, 190, 194, 198, 202, 206,
    209, 213, 216, 220, 223, 226, 229, 231, 234, 237, 239, 241, 243,
    245, 247, 248, 250, 251, 252, 253, 254, 255, 255, 256, 256, 256,
};

static int16_t tan_tab[FB_WIDTH]; // 8.8 offset from the view axis per column
static uint16_t inv_z[NSTEPS];    // SCALE_H / z, in 8.8
static uint8_t sky[FB_HEIGHT];

static const uint8_t __far *height_map;
static const uint8_t __far *colour_map;

int16_t voxel_sin(uint8_t angle)
{
  uint8_t q = angle & 63;
  int16_t v = (angle & 64) ? sin_quarter[64 - q] : sin_quarter[q];

  return (angle & 128) ? -v : v;
}

static int16_t voxel_cos(uint8_t angle)
{
  return voxel_sin(angle + 64);
}

// Signed index of map cell (x >> 8, y >> 8), biased to match the map pointers.
static int16_t map_index(uint16_t x, uint16_t y)
{
  return (int16_t)(((y & 0xFF00) | (x >> 8)) ^ MAP_BIAS);
}

void voxel_init(void)
{
  uint16_t k, z = Z_NEAR, step = Z_STEP0;
  uint8_t x, y, band, i;

  height_map = (const uint8_t __far *)(HEIGHTMAP + MAP_BIAS);
  colour_map = (const uint8_t __far *)(COLOURMAP + MAP_BIAS);

  for (x = 0; x < FB_WIDTH; x++)
    tan_tab[x] = (int16_t)(((int16_t)x - FB_WIDTH / 2) * TAN_HALF_FOV) / (FB_WIDTH / 2);

  k = 0;
  for (band = 0; band < BANDS; band++) {
    for (i = 0; i < BAND_STEPS; i++) {
      inv_z[k++] = (uint16_t)(((uint32_t)SCALE_H << 16) / z);
      z += step;
    }
    step <<= 1;
  }

  for (y = 0; y < FB_HEIGHT; y++)
    sky[y] = SKY_BASE + (uint8_t)(((uint16_t)y * SKY_SHADES) / FB_HEIGHT);
}

uint8_t voxel_ground(uint16_t x, uint16_t y)
{
  return height_map[map_index(x, y)];
}

static void column(uint8_t __far *fb, const camera *cam, int16_t dirx, int16_t diry)
{
  uint16_t px = cam->x, py = cam->y;
  int16_t stepx = dirx >> 1, stepy = diry >> 1;
  uint8_t ybuf = FB_HEIGHT;
  uint8_t band, i, y;
  uint16_t k = 0;

  for (band = 0; band < BANDS; band++) {
    for (i = 0; i < BAND_STEPS; i++, k++) {
      int16_t idx, ys;
      uint8_t colour;
      uint8_t __far *p;

      px += stepx;
      py += stepy;
      idx = map_index(px, py);

      ys = cam->horizon +
           (int16_t)(((int32_t)(cam->height - height_map[idx]) * inv_z[k]) >> 8);
      if (ys >= (int16_t)ybuf)
        continue; // hidden behind something nearer
      if (ys < 0)
        ys = 0;

      colour = colour_map[idx];
      p = fb + (int16_t)ys * 8;
      for (y = (uint8_t)ys; y < ybuf; y++) {
        *p = colour;
        p += 8;
      }
      ybuf = (uint8_t)ys;
      if (ybuf == 0)
        return; // column is full, nothing further can show
    }
    stepx <<= 1;
    stepy <<= 1;
  }

  // Whatever the terrain did not reach is sky. Between the spans above and
  // this, every pixel of the column is written exactly once, so the frame
  // needs no clearing pass.
  {
    uint8_t __far *p = fb;
    for (y = 0; y < ybuf; y++) {
      *p = sky[y];
      p += 8;
    }
  }
}

void voxel_render(uint32_t base, const camera *cam)
{
  int16_t cs = voxel_cos(cam->angle);
  int16_t sn = voxel_sin(cam->angle);
  uint8_t x;

  for (x = 0; x < FB_WIDTH; x++) {
    int16_t t = tan_tab[x];
    // Not normalised on purpose: the ray lengthening towards the edges is
    // exactly what projects onto a flat screen without fisheye distortion.
    int16_t dirx = cs - (int16_t)(((int32_t)sn * t) >> 8);
    int16_t diry = sn + (int16_t)(((int32_t)cs * t) >> 8);

    column((uint8_t __far *)FB_COLUMN(base, x), cam, dirx, diry);
  }
}
