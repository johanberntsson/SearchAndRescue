#include <mega65.h>

#include "dma.h"
#include "loader.h"
#include "dma.h"
#include "loader.h"
#include "panel.h"
#include "vic4.h"

#define COLOUR_RAM 0xFF80000UL

// Screen RAM: one 16-bit character number per cell, read row by row. Up in
// bank 5 rather than in a C array -- see vic4.h -- so every access here is a
// far one. All of them are cold: nothing in a frame touches screen RAM except
// the panel's handful of characters.
static uint16_t __far *screen(uint8_t buffer)
{
  return (uint16_t __far *)(buffer ? SCREEN_B : SCREEN_A);
}

// Both tables at once, which is what almost every caller wants: the panel and
// the game screens are the same in either buffer, so only the framebuffer
// tiles differ between them.
static void put_cell(uint16_t cell, uint16_t a, uint16_t b, uint8_t colour)
{
  uint8_t __far *cram = (uint8_t __far *)COLOUR_RAM;

  screen(0)[(int16_t)cell] = a;
  screen(1)[(int16_t)cell] = b;

  // Two colour RAM bytes per cell in 16-bit character mode: attributes then
  // the foreground colour, which is a full 8-bit palette index. A full-colour
  // character carries a palette index per pixel and takes no ink from here.
  cram[(int16_t)(cell * 2)] = 0;
  cram[(int16_t)(cell * 2 + 1)] = colour;
}

// The framebuffer's own character numbers for the 3D view, and spaces for the
// panel below it.
static void build_screen_tables(void)
{
  uint8_t row, col;

  for (row = 0; row < SCREEN_ROWS; row++) {
    for (col = 0; col < FB_COLS; col++) {
      uint16_t cell = (uint16_t)row * FB_COLS + col;

      if (row < FB_ROWS) {
        uint16_t tile = col * FB_ROWS + row;  // column-major, see vic4.h

        put_cell(cell, (uint16_t)(FB_A / 64) + tile,
                 (uint16_t)(FB_B / 64) + tile, 0);
      } else {
        put_cell(cell, ' ', ' ', PANEL_INK);  // a text number, below $100
      }
    }
  }
}

void vic4_text_char(uint8_t col, uint8_t row, uint8_t ch, uint8_t colour)
{
  put_cell((uint16_t)row * FB_COLS + col, ch, ch, colour);
}

void vic4_panel_char(uint8_t col, uint8_t row, uint8_t ch, uint8_t colour)
{
  vic4_text_char(col, (uint8_t)(FB_ROWS + row), ch, colour);
}

void vic4_panel_tile(uint8_t col, uint8_t row, uint16_t charnum)
{
  put_cell((uint16_t)(FB_ROWS + row) * FB_COLS + col, charnum, charnum, 0);
}

// Screen codes are not ASCII: in the uppercase character set the letters run
// from 1 rather than from $41, while digits and most punctuation sit where
// ASCII has them.
uint8_t vic4_screen_code(char c)
{
  uint8_t u = (uint8_t)c;

  if (u >= 0x40 && u < 0x60)
    return u - 0x40;  // A-Z
  if (u >= 0x60 && u < 0x80)
    return u - 0x60;  // a-z, same glyphs
  return u;
}

void vic4_puts(uint8_t col, uint8_t row, const char *s, uint8_t colour)
{
  while (*s && col < FB_COLS) {
    vic4_text_char(col, row, vic4_screen_code(*s), colour);
    col++;
    s++;
  }
}

// Give the whole display over to text: the mode is chosen per character
// NUMBER, so writing numbers below $100 into the rows the 3D view normally
// occupies turns them into ordinary characters, and vic4_view_mode turns them
// back. No pixels move either way.
void vic4_text_mode(void)
{
  uint16_t cell;

  for (cell = 0; cell < SCREEN_ROWS * FB_COLS; cell++)
    put_cell(cell, ' ', ' ', PANEL_INK);
}

void vic4_view_mode(void)
{
  build_screen_tables();
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

  // Whatever the ROM left enabled: the sprite pointers come from screen+$3F8,
  // which in this layout is somebody else's data, so any enabled sprite draws
  // confetti. Safe to write now that the hot registers are off.
  *(volatile uint8_t *)0xD015 = 0;

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

  // Text, not the framebuffer tiles: bank 1 has never been written at this
  // point, so showing the 3D view here would put a screenful of whatever was
  // in RAM between the boot screen and the title. Every caller that wants the
  // view asks for it with vic4_view_mode.
  vic4_text_mode();
  vic4_show(0);
}



void vic4_set_entry(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
  // The registers hold each channel with its nybbles reversed, which is why
  // tools/convmap.py ships the palette that way.
  PALETTE.red[index] = (uint8_t)((r & 0x0F) << 4 | r >> 4);
  PALETTE.green[index] = (uint8_t)((g & 0x0F) << 4 | g >> 4);
  PALETTE.blue[index] = (uint8_t)((b & 0x0F) << 4 | b >> 4);
}

void vic4_set_range(const uint8_t __far *planes, uint16_t first, uint16_t count)
{
  uint16_t i;

  for (i = 0; i < count; i++) {
    uint16_t n = first + i;

    PALETTE.red[n] = planes[n];
    PALETTE.green[n] = planes[256 + n];
    PALETTE.blue[n] = planes[512 + n];
  }
}

void vic4_set_palette(const uint8_t __far *planes)
{
  vic4_set_range(planes, 0, 256);
}

void vic4_show(uint8_t buffer)
{
  // Writing all four bytes also clears the CHRCOUNT high bits and the screen
  // pointer megabyte in $D063, which is what we want.
  VICIV.scrnptr = (buffer & 1) ? SCREEN_B : SCREEN_A;
}

uint32_t vic4_base(uint8_t buffer)
{
  return (buffer & 1) ? FB_B : FB_A;
}
