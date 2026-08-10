#include <mega65.h>

#include "dma.h"
#include "loader.h"
#include "dma.h"
#include "loader.h"
#include "panel.h"
#include "vic4.h"

#define COLOUR_RAM 0xFF80000UL

// Screen RAM: one 16-bit character number per cell, read row by row.
static uint16_t screen[2][SCREEN_ROWS * FB_COLS];

static void build_screen_tables(void)
{
  uint8_t row, col;

  for (row = 0; row < SCREEN_ROWS; row++) {
    for (col = 0; col < FB_COLS; col++) {
      uint16_t a, b;

      if (row < FB_ROWS) {
        uint16_t cell = col * FB_ROWS + row;  // column-major, see vic4.h
        a = (uint16_t)(FB_A / 64) + cell;
        b = (uint16_t)(FB_B / 64) + cell;
      } else {
        a = b = ' ';  // panel: a text character number, below $100
      }
      screen[0][row * FB_COLS + col] = a;
      screen[1][row * FB_COLS + col] = b;
    }
  }
}

void vic4_panel_char(uint8_t col, uint8_t row, uint8_t ch, uint8_t colour)
{
  uint16_t cell = (uint16_t)(FB_ROWS + row) * FB_COLS + col;
  uint8_t __far *cram = (uint8_t __far *)COLOUR_RAM;

  screen[0][cell] = ch;
  screen[1][cell] = ch;

  // Two colour RAM bytes per cell in 16-bit character mode: attributes then
  // the foreground colour, which is a full 8-bit palette index.
  cram[(int16_t)(cell * 2)] = 0;
  cram[(int16_t)(cell * 2 + 1)] = colour;
}

void vic4_panel_tile(uint8_t col, uint8_t row, uint16_t charnum)
{
  uint16_t cell = (uint16_t)(FB_ROWS + row) * FB_COLS + col;
  uint8_t __far *cram = (uint8_t __far *)COLOUR_RAM;

  screen[0][cell] = charnum;
  screen[1][cell] = charnum;

  // A full-colour character carries a palette index per pixel, so colour RAM
  // holds no ink for it -- only attributes, and none of those are wanted.
  cram[(int16_t)(cell * 2)] = 0;
  cram[(int16_t)(cell * 2 + 1)] = 0;
}

// The crosshair over the overview map.
//
// A hardware sprite was the obvious way and does not work here. The VIC-IV's
// SPRPTRADR is ignored -- 8-bit pointers had no effect and SPR_PTR16
// segfaults xemu outright -- so the sprite pointer still comes from the
// legacy screen+$3F8, which in this screen layout falls at row 12 column 28,
// in the middle of the 3D view, where the character number is a framebuffer
// tile and cannot be given a useful value. Verified by dumping memory: the
// bitmap was where we put it and the VIC was reading somewhere else entirely.
// Worth retrying on real hardware, where SPRPTRADR most likely does work.
//
// Drawing into the map's own pixels costs eight byte writes and a restore of
// the eight before them, and works everywhere. Palette 240 is reserved by
// tools/convmap.py for exactly this kind of overlay and is white; a
// full-colour character is a byte per pixel, so any of the 256 entries is
// available here.
#define CROSS_INK 240

// A pristine copy of the tiles, so the crosshair can be lifted by putting the
// map back rather than by remembering what each pixel used to be. Restoring
// from saved pixels is the obvious way and was tried first: it corrupted the
// whole map within a few hundred frames, every byte of it eventually. A whole
// 1024-byte DMA is 60 microseconds of a 77 millisecond frame -- nothing -- and
// there is no state to get wrong.
#define OVERVIEW_CLEAN (OVERVIEW + 0x800)

void vic4_overview_ready(void)
{
  dma_copy(OVERVIEW, OVERVIEW_CLEAN, OVERVIEW_BYTES);
}

