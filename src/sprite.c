#include "sprite.h"

#include "loader.h"
#include "vic4.h"

// The figure's world height, in heightmap units -- of which there are four to
// a map cell. Nothing here is to scale: the world is 256 cells over 28 km, so
// a map cell is a hundred metres and a life-sized survivor would be a
// fraction of a pixel from anywhere worth flying. This is really the number
// that decides how far away one can still be spotted, and it is meant to be
// set by eye. 12 units is three map cells, which reads as a person-sized
// figure against terrain whose hills are 100 units tall.
#define SPR_WORLD_H 12

// Nearer than this the projection runs out of screen -- the scale goes as
// 1/z, and at two cells the figure is already taller than the view. You have
// flown into them; there is nothing useful left to draw.
#define SPR_Z_NEAR 512  // 8.8 map cells

// Past the end of the march. Beyond it the terrain the sprite stands on is
// not drawn either, so there is nothing to stand on.
#define SPR_Z_FAR 30720

// The stored figure, straight off the disk: a four byte header (width,
// height, two spare) and then the pixels column by column, which is the order
// they are drawn in. tools/convmap.py fixes the height and lets the width
// follow the figure's proportions, so only the maximum is known here.
#define SPR_MAX_W 32
#define SPR_MAX_H 32

static uint8_t spr_file[4 + SPR_MAX_W * SPR_MAX_H];
static uint8_t spr_w, spr_h;

static uint16_t spr_x, spr_y;  // 8.8 map position
static int16_t spr_ground;     // heightmap units under it

// Where this frame's projection put it, in screen pixels, and how far away it
// was along the view axis.
static int16_t spr_left, spr_top, spr_wide, spr_high;
static int16_t spr_depth;
static uint8_t spr_shown;

// Close enough to report, in 8.8 map cells. Ten cells draws the figure about
// 25 pixels tall, which is plainly a person rather than a red speck.
#define SPR_REPORT_RANGE 2560

// Which source column each destination column samples, and which source row
// each destination row does. Built once a frame so that the drawing loop is a
// table lookup per pixel rather than a multiply and a divide.
//
// FB_HEIGHT covers both: SPR_Z_NEAR keeps the drawn figure no taller than the
// view, and no wider, since it is taller than it is wide -- sprite_prepare
// clamps the width to this in case a future pose is not.
static uint8_t src_col[FB_HEIGHT];
static uint8_t src_row[FB_HEIGHT];

int sprite_load(void)
{
  int got = load_small("TERRAIN.SPR", spr_file, sizeof spr_file);

  if (got < 4)
    return -1;

  spr_w = spr_file[0];
  spr_h = spr_file[1];
  if (spr_w > SPR_MAX_W || spr_h > SPR_MAX_H ||
      (uint16_t)spr_w * spr_h + 4 > (uint16_t)got) {
    spr_w = 0;
    return -1;
  }
  return 0;
}

void sprite_place(uint16_t x, uint16_t y)
{
  spr_x = x;
  spr_y = y;
  spr_ground = voxel_ground(x, y);
}

// Fill a table mapping `count` destination steps onto `span` source ones, as
// an 8.8 accumulator: the last entry is span * (count - 1) / count, which is
// always below span, so nothing walks off the end of the figure.
static void ramp(uint8_t *table, int16_t count, uint8_t span)
{
  uint16_t acc = 0, step = ((uint16_t)span << 8) / (uint16_t)count;
  int16_t i;

  for (i = 0; i < count; i++) {
    table[i] = (uint8_t)(acc >> 8);
    acc += step;
  }
}

