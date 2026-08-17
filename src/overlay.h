// The panel's text, on a plane of hardware sprites over its artwork.
//
// **Why it is not text characters any more.** The artwork is a picture and
// pictures are not drawn on an 8-pixel grid; a text character is. Every way
// of moving a character off that grid was tried on paper first and none of
// them works here: a pre-shifted font puts each line across two cells, which
// five lines in six rows cannot afford; the Raster Rewrite Buffer can do it,
// but wants 64-byte glyphs and depends on bits xemu has had a bug report
// against. Hardware sprites need none of that, and `make sprtest` proved
// every register this uses -- 16-bit pointers, 64-pixel widths, 48-pixel
// heights, priority over full-colour characters -- in the emulator and then
// on a real MEGA65.
//
// Five sprites of 64x48 side by side are 320x48, which is the panel exactly.
// A glyph is about sixteen writes -- eight rows of one shifted byte pair --
// against sixty-four for painting the same glyph into full-colour pixels, and
// **nothing has to be restored**: the plane is transparent where it is not
// drawn, so clearing a field is writing zeros and the artwork underneath is
// never touched.
#ifndef OVERLAY_H
#define OVERLAY_H

#include <stdint.h>

#define OVERLAY_PLANE_SPRITES 5                    // 5 x 64 is the panel wide
#define OVERLAY_SPRITES 6                          // and one more, see below
#define OVERLAY_STRIDE  8                            // bytes per sprite row
#define OVERLAY_ROWS    48                              // the panel's six rows
#define OVERLAY_W       (OVERLAY_SPRITES * 64)          // the drawing surface
#define OVERLAY_SLOT    (OVERLAY_STRIDE * OVERLAY_ROWS) // 384 bytes a sprite
#define OVERLAY_COLS    (OVERLAY_W / 8)                 // byte columns across

// **The battery has a sprite to itself, because a sprite carries one colour.**
// It is the one readout that changes colour -- green, then yellow, then red --
// and everything else on the plane has to stay green, so it cannot share one.
//
// It is drawn like any other field, at x OVERLAY_ALERT in the same coordinates
// as everything else, and simply appears somewhere else: the five plane
// sprites are laid end to end from x 0, and this one is placed over the
// artwork's battery box instead. It is behind sprites 0-4 in the hardware's
// priority, which costs nothing, since no two of them draw in the same place.
#define OVERLAY_ALERT    (OVERLAY_PLANE_SPRITES * 64)  // where it is drawn
#define OVERLAY_ALERT_AT 199                           // where it appears

// One sprite's colour, as a palette entry. Sprite OVERLAY_PLANE_SPRITES is
// the battery's; the rest are green and set once by overlay_on.
void overlay_ink(uint8_t sprite, uint8_t entry);

// Point the sprites at the plane, size them, colour them and turn them on.
// Called when a flight starts; the plane is wiped by it.
void overlay_on(void);

// Take them away again, which every page does through vic4_text_mode: a
// sprite is above everything, so one left enabled would float over the
// mission list.
void overlay_off(void);

// Blank a rectangle, to the pixel: the columns at either end are masked
// rather than rounded out, so a field may be redrawn without eating whatever
// shares its first and last byte columns. It always does share them -- text
// at an arbitrary x is the whole point of this.
void overlay_clear(uint16_t x, uint8_t y, uint16_t w, uint8_t h);

// A string at any pixel position at all, which is the whole point of this.
// The span it covers is cleared first, so a caller redrawing a field needs
// nothing else -- but a field that can grow shorter has to clear the widest
// it has been (see panel_cargo).
void overlay_text(uint16_t x, uint8_t y, const char *s);

#endif
