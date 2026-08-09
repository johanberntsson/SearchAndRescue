#include "voxel.h"

#include <mega65.h>

#include "dma.h"
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

// The height difference the projection multiplies is camh - height, which goes
// negative wherever terrain rises above the camera -- and sign-extending it
// into the multiplier cost a branch and two register writes on every single
// sample. Biasing the camera height up by CAM_BIAS makes it always positive.
//
// The bias has to come back out of the result, and 256 is chosen so that it
// comes out free and *exact*: (256 * inv_z) >> 8 is inv_z, so the correction
// is a subtraction from the horizon rather than a multiply, and since the low
// eight bits of 256 * inv_z are zero the shift loses nothing. vx_horiz holds
// horizon - inv_z[k] per step, and ys comes out bit for bit what it was.
//
// It costs headroom: the intermediate is larger by inv_z, so the camera
// height at which the 16-bit projection overflows drops from about 1310 to
// about 1050. Flight sits near 60.
#define CAM_BIAS 256

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

// Byte offset into a framebuffer of the pixel one row *below* the bottom of
// each column: where the span fill starts counting down from. Precomputed
// because working it out needs a multiply by FB_STRIDE, and the compiler
// builds that out of a shift loop made of jsr fragments.
static uint16_t col_top[FB_WIDTH];

// Shared with src/voxel_asm.s, which indexes both with an 8-bit register, so
// neither may grow past 256 bytes.
uint16_t vx_inv_z[NSTEPS];  // SCALE_H / z, in 8.8
int16_t vx_horiz[NSTEPS];   // the horizon, biased per step: see CAM_BIAS

// The assembly column routine's parameter block. Zero page because every one
// of these is touched in the inner loop.
// Map pointers. Byte arrays rather than far pointers because the assembly
// keeps the *high* bytes of the ray position in bytes 0 and 1 -- that is the
// map cell address, since the maps are 256x256 on a 64K boundary -- and the C
// side only ever sets the bank and megabyte, once, in voxel_init.
__zpage uint8_t vx_hptr[4];  // heightmap cell
__zpage uint8_t vx_cptr[4];  // colourmap cell

// Write pointer, and the one piece of state that carries between spans: it
// always addresses pixel row vx_ybuf of the current column. Each span fills
// upwards from there, so it ends up addressing row vx_ys -- which is the next
// span's vx_ybuf. That makes the whole `fbbase + ys * 8` calculation, some 70
// cycles a span, disappear.
//
// A byte array rather than a far pointer so the C side can set the low two
// bytes per column and leave the bank and megabyte alone.
__zpage uint8_t vx_fbtop[4];
// Only the fractional halves: the whole-cell halves live in vx_hptr.
__zpage uint8_t vx_px, vx_py;
__zpage int16_t vx_stepx, vx_stepy;
__zpage int16_t vx_camh;  // biased by CAM_BIAS
__zpage int16_t vx_ys;
__zpage uint8_t vx_ybuf, vx_tmp;
__zpage uint8_t vx_bands, vx_bandsteps, vx_band, vx_step;
#if PROFILE_DETAIL
// Event counts for one column, reset before the call and added up after it.
// Counting in the assembly keeps the inner loop down to one `inc` a sample.
__zpage uint8_t vx_n_sample, vx_n_span, vx_n_spanpix;

// Frame totals. profile_count() adds into a 32-bit field, which is far too
// expensive to call four times a column: doing that cost 9% of the frame.
// None of these can overflow 16 bits in one frame.
static uint16_t n_sample, n_span, n_spanpix, n_skypix;
#endif

void voxel_column_asm(void);

static const uint8_t __far *height_map;
static int16_t horizon_built = -1;  // the horizon vx_horiz was built for

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
  uint16_t k, x, z = Z_NEAR, step = Z_STEP0;
  uint8_t y, band, i;

  height_map = (const uint8_t __far *)(HEIGHTMAP + MAP_BIAS);

  // The assembly rewrites only the low two bytes of these, so the bank and
  // megabyte set here stand for the whole run.
  vx_hptr[2] = (uint8_t)(HEIGHTMAP >> 16);
  vx_hptr[3] = (uint8_t)(HEIGHTMAP >> 24);
  vx_cptr[2] = (uint8_t)(COLOURMAP >> 16);
  vx_cptr[3] = (uint8_t)(COLOURMAP >> 24);
  vx_bands = BANDS;
  vx_bandsteps = BAND_STEPS;

  for (x = 0; x < FB_WIDTH; x++) {
    tan_tab[x] = (int16_t)(((int16_t)x - FB_WIDTH / 2) * TAN_HALF_FOV) / (FB_WIDTH / 2);
    // See FB_COLUMN in vic4.h, plus one whole column of rows.
    col_top[x] = (uint16_t)(x >> 3) * FB_STRIDE + (x & 7) + FB_STRIDE;
  }

  k = 0;
  for (band = 0; band < BANDS; band++) {
    for (i = 0; i < BAND_STEPS; i++) {
      vx_inv_z[k++] = (uint16_t)(((uint32_t)SCALE_H << 16) / z);
      z += step;
    }
    step <<= 1;
  }

  // One column strip of sky, for voxel_render to DMA across the buffer. The
  // gradient runs down the screen, and within a strip the eight pixels of a
  // row are eight consecutive bytes, so a row is eight copies of one shade.
  {
    uint8_t __far *tmpl = (uint8_t __far *)FB_SKY;
    uint8_t i;

    for (y = 0; y < FB_HEIGHT; y++) {
      uint8_t shade = SKY_BASE + (uint8_t)(((uint16_t)y * SKY_SHADES) / FB_HEIGHT);
      for (i = 0; i < 8; i++)
        tmpl[(int16_t)y * 8 + i] = shade;
    }
  }
}