uint8_t sprite_prepare(const camera *cam, int16_t cs, int16_t sn)
{
  int16_t dx = (int16_t)(spr_x - cam->x);
  int16_t dy = (int16_t)(spr_y - cam->y);
  int32_t z, u;
  int16_t centre, feet;
  uint16_t inv_z;

  spr_shown = 0;
  if (!spr_w)
    return VOXEL_NO_STEP;

  // The camera's own axes: the view runs along (cos, sin) and the ray for
  // screen column i leaves it sideways by tan_tab[i], so the lateral axis is
  // (-sin, cos). Both differences are 16-bit, which is the whole world across,
  // so a survivor behind the map's edge comes out in front through the wrap
  // exactly as the terrain does.
  //
  // Held in 32 bits only because two products of a full-range difference can
  // just overflow when added; both are back inside 16 bits by the time they
  // are used.
  if (dx > SPR_Z_FAR || dx < -SPR_Z_FAR || dy > SPR_Z_FAR || dy < -SPR_Z_FAR)
    return VOXEL_NO_STEP;
  z = (int32_t)voxel_mul_shift8(dx, cs) + voxel_mul_shift8(dy, sn);
  if (z < SPR_Z_NEAR || z > SPR_Z_FAR)
    return VOXEL_NO_STEP;
  u = (int32_t)voxel_mul_shift8(dy, cs) - voxel_mul_shift8(dx, sn);

  // The sideways tangent is u / z, and PX_PER_TAN turns a tangent into
  // columns; doing it as one divide keeps the precision that u / z on its own
  // would throw away at any distance.
  centre = (int16_t)(FB_WIDTH / 2 + (u * PX_PER_TAN) / z);

  inv_z = voxel_inv_z((uint16_t)z);
  spr_high = (int16_t)(((uint32_t)SPR_WORLD_H * inv_z) >> 8);
  if (spr_high < 2 || spr_high > FB_HEIGHT)
    return VOXEL_NO_STEP;  // a single pixel is not worth the setup
  spr_wide = spr_high * spr_w / spr_h;
  if (spr_wide < 1)
    spr_wide = 1;
  else if (spr_wide > FB_HEIGHT)
    spr_wide = FB_HEIGHT;  // src_col is only that long

  // The feet stand on the terrain, and the same projection the march uses
  // puts that point on a row: ys = horizon + (camera height - height) / z.
  feet = (int16_t)(((int32_t)(cam->height - spr_ground) * inv_z) >> 8);
  spr_top = cam->horizon + feet - spr_high;
  spr_left = centre - spr_wide / 2;

  if (spr_left >= FB_WIDTH || spr_left + spr_wide <= 0 ||
      spr_top >= FB_HEIGHT || spr_top + spr_high <= 0)
    return VOXEL_NO_STEP;

  ramp(src_col, spr_wide, spr_w);
  ramp(src_row, spr_high, spr_h);
  spr_depth = (int16_t)z;
  spr_shown = 1;

  return voxel_depth_step((uint16_t)z);
}

uint8_t sprite_reportable(void)
{
  return spr_shown && spr_depth <= SPR_REPORT_RANGE;
}

void sprite_draw(uint32_t base)
{
  const uint8_t *pixels = spr_file + 4;
  int16_t x, xe, ys, ye;
  uint8_t __far *p;

  if (!spr_shown)
    return;

  x = spr_left < 0 ? 0 : spr_left;
  xe = spr_left + spr_wide;
  if (xe > FB_WIDTH)
    xe = FB_WIDTH;
  ys = spr_top < 0 ? 0 : spr_top;
  ye = spr_top + spr_high;
  if (ye > FB_HEIGHT)
    ye = FB_HEIGHT;

  // Column strips: a screen column is eight bytes apart down its own strip,
  // and the next column is the very next byte until the strip ends. Stepping
  // that keeps the one 16-bit multiply FB_COLUMN needs out of the loop.
  p = (uint8_t __far *)FB_COLUMN(base, (uint16_t)x);

  for (; x < xe; x++) {
    const uint8_t *column = pixels + (uint16_t)src_col[x - spr_left] * spr_h;
    const uint8_t *row = src_row + (ys - spr_top);
    // Terrain nearer than the sprite covers everything from this row down.
    int16_t clip = voxel_yclip[x / VX_STEP];
    int16_t y, last = ye < clip ? ye : clip;
    int16_t at = ys * 8;

    for (y = ys; y < last; y++) {
      uint8_t v = column[*row++];

      if (v)  // 0 is the figure's transparent index, never a colour
        p[at] = v;
      at += 8;
    }
    p += (x & 7) == 7 ? FB_STRIDE - 7 : 1;
  }
}
