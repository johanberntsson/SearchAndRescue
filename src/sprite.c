#include "sprite.h"

#include "dma.h"
#include "loader.h"
#include "thermal.h"
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

// A stored figure, straight off the disk: a four byte header (width, height,
// two spare) and then the pixels column by column, which is the order they are
// drawn in. tools/convmap.py scales each one to fit a SPR_MAX box, longest
// side first -- a lone standing hiker comes out full height and a scene of two
// people full width -- so only the box is known here.
#define SPR_MAX  32
#define SPR_SLOT (4 + SPR_MAX * SPR_MAX)

// The figure is held in the loader's staging buffer rather than in one of its
// own: the two are never live at the same time, and 32K has no room for a
// kilobyte twice. See LOAD_STAGING for the invariant that makes it safe.
#define spr_file load_staging
#if SPR_SLOT > LOAD_STAGING
#error "LOAD_STAGING is too small to hold a figure"
#endif

// Bank 1 is full to the byte: the figures, then the overview maps, then the
// colour RAM alias. Both of these were slack before the campaign made the
// counts a choice, and neither is any more.
#if SPRITE_STORE + SPRITE_MAX * SPR_SLOT > OVERVIEW_STORE
#error "SPRITE_MAX figures do not fit below OVERVIEW_STORE in bank 1"
#endif
#if OVERVIEW_STORE + MAP_SLOTS * OVERVIEW_BYTES > 0x1F800UL
#error "MAP_SLOTS overview maps run into the colour RAM alias at $1F800"
#endif

// The figures on disk, in the order the campaign numbers them: the first is
// TERRAIN.SPR and the rest are .SP2, .SP3, which is what tools/convmap.py
// names them and how many there are is campaign_figures(). One buffer patched
// in place, because there is no display to printf a name onto by the time
// this runs. They all live in bank 1 (SPRITE_STORE) and only the flight's own
// is copied down here.
static char spr_name[] = "TERRAIN.SPR";

static const char *figure_file(uint8_t n)
{
  spr_name[10] = n ? (char)('1' + n) : 'R';
  return spr_name;
}

// How many were read, so a mission naming one that is not there draws nothing
// rather than a slot of whatever bank 1 held.
static uint8_t spr_count;

static uint8_t spr_w, spr_h;

// Which figure sprite_select brought down, so that sprite_thermal can put the
// unburnt pixels back by fetching it again.
static uint8_t spr_chosen;
static uint8_t spr_hot;

// A figure the optical camera cannot see: buried, and drawn only while the
// thermal camera is armed. It is checked in sprite_prepare rather than in
// sprite_draw so that a buried figure is not "on screen" either -- no report
// can be filed on somebody the pilot has not been shown.
static uint8_t spr_hidden;

static uint16_t spr_x, spr_y;  // 8.8 map position
static int16_t spr_ground;     // heightmap units under it

// Where this frame's projection put it, in screen pixels, and how far away it
// was along the view axis.
static int16_t spr_left, spr_top, spr_wide, spr_high;
static int16_t spr_depth;
static uint8_t spr_shown;
static uint8_t spr_near;  // within SPR_DROP_RANGE, camera pointing anywhere

// Close enough to report, in 8.8 map cells. Ten cells draws the figure about
// 25 pixels tall, which is plainly a person rather than a red speck.
#define SPR_REPORT_RANGE 2560

// Close enough to drop something to, which is a shorter reach than a camera's
// and does not care where the camera is pointing: a delivery is made by flying
// to the spot, not by looking at it. Five cells is half a kilometre at the
// map's scale -- close enough that the pilot has had to go and find them.
#define SPR_DROP_RANGE 1280

// Which source column each destination column samples, and which source row
// each destination row does. Built once a frame so that the drawing loop is a
// table lookup per pixel rather than a multiply and a divide.
//
// FB_HEIGHT covers both: SPR_Z_NEAR keeps the drawn figure no taller than the
// view, and no wider, since it is taller than it is wide -- sprite_prepare
// clamps the width to this in case a future pose is not.
// In the low free RAM at $1600 with the renderer's own tables: they are
// rebuilt every frame a figure is in view and never touched while the disk is
// being read, which is what LOW_FREE requires.
LOW_FREE static uint8_t src_col[FB_HEIGHT];
LOW_FREE static uint8_t src_row[FB_HEIGHT];

const char *sprite_load(uint8_t figures)
{
  uint8_t n;

  spr_w = 0;  // nothing selected until a flight asks for one
  spr_count = figures > SPRITE_MAX ? SPRITE_MAX : figures;
  for (n = 0; n < spr_count; n++) {
    int got = load_small(figure_file(n), spr_file, sizeof spr_file);

    if (got < 4 || spr_file[0] > SPR_MAX || spr_file[1] > SPR_MAX ||
        (uint16_t)spr_file[0] * spr_file[1] + 4 > (uint16_t)got)
      return figure_file(n);

    // Straight back up out of the near buffer, whole slot and all: the tail
    // past the figure is never read, so there is nothing to be gained by
    // copying only the bytes that matter.
    dma_copy((uint32_t)(uint16_t)spr_file,
             SPRITE_STORE + (uint32_t)n * SPR_SLOT, SPR_SLOT);
  }
  return 0;
}

void sprite_select(uint8_t figure)
{
  spr_hot = 0;
  spr_hidden = 0;
  if (figure >= spr_count) {
    spr_w = 0;  // draws nothing, rather than drawing the wrong thing
    spr_chosen = SPRITE_MAX;
    return;
  }
  spr_chosen = figure;
  dma_copy(SPRITE_STORE + (uint32_t)figure * SPR_SLOT,
           (uint32_t)(uint16_t)spr_file, SPR_SLOT);
  spr_w = spr_file[0];
  spr_h = spr_file[1];
}

void sprite_show(uint8_t shown)
{
  spr_hidden = !shown;
}

// What the thermal camera does to a figure, and it is not a tint: every pixel
// that is not the transparent 0 becomes the one hot index, so what is drawn is
// a heat signature and not a person in a coat. Putting it back is the DMA out
// of bank 1 that sprite_select already does, so nothing here has to remember
// what any pixel used to be.
//
// A kilobyte a toggle, against a test per pixel in a drawing loop that already
// costs 170 cycles each -- and the loop never learns the camera exists.
void sprite_thermal(uint8_t hot)
{
  uint8_t *pixels = spr_file + 4;
  uint16_t n, count = (uint16_t)spr_w * spr_h;

  if (!spr_w || !hot == !spr_hot)
    return;
  spr_hot = hot;

  if (!hot) {
    dma_copy(SPRITE_STORE + (uint32_t)spr_chosen * SPR_SLOT,
             (uint32_t)(uint16_t)spr_file, SPR_SLOT);
    return;
  }
  for (n = 0; n < count; n++)
    if (pixels[n])
      pixels[n] = THERMAL_HOT;
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
  spr_near = 0;
  if (!spr_w || spr_hidden)
    return VOXEL_NO_STEP;

  // How near, before anything is projected: a delivery is judged on this and
  // has to be answerable on the frames where the figure is off screen or
  // behind a hill. A square rather than a circle -- it saves two multiplies
  // and nobody can tell the corners from the edge of a search radius.
  spr_near = dx < SPR_DROP_RANGE && dx > -SPR_DROP_RANGE &&
             dy < SPR_DROP_RANGE && dy > -SPR_DROP_RANGE;

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

uint8_t sprite_in_range(void)
{
  return spr_near;
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