uint8_t voxel_ground(uint16_t x, uint16_t y)
{
  return height_map[map_index(x, y)];
}

static void column(uint16_t top, const camera *cam, int16_t dirx, int16_t diry)
{
  vx_fbtop[0] = (uint8_t)top;
  vx_fbtop[1] = (uint8_t)(top >> 8);
  vx_px = (uint8_t)cam->x;
  vx_hptr[0] = (uint8_t)(cam->x >> 8);
  vx_py = (uint8_t)cam->y;
  vx_hptr[1] = (uint8_t)(cam->y >> 8);
  vx_stepx = dirx >> 1;
  vx_stepy = diry >> 1;
  vx_camh = cam->height + CAM_BIAS;
  vx_ybuf = FB_HEIGHT;
#if PROFILE_DETAIL
  vx_n_sample = 0;
  vx_n_span = 0;
  vx_n_spanpix = 0;
#endif

  // The compiler uses the same multiplier for its own inlined multiplies, so
  // the top half of input B cannot be assumed to have survived the last call.
  // The assembly only ever writes the low half.
  MATH.multinb32 = 0;

  voxel_column_asm();

#if PROFILE_DETAIL
  n_skypix += vx_ybuf;
  n_sample += vx_n_sample;
  n_span += vx_n_span;
  n_spanpix += vx_n_spanpix;
#endif
}

void voxel_render(uint32_t base, const camera *cam)
{
  int16_t cs = voxel_cos(cam->angle);
  int16_t sn = voxel_sin(cam->angle);
  uint16_t base_lo = (uint16_t)base;
  uint16_t x;  // 320 columns will not fit a byte

  // Rebuilt only when the horizon moves, which today is never -- but pitch
  // control will move it, and this is where that gets handled.
  if (cam->horizon != horizon_built) {
    horizon_built = cam->horizon;
    for (x = 0; x < NSTEPS; x++)
      vx_horiz[x] = cam->horizon - (int16_t)vx_inv_z[x];
  }

  // Both buffers sit inside one 64K bank and no column reaches past its end,
  // so the top two bytes of the write pointer are fixed for the whole frame.
  vx_fbtop[2] = (uint8_t)(base >> 16);
  vx_fbtop[3] = (uint8_t)(base >> 24);

  // Lay the sky down over the whole buffer first and let the terrain draw
  // over it. That writes more pixels than filling only the gap above each
  // column did, but the DMA moves a byte in a couple of cycles against the
  // 25-odd the fill loop took, and it takes the sky out of the inner loop
  // altogether. It also serves as the frame's clear pass: every pixel is
  // written before anything reads it.
  {
    uint16_t t0 = PROF_NOW();
    for (x = 0; x < FB_COLS; x++)
      dma_copy(FB_SKY, base + (uint32_t)x * FB_STRIDE, FB_STRIDE);
    PROF_ADD(P_SKY, t0);
  }

  for (x = 0; x < FB_WIDTH; x++) {
    uint16_t t0 = PROF_NOW();
    int16_t t = tan_tab[x];
    // Not normalised on purpose: the ray lengthening towards the edges is
    // exactly what projects onto a flat screen without fisheye distortion.
    int16_t dirx = cs - mul_shift8(sn, t);
    int16_t diry = sn + mul_shift8(cs, t);

    column(base_lo + col_top[x], cam, dirx, diry);
    PROF_ADD(P_COLUMN, t0);
  }

#if PROFILE_DETAIL
  profile_count(C_SAMPLE, n_sample);
  profile_count(C_SPAN, n_span);
  profile_count(C_SPANPIX, n_spanpix);
  profile_count(C_SKYPIX, n_skypix);
  n_sample = n_span = n_spanpix = n_skypix = 0;
#endif
}
