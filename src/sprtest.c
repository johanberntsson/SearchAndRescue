// Do the VIC-IV's hardware sprites work in THIS display layout?
//
// A standalone program, not part of the game: `make sprtest` builds
// build/sprtest.prg and `make runsprtest` boots it. It sets the display up
// with the game's own vic4_init -- which is the whole point, since the
// question is about this layout and not about sprites in general -- and then
// puts four sprites over the rows the panel occupies.
//
// **Why it is worth asking.** The panel is a picture now and its text is
// still on the 8-pixel character grid, which is the one thing the picture
// cannot live with. A sprite plane over the panel would fix it outright: five
// 64-pixel-wide sprites cover all 320 pixels, a glyph shifted into a 1bpp
// sprite is about 16 writes against 64 for painting the same glyph into
// full-colour pixels, and nothing has to be restored afterwards because the
// plane is transparent where it is not drawn.
//
// **And why it is not already known.** In this layout the legacy sprite
// pointers come from screen+$3F8, which lands at row 12 column 28 -- in the
// middle of the 3D view, where the character number is a framebuffer tile and
// cannot be given a useful value. SPRPTRADR is the way out and, when it was
// last tried here, xemu ignored the 8-bit form and segfaulted outright on
// SPR_PTR16. The sprite itself displayed, so it was only the pointer sourcing
// that was broken. That note is months old and this program is how to retest
// it -- in the emulator, and then on the machine, where the answer may well
// differ.
//
// What each sprite is asking, left to right:
//
//   0  24x21, plain          do 16-bit pointers work at all?
//   1  64x21, SPRX64EN       does the extended width work?
//   2  24x48, SPRHGTEN       does the extended height work?
//   3  64x48, both           the shape the panel actually wants -- and it
//                            carries text, drawn from the ROM character set
//                            at a 3-pixel vertical offset, which is the thing
//                            the character grid cannot do.
//
// A run that draws nothing has failed; the legend on screen says what each
// one should look like.
#include <mega65.h>

#include "dma.h"
#include "loader.h"
#include "panel.h"
#include "vic4.h"

// bank.s owns __low_level_init and wants somewhere to put its measurement.
// The game's copy is in main.c, which this program does not link.
uint16_t cstack_unused;

// Bank 4, above the panel artwork's 15360 bytes, which end at $43C00. Both
// the pointer list and the data have to be somewhere the VIC-IV can read;
// with SPR_PTR16 the data may sit on any 64-byte boundary in chip RAM, which
// is exactly what the game has no room for anywhere else.
#define SPR_PTRS  0x44000UL
#define SPR_DATA  0x44100UL  // $44100 / 64 = $1104, so the pointer is exact
#define SPR_SLOT  512        // 8 bytes a row by 48 rows is 384; round up

#define SPR_MAX_ROWS 48
#define SPR_TALL     48  // what SPRHGHT is set to

// One opaque full-colour character to stand in for the panel's artwork, so
// that the sprites are asked to draw over the kind of thing they will have to
// draw over. Above the sprite data, on its own 64-byte boundary.
#define FCM_TILE (SPR_DATA + 4 * SPR_SLOT)

// The registers the MEGA65 adds. mega65.h names the legacy ones; these are
// spelled out so the test says what it is doing.
#define SPRHGTEN    (*(volatile uint8_t *)0xD055)  // extended height, per sprite
#define SPRHGHT     (*(volatile uint8_t *)0xD056)  // how tall, in pixels
#define SPRX64EN    (*(volatile uint8_t *)0xD057)  // 64 pixels wide, per sprite
#define SPRPTRADRL  (*(volatile uint8_t *)0xD06C)
#define SPRPTRADRM  (*(volatile uint8_t *)0xD06D)
#define SPRPTRBNK   (*(volatile uint8_t *)0xD06E)  // bits 0-6 address, bit 7 SPR_PTR16
#define SPR_PTR16   0x80

#define SPRITE_ENABLE (*(volatile uint8_t *)0xD015)
#define SPRITE_X(n)   (*(volatile uint8_t *)(0xD000 + (n) * 2))
#define SPRITE_Y(n)   (*(volatile uint8_t *)(0xD001 + (n) * 2))
#define SPRITE_XMSB   (*(volatile uint8_t *)0xD010)
#define SPRITE_COL(n) (*(volatile uint8_t *)(0xD027 + (n)))

