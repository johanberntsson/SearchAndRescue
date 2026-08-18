#include "voxel.h"

#include <mega65.h>

#include "dma.h"
#include "loader.h"
#include "profile.h"
#include "sprite.h"
#include "vic4.h"
#include "weather.h"

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

// The three per-ray tables live in the low free RAM at $1600 rather than in
// the 32K the program shares -- 800 bytes of it at VX_COLS 160, 1600 at 320.
// They are built by voxel_init and rewritten every frame, long after the last
// Kernal disk call, which is what makes them eligible; see LOW_FREE.
LOW_FREE static int16_t tan_tab[VX_COLS]; // 8.8 offset from the view axis per ray

// Byte offset into a framebuffer of the pixel one row *below* the bottom of
// each column: where the span fill starts counting down from. Precomputed
// because working it out needs a multiply by FB_STRIDE, and the compiler
// builds that out of a shift loop made of jsr fragments.
LOW_FREE static uint16_t col_top[VX_COLS];

// Shared with src/voxel_asm.s, which indexes both with an 8-bit register, so
// neither may grow past 256 bytes.
uint16_t vx_inv_z[NSTEPS];  // SCALE_H / z, in 8.8
int16_t vx_horiz[NSTEPS];   // the horizon, biased per step: see CAM_BIAS

// Sub-cell plane lookup, indexed by a position fraction byte and OR'd
// together to make the map pointer's bank byte. A table rather than shifts
// because the shift count depends on the map size and the cost must not:
// this is one `lda abs,x` either way, for four planes or sixteen.
//
// In the low free RAM at $1600, which is what retiring WIDE=1 paid for: a
// kilobyte is 38% of everything the 32K had left for data. They are read by
// the inner loop 10240 times a frame, so they have to stay *near* -- $1600 is
// ordinary chip RAM the CPU addresses as cheaply as any other, so this costs
// nothing per sample. Built by voxel_init, long after the last disk call.
#if COL_AXIS > 1
LOW_FREE uint8_t vx_cplane_x[256], vx_cplane_y[256];
#endif
#if HGT_AXIS > 1
LOW_FREE uint8_t vx_hplane_x[256], vx_hplane_y[256];
#endif

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

// Billboard depth clip. vx_zclip is the value of the march's step index Y at
// which the y buffer is to be sampled -- twice the step number, because the
// tables Y walks are 16-bit -- and the assembly parks it at VOXEL_NO_STEP
// once it has taken the sample, which is both the "already done" flag and the
// "never do it" value.
__zpage uint8_t vx_zclip, vx_yclip;

LOW_FREE uint8_t voxel_yclip[VX_COLS];
static uint8_t clip_y;  // vx_zclip for every column of the current frame
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

#if COL_AXIS > 1 || HGT_AXIS > 1
// The top log2(axis) bits of each fraction byte say which sub-cell of the
// map cell the sample landed in. The y table carries the map's own bank byte
// as well, so the two lookups OR straight into the pointer.
static void build_planes(uint8_t *xtab, uint8_t *ytab, uint8_t axis, uint8_t bank)
{
  uint16_t i;
  uint8_t shift = 0;

  while ((uint8_t)(1 << shift) < axis)
    shift++;

  for (i = 0; i < 256; i++) {
    uint8_t sub = (uint8_t)(i >> (8 - shift));
    xtab[i] = sub;
    ytab[i] = (uint8_t)(bank + sub * axis);
  }
}
#endif

// Point the march at one of the resident maps. **This is the whole of what a
// map is to the renderer**: two pointers' high bytes and the plane tables that
// carry the bank byte, so several maps can sit in attic RAM at once and a
// mission chooses between them for 512 bytes of table and no pixels at all.
//
// The 256x256 case has no plane tables -- the bank goes straight in the
// pointer -- and cannot be in attic anyway, so it is always slot 0.
void voxel_set_map(uint8_t slot)
{
  uint32_t colour = MAP_COLOURMAP(slot);
#if HGT_SIZE > 256
  uint32_t height = MAP_HEIGHTMAP(slot);
#else
  uint32_t height = HEIGHTMAP;
  (void)slot;
#endif

  height_map = (const uint8_t __far *)(height + MAP_BIAS);

  // The assembly rewrites only the low two bytes of these, so the bank and
  // megabyte set here stand for the whole run.
  vx_hptr[2] = (uint8_t)(height >> 16);
  vx_hptr[3] = (uint8_t)(height >> 24);
  vx_cptr[3] = (uint8_t)(colour >> 24);
#if COL_AXIS > 1
  // Byte 2 is the plane, picked per span by the assembly.
  build_planes(vx_cplane_x, vx_cplane_y, COL_AXIS, (uint8_t)(colour >> 16));
#else
  vx_cptr[2] = (uint8_t)(colour >> 16);
#endif
#if HGT_AXIS > 1
  build_planes(vx_hplane_x, vx_hplane_y, HGT_AXIS, (uint8_t)(height >> 16));
#endif
}