// One pixel of the crosshair, clipped to the map. int16_t throughout on
// purpose: the same routine written with int8_t coordinates and the arms in a
// static const array compiled to something that never stored anything at all,
// and cost an evening to pin down.
static void cross_plot(volatile uint8_t __far *tiles, int16_t x, int16_t y)
{
  if (x < 0 || x >= OVERVIEW_PX || y < 0 || y >= OVERVIEW_PX)
    return;

  // Tiles are in reading order and each is eight rows of eight bytes.
  tiles[(int16_t)(((y >> 3) * OVERVIEW_CHARS + (x >> 3)) * 64
                  + (y & 7) * 8 + (x & 7))] = CROSS_INK;
}

void vic4_crosshair(uint8_t px, uint8_t py)
{
  volatile uint8_t __far *tiles = (volatile uint8_t __far *)OVERVIEW;
  int16_t x = px, y = py;

  dma_copy(OVERVIEW_CLEAN, OVERVIEW, OVERVIEW_BYTES);

  // Four arms with a gap at the centre, so the map pixel being pointed at
  // stays visible.
  cross_plot(tiles, x - 2, y);
  cross_plot(tiles, x - 1, y);
  cross_plot(tiles, x + 1, y);
  cross_plot(tiles, x + 2, y);
  cross_plot(tiles, x, y - 2);
  cross_plot(tiles, x, y - 1);
  cross_plot(tiles, x, y + 1);
  cross_plot(tiles, x, y + 2);
}

void vic4_init(void)
{
  uint16_t i;
  uint8_t __far *cram = (uint8_t __far *)COLOUR_RAM;

  CPU_PORTDDR = 0x41;  // 40 MHz

  VICIV.key = 0x47;  // unlock the VIC-IV registers
  VICIV.key = 0x53;

  VICIV.ctrlb &= (uint8_t)~0x80;  // H320, not H640

  // Turn off the hot registers, or a stray write to a legacy VIC-II register
  // makes the VIC-IV recompute the layout and undo everything below.
  VICIV.sdbdrwd_msb &= (uint8_t)~VIC4_HOTREG_MASK;

  VICIV.ctrlc |= VIC4_CHR16_MASK    // 16-bit character numbers
                 | VIC4_FCLRHI_MASK // full colour for characters above $FF
                 | VIC4_VFAST_MASK;

  // Character data for the panel's text rows. Nothing had set this before the
  // panel existed, and whatever the ROM left behind draws a horizontal line
  // for a space. The C65 ROM's 8x8 set sits at $2D000 and the VIC-IV can read
  // it where it is, so the panel costs no RAM for a font. Written a byte at a
  // time: the 32-bit field runs over $D06B, which is not part of the pointer.
  VICIV.charptr_lsb = 0x00;
  VICIV.charptr_msb = 0xD0;
  VICIV.charptr_bnk = 0x02;

  VICIV.linestep = FB_COLS * 2;  // bytes of screen RAM per row
  VICIV.chrcount = FB_COLS;
  // Source pixels per output pixel, in 120ths: 1:1. The 160-column renderer
  // doubles its pixels into the framebuffer itself rather than having the
  // VIC-IV stretch them, because the stretch would take the panel down to 20
  // characters with it and the geometry cannot be changed per raster row.
  VICIV.chrxscl = 120;

  VICIV.bordercol = 0;
  VICIV.screencol = 0;

  // Character attributes are not used, and whatever the ROM left behind would
  // be interpreted as one.
  for (i = 0; i < SCREEN_ROWS * FB_COLS * 2; i++)
    cram[i] = 0;

  build_screen_tables();
  vic4_show(0);
}

void vic4_set_palette(const uint8_t *planes)
{
  uint16_t i;

  for (i = 0; i < 256; i++) {
    PALETTE.red[i] = planes[i];
    PALETTE.green[i] = planes[256 + i];
    PALETTE.blue[i] = planes[512 + i];
  }
}

void vic4_show(uint8_t buffer)
{
  // Writing all four bytes also clears the CHRCOUNT high bits and the screen
  // pointer megabyte in $D063, which is what we want.
  VICIV.scrnptr = (uint32_t)(uint16_t)&screen[buffer & 1][0];
}

uint32_t vic4_base(uint8_t buffer)
{
  return (buffer & 1) ? FB_B : FB_A;
}