// Sprite coordinates are the VIC-II's: the 40-column display starts at X 24,
// and its first character row at Y 50. So a panel cell is one multiply away,
// and the panel is the six rows below the framebuffer's nineteen.
#define SPR_X(col) (uint8_t)(24 + (col) * 8)
#define SPR_Y(row) (uint8_t)(50 + (FB_ROWS + (row)) * 8)

// The C65 ROM's 8x8 character set, where the panel's text already comes from.
// Read here with the CPU rather than by the VIC-IV, which is a question of
// its own: the ROM lives in RAM at $20000-$3FFFF, so a far read should simply
// work, and if the glyphs come out as rubbish that is the answer.
#define ROM_FONT 0x2D000UL

static uint32_t spr_data(uint8_t n)
{
  return SPR_DATA + (uint32_t)n * SPR_SLOT;
}

// One glyph ORed into a mono sprite at any pixel position at all -- which is
// the whole argument for doing it this way. Eight rows of one shifted byte
// pair, about sixteen writes; painting the same glyph into the panel's
// full-colour characters is sixty-four, and needs the background restoring
// afterwards.
static void spr_glyph(uint32_t bitmap, uint8_t stride, uint8_t x, uint8_t y,
                      uint8_t code)
{
  volatile uint8_t __far *bmp = (volatile uint8_t __far *)bitmap;
  const uint8_t __far *glyph = (const uint8_t __far *)(ROM_FONT
                                                       + (uint32_t)code * 8);
  uint8_t col = (uint8_t)(x >> 3);
  uint8_t shift = (uint8_t)(x & 7);
  uint8_t r;

  for (r = 0; r < 8; r++) {
    // Widened first and shifted in 16 bits throughout. A byte that reaches
    // the top of an int here would come back sign extended -- the trap
    // CLAUDE.md records under Performance, which cost a wind bearing of 896
    // degrees -- and every glyph with a pixel in column 0 would smear.
    uint16_t bits = (uint16_t)glyph[r];
    int16_t at = (int16_t)((uint16_t)(y + r) * stride + col);

    bits <<= 8;
    bits >>= shift;
    bmp[at] |= (uint8_t)(bits >> 8);
    if (shift)
      bmp[at + 1] |= (uint8_t)bits;
  }
}

static void spr_text(uint32_t bitmap, uint8_t stride, uint8_t x, uint8_t y,
                     const char *s)
{
  while (*s) {
    spr_glyph(bitmap, stride, x, y, vic4_screen_code(*s));
    x = (uint8_t)(x + 8);
    s++;
  }
}

// A one-pixel frame around the sprite's whole box, so that what is on screen
// says where the sprite is and how big the VIC-IV thinks it is, not merely
// that something appeared.
static void spr_frame(uint32_t bitmap, uint8_t stride, uint8_t rows)
{
  volatile uint8_t __far *bmp = (volatile uint8_t __far *)bitmap;
  uint8_t r, c;

  for (c = 0; c < stride; c++) {
    bmp[(int16_t)c] = 0xFF;
    bmp[(int16_t)((rows - 1) * stride + c)] = 0xFF;
  }
  for (r = 0; r < rows; r++) {
    bmp[(int16_t)(r * stride)] |= 0x80;
    bmp[(int16_t)(r * stride + stride - 1)] |= 0x01;
  }
}

static void legend(uint8_t row, const char *s)
{
  vic4_puts(1, row, s, PANEL_INK);
}