void voxel_init(void)
{
  uint16_t k, x, z = Z_NEAR, step = Z_STEP0;
  uint8_t y, band, i;

  voxel_set_map(0);
  vx_bands = BANDS;
  vx_bandsteps = BAND_STEPS;

  for (x = 0; x < VX_COLS; x++) {
    // The ray's own index spans the field of view; the pixel it starts at is
    // VX_STEP times that, because at 160 columns each ray owns two pixels.
    uint16_t px = x * VX_STEP;

    tan_tab[x] = (int16_t)(((int16_t)x - VX_COLS / 2) * TAN_HALF_FOV) / (VX_COLS / 2);
    // See FB_COLUMN in vic4.h, plus one whole column of rows.
    col_top[x] = (px >> 3) * FB_STRIDE + (px & 7) + FB_STRIDE;
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

uint16_t voxel_column_offset(uint16_t raycol)
{
  // col_top addresses the row one *below* the bottom of the column, because
  // that is where the span fill counts down from. Anything drawing downwards
  // from the top wants one strip less.
  return col_top[raycol] - FB_STRIDE;
}

uint8_t voxel_ground(uint16_t x, uint16_t y)
{
#if HGT_AXIS > 1
  // Above 256x256 the map is planes on 64K boundaries and the sub-cell picks
  // the bank byte, exactly as the inner loop does it -- otherwise this reads
  // plane 0, a point sample of the finer map, and reports a height the
  // renderer never draws. On a peak that is several units out, which is
  // several pixels of a billboard standing on it.
  uint32_t plane = (uint32_t)(vx_hplane_x[(uint8_t)x] | vx_hplane_y[(uint8_t)y]);
  const uint8_t __far *m =
      (const uint8_t __far *)((HEIGHTMAP & 0xFF000000UL) + (plane << 16) + MAP_BIAS);

  return m[map_index(x, y)];
#else
  return height_map[map_index(x, y)];
#endif
}

int16_t voxel_mul_shift8(int16_t a, int16_t b)
{
  return mul_shift8(a, b);
}

uint16_t voxel_inv_z(uint16_t z)
{
  return (uint16_t)(((uint32_t)SCALE_H << 16) / z);
}

uint8_t voxel_depth_step(uint16_t z)
{
  uint16_t zk = Z_NEAR, step = Z_STEP0;
  uint8_t band, i, k = 0;

  // The same schedule voxel_init lays the inv_z table out with; walking it is
  // 64 comparisons once a frame, against carrying a second copy of it around.
  for (band = 0; band < BANDS; band++) {
    for (i = 0; i < BAND_STEPS; i++) {
      if (zk >= z)
        return k;
      zk += step;
      k++;
    }
    step <<= 1;
  }
  return VOXEL_NO_STEP;
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
  vx_zclip = clip_y;
  vx_yclip = FB_HEIGHT;  // nothing in front of the billboard here
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
  uint16_t x;  // 320 rays will not fit a byte
  uint8_t step = sprite_prepare(cam, cs, sn);

  // Y walks the 16-bit step tables, so it counts by two. VOXEL_NO_STEP is
  // past anything it reaches and turns the snapshot off for the whole frame.
  clip_y = step == VOXEL_NO_STEP ? VOXEL_NO_STEP : (uint8_t)(step * 2);

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

  for (x = 0; x < VX_COLS; x++) {
    uint16_t t0 = PROF_NOW();
    int16_t t = tan_tab[x];
    // Not normalised on purpose: the ray lengthening towards the edges is
    // exactly what projects onto a flat screen without fisheye distortion.
    int16_t dirx = cs - mul_shift8(sn, t);
    int16_t diry = sn + mul_shift8(cs, t);

    column(base_lo + col_top[x], cam, dirx, diry);
    // The assembly takes the snapshot at the first span behind the billboard
    // and parks vx_zclip past the end of the march to say it has. If it never
    // got there -- the column filled first, or drew nothing behind the
    // billboard -- the y buffer never moved again, so the final one is the
    // answer.
    voxel_yclip[x] = vx_zclip == VOXEL_NO_STEP ? vx_yclip : vx_ybuf;
    PROF_ADD(P_COLUMN, t0);
  }

  {
    uint16_t t0 = PROF_NOW();

    sprite_draw(base);
    // Last, so the rain is in front of the terrain and the figure both. It
    // needs no clearing pass of its own: the sky DMA at the top of the next
    // frame repaints every pixel before anything reads it.
    weather_draw(base);
    PROF_ADD(P_SPRITE, t0);
  }

#if PROFILE_DETAIL
  profile_count(C_SAMPLE, n_sample);
  profile_count(C_SPAN, n_span);
  profile_count(C_SPANPIX, n_spanpix);
  profile_count(C_SKYPIX, n_skypix);
  n_sample = n_span = n_spanpix = n_skypix = 0;
#endif
}
