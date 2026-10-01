#include "drone.h"

#include <mega65.h>

#include "loader.h"
#include "voxel.h"

#define SPR_L  6
#define SPR_R  7
#define SPR_LR ((1 << SPR_L) | (1 << SPR_R))

#define SPRITE_X(n)   (*(volatile uint8_t *)(0xD000 + (n) * 2))
#define SPRITE_Y(n)   (*(volatile uint8_t *)(0xD001 + (n) * 2))
#define SPRITE_XMSB   (*(volatile uint8_t *)0xD010)
#define SPRITE_ENABLE (*(volatile uint8_t *)0xD015)
#define SPRITE_YEXP   (*(volatile uint8_t *)0xD017)
#define SPRITE_MCM    (*(volatile uint8_t *)0xD01C)
#define SPRITE_XEXP   (*(volatile uint8_t *)0xD01D)
#define SPRITE_COL(n) (*(volatile uint8_t *)(0xD027 + (n)))
#define SPRHGTEN      (*(volatile uint8_t *)0xD055)
#define SPRHGHT       (*(volatile uint8_t *)0xD056)
#define SPR_PTR16     0x80

// Where it may go, in the screen's own pixels: the band between the title's
// second line (row 10) and the prompt (row 22), with a little air at each
// end, and the full width less a margin. It flies a Lissajous figure about
// the middle of that, which is what keeps it inside without ever testing an
// edge, and which starts -- at phase 0 -- exactly where the loading screen
// left it.
#define MID_X   ((320 - 32) / 2)
#define MID_Y   120
#define SWING_X 128
#define SWING_Y 28

// The VIC-II's sprite coordinates: the 40-column display starts at X 24 and
// Y 50, as the panel's text plane found.
#define ORIGIN_X 24
#define ORIGIN_Y 50

// Phases in 8.8, so the figure can be slower than a step of the sine table a
// frame: about seven seconds across and back, and a bob three times as fast.
#define STEP_X 0x00A0
#define STEP_Y 0x01E0

static uint8_t ok;
static uint8_t spin;
static uint16_t phase_x, phase_y;

int drone_load(void)
{
  int r = load_drone();

  ok = !r;
  return r;
}

static void place(int16_t x, int16_t y)
{
  uint16_t sx = (uint16_t)(x + ORIGIN_X);
  uint16_t rx = sx + 16;  // the right half, sixteen pixels on
  uint8_t msb = (uint8_t)(SPRITE_XMSB & ~SPR_LR);

  SPRITE_X(SPR_L) = (uint8_t)sx;
  SPRITE_X(SPR_R) = (uint8_t)rx;
  if (sx > 255)
    msb |= 1 << SPR_L;
  if (rx > 255)
    msb |= 1 << SPR_R;
  SPRITE_XMSB = msb;
  SPRITE_Y(SPR_L) = SPRITE_Y(SPR_R) = (uint8_t)(y + ORIGIN_Y);
}

// Point the two sprites at a frame. A 16-bit pointer is the address over 64,
// and the blocks are in the order the file has them.
static void frame(uint8_t f)
{
  volatile uint8_t __far *ptr = (volatile uint8_t __far *)DRONE_PTRS;
  uint16_t l = (uint16_t)((DRONE_DATA + (uint32_t)(f * 2) * DRONE_BLOCK) / 64);
  uint16_t r = l + DRONE_BLOCK / 64;

  ptr[SPR_L * 2] = (uint8_t)l;
  ptr[SPR_L * 2 + 1] = (uint8_t)(l >> 8);
  ptr[SPR_R * 2] = (uint8_t)r;
  ptr[SPR_R * 2 + 1] = (uint8_t)(r >> 8);
}

void drone_show(void)
{
  const uint8_t __far *pal = (const uint8_t __far *)DRONE_PAL;
  uint8_t i;

  if (!ok)
    return;

  // Its colours, twice: each sprite has sixteen entries of its own. Entry 0
  // of each is never drawn -- it is the clear colour, named in SPRITE_COL.
  for (i = 1; i < 16; i++) {
    PALETTE.red[SPR_L * 16 + i] = PALETTE.red[SPR_R * 16 + i] = pal[i];
    PALETTE.green[SPR_L * 16 + i] = PALETTE.green[SPR_R * 16 + i] = pal[16 + i];
    PALETTE.blue[SPR_L * 16 + i] = PALETTE.blue[SPR_R * 16 + i] = pal[32 + i];
  }

  // Sprites take their palette bank from SPRPALSEL; make it the one the
  // pages draw from, which is the one the entries above were written to.
  VICIV.palsel = (uint8_t)((VICIV.palsel & ~VIC4_SPRPALSEL_MASK)
                           | ((VICIV.palsel & VIC4_BTPALSEL_MASK) >> 2));
  VICIV.spr_16en |= SPR_LR;
  VICIV.bp16ens &= (uint8_t)~SPR_LR;  // bitplane mode would move the colours
  SPRITE_MCM &= (uint8_t)~SPR_LR;
  SPRITE_XEXP &= (uint8_t)~SPR_LR;
  SPRITE_YEXP &= (uint8_t)~SPR_LR;
  SPRHGTEN |= SPR_LR;
  SPRHGHT = DRONE_ROWS;  // the panel's plane sets its own 48 at every launch
  SPRITE_COL(SPR_L) = SPRITE_COL(SPR_R) = 0;

  VICIV.spr_ptradr_lsb = (uint8_t)DRONE_PTRS;
  VICIV.spr_ptradr_msb = (uint8_t)(DRONE_PTRS >> 8);
  VICIV.spr_ptradr_bnk = (uint8_t)((DRONE_PTRS >> 16) | SPR_PTR16);

  frame(spin & 1);
  drone_tick(0);
  SPRITE_ENABLE |= SPR_LR;
}

void drone_tick(uint8_t fly)
{
  if (!ok)
    return;

  // A new blade position every other frame: every frame is a blur on a
  // 50 Hz display, and every fourth looks like a slow propeller.
  spin++;
  if (!(spin & 1))
    frame((uint8_t)(spin >> 1) & 1);

  if (fly) {
    phase_x += STEP_X;
    phase_y += STEP_Y;
  }
  place(MID_X + voxel_mul_shift8(voxel_sin((uint8_t)(phase_x >> 8)), SWING_X),
        MID_Y + voxel_mul_shift8(voxel_sin((uint8_t)(phase_y >> 8)), SWING_Y));
}
