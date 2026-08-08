#include "voxel.h"

#include <mega65.h>

#include "loader.h"
#include "profile.h"
#include "vic4.h"

// The ray march walks in bands, doubling the step each band, so near ground is
// sampled finely and the far distance cheaply. All three bands are the same
// length, which keeps the step a shift rather than a multiply.
#define BAND_STEPS 16
#define BANDS      4
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

// Shared with src/voxel_asm.s, which indexes both with an 8-bit register, so
// neither may grow past 256 bytes.
uint16_t vx_inv_z[NSTEPS];  // SCALE_H / z, in 8.8
uint8_t vx_sky[FB_HEIGHT];

// The assembly column routine's parameter block. Zero page because every one
// of these is touched in the inner loop.
uint8_t __far *__attribute__((zpage)) vx_hptr;    // heightmap cell
uint8_t __far *__attribute__((zpage)) vx_cptr;    // colourmap cell
uint8_t __far *__attribute__((zpage)) vx_fb;      // walking write pointer
uint8_t __far *__attribute__((zpage)) vx_fbbase;  // top of the column
__zpage uint16_t vx_px, vx_py;
__zpage int16_t vx_stepx, vx_stepy;
__zpage int16_t vx_camh, vx_horizon;
__zpage int16_t vx_ys;
__zpage uint8_t vx_ybuf, vx_tmp;
__zpage uint8_t vx_bands, vx_bandsteps, vx_band, vx_step;

void voxel_column_asm(void);

static const uint8_t __far *height_map;

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
static inline int16_t map_index(uint16_t x, uint16_t y)
{
  return (int16_t)(((y & 0xFF00) | (x >> 8)) ^ MAP_BIAS);
}

// (a * b) >> 8 on the 45GS02's multiplier. The C compiler's own 32-bit
// multiply measures 2203 cycles against 85 for this, and the inner loop runs
// one per heightmap sample, so it is most of the frame.
//
// The multiplier is unsigned, but sign-extending both operands into 32 bits
// leaves the low 32 bits of the product equal to the signed product, which is
// all this needs. Bytes 1 and 2 of the result are the >> 8, so there is no
// shift to do either.
static inline int16_t mul_shift8(int16_t a, int16_t b)
{
  MATH.multina32 = (uint32_t)(int32_t)a;
  MATH.multinb32 = (uint32_t)(int32_t)b;
  return (int16_t)((uint16_t)MATH.multout[1] | (uint16_t)MATH.multout[2] << 8);
}

void voxel_init(void)
{
  uint16_t k, z = Z_NEAR, step = Z_STEP0;
  uint8_t x, y, band, i;

  height_map = (const uint8_t __far *)(HEIGHTMAP + MAP_BIAS);

  // The assembly rewrites only the low two bytes of these, so the bank and
  // megabyte set here stand for the whole run.
  vx_hptr = (uint8_t __far *)HEIGHTMAP;
  vx_cptr = (uint8_t __far *)COLOURMAP;
  vx_bands = BANDS;
  vx_bandsteps = BAND_STEPS;

  for (x = 0; x < FB_WIDTH; x++)
    tan_tab[x] = (int16_t)(((int16_t)x - FB_WIDTH / 2) * TAN_HALF_FOV) / (FB_WIDTH / 2);

  k = 0;
  for (band = 0; band < BANDS; band++) {
    for (i = 0; i < BAND_STEPS; i++) {
      vx_inv_z[k++] = (uint16_t)(((uint32_t)SCALE_H << 16) / z);
      z += step;
    }
    step <<= 1;
  }

  for (y = 0; y < FB_HEIGHT; y++)
    vx_sky[y] = SKY_BASE + (uint8_t)(((uint16_t)y * SKY_SHADES) / FB_HEIGHT);
}

uint8_t voxel_ground(uint16_t x, uint16_t y)
{
  return height_map[map_index(x, y)];
}

static void column(uint32_t fbcol, const camera *cam, int16_t dirx, int16_t diry)
{
  vx_fbbase = (uint8_t __far *)fbcol;
  vx_px = cam->x;
  vx_py = cam->y;
  vx_stepx = dirx >> 1;
  vx_stepy = diry >> 1;
  vx_camh = cam->height;
  vx_horizon = cam->horizon;
  vx_ybuf = FB_HEIGHT;

  // The compiler uses the same multiplier for its own inlined multiplies, so
  // the top half of input B cannot be assumed to have survived the last call.
  // The assembly only ever writes the low half.
  MATH.multinb32 = 0;

  voxel_column_asm();

  PROF_COUNT(C_SKYPIX, vx_ybuf);
}

void voxel_render(uint32_t base, const camera *cam)
{
  int16_t cs = voxel_cos(cam->angle);
  int16_t sn = voxel_sin(cam->angle);
  uint8_t x;

  for (x = 0; x < FB_WIDTH; x++) {
    uint16_t t0 = PROF_NOW();
    int16_t t = tan_tab[x];
    // Not normalised on purpose: the ray lengthening towards the edges is
    // exactly what projects onto a flat screen without fisheye distortion.
    int16_t dirx = cs - mul_shift8(sn, t);
    int16_t diry = sn + mul_shift8(cs, t);

    column(FB_COLUMN(base, x), cam, dirx, diry);
    PROF_ADD(P_COLUMN, t0);
  }
}