int main(void)
{
  uint8_t n;

  // The game's own display, because the layout is the question: 16-bit
  // character numbers, full colour above $FF, screen RAM up in bank 5 -- and
  // therefore sprite pointers at $5C3F8, in the middle of the 3D view.
  vic4_init();

  // Nothing has loaded a palette, so name the handful of entries this needs.
  vic4_set_entry(0, 0, 0, 0);
  vic4_set_entry(PANEL_INK, 255, 255, 255);
  vic4_set_entry(PANEL_LABEL, 160, 160, 160);
  vic4_set_entry(7, 80, 255, 80);
  vic4_set_entry(8, 60, 70, 60);   // the stand-in for the panel artwork

  legend(1, "HARDWARE SPRITE TEST");
  legend(3, "EXPECT FOUR GREEN BOXES BELOW,");
  legend(4, "OVER THE ROWS THE PANEL USES:");
  legend(6, "0  24X21  PLAIN");
  legend(7, "1  64X21  SPRX64EN");
  legend(8, "2  24X48  SPRHGTEN");
  legend(9, "3  64X48  BOTH, WITH TEXT IN IT");
  legend(11, "THE TEXT IN SPRITE 3 SITS 3 PIXELS");
  legend(12, "DOWN, WHICH IS THE WHOLE POINT.");
  legend(14, "THE PANEL ROWS ARE FULL-COLOUR TILES:");
  legend(15, "THE SPRITES MUST BE OVER THEM.");
  legend(17, "NOTHING AT ALL MEANS SPRPTRADR AND");
  legend(18, "SPR_PTR16 DO NOT WORK HERE.");
  // row 19 is the last text row before the panel

  // The panel is made of FULL-COLOUR characters, and a sprite that goes
  // behind those is no use whatever else works. So the six panel rows are
  // filled with an opaque full-colour tile of their own -- built here rather
  // than loaded, since this program never opens the disk -- and the sprites
  // are laid over it. Priority is left at the default, sprites in front.
  {
    uint8_t row, col;

    dma_fill(FCM_TILE, 8, 64);
    for (row = 0; row < PANEL_ROWS; row++)
      for (col = 0; col < PANEL_COLS; col++)
        vic4_panel_tile(col, row, (uint16_t)(FCM_TILE / 64));
  }

  // Bitmaps. Cleared with the DMA rather than a loop, since that is four
  // jobs against two thousand far writes.
  for (n = 0; n < 4; n++)
    dma_fill(spr_data(n), 0, SPR_SLOT);

  spr_frame(spr_data(0), 3, 21);
  spr_frame(spr_data(1), 8, 21);
  spr_frame(spr_data(2), 3, SPR_TALL);
  spr_frame(spr_data(3), 8, SPR_TALL);

  // Sprite 1 gets stripes across its full 64 pixels: a sprite that came out
  // 24 wide would show only the first of them.
  {
    volatile uint8_t __far *bmp = (volatile uint8_t __far *)spr_data(1);
    uint8_t c;

    for (c = 0; c < 8; c++)
      bmp[(int16_t)(10 * 8 + c)] = (uint8_t)(c & 1 ? 0x55 : 0xAA);
  }

  // And sprite 3 gets what this is all for: text at a vertical offset the
  // character grid cannot produce. Seven characters is 56 pixels of the 64.
  spr_text(spr_data(3), 8, 4, 3, "ALT 126");
  spr_text(spr_data(3), 8, 4, 19, "HDG 090");
  spr_text(spr_data(3), 8, 4, 35, "SPD NRM");

  // The pointer list: eight 16-bit values, each the data address divided by
  // 64. This is what SPR_PTR16 buys -- without it the pointers would still be
  // read from screen+$3F8, where the character number is a framebuffer tile.
  {
    volatile uint8_t __far *ptrs = (volatile uint8_t __far *)SPR_PTRS;

    for (n = 0; n < 8; n++) {
      uint16_t p = (uint16_t)(spr_data(n < 4 ? n : 0) / 64);

      ptrs[(int16_t)(n * 2)] = (uint8_t)p;
      ptrs[(int16_t)(n * 2 + 1)] = (uint8_t)(p >> 8);
    }
  }

  SPRPTRADRL = (uint8_t)(SPR_PTRS & 0xFF);
  SPRPTRADRM = (uint8_t)((SPR_PTRS >> 8) & 0xFF);
  SPRPTRBNK = (uint8_t)(((SPR_PTRS >> 16) & 0x7F) | SPR_PTR16);

  SPRX64EN = 0x0A;   // sprites 1 and 3 are 64 pixels wide
  SPRHGTEN = 0x0C;   // sprites 2 and 3 are taller than 21
  SPRHGHT = SPR_TALL;

  for (n = 0; n < 4; n++)
    SPRITE_COL(n) = 7;

  SPRITE_X(0) = SPR_X(1);
  SPRITE_X(1) = SPR_X(5);
  SPRITE_X(2) = SPR_X(17);
  SPRITE_X(3) = SPR_X(21);
  for (n = 0; n < 4; n++)
    SPRITE_Y(n) = SPR_Y(0);
  SPRITE_XMSB = 0;

  SPRITE_ENABLE = 0x0F;

  for (;;)
    ;
}
