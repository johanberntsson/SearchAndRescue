#include "screens.h"

#include <mega65.h>

#include "mission.h"
#include "panel.h"
#include "vic4.h"

#define TITLE_ROW   8
#define PROMPT_ROW  22
#define SOUND_ROW   (PROMPT_ROW + 2)  // a blank row under the prompt

// The two lines the title screen and the boot screen both carry, written once
// so the boot screen cannot say something the title screen does not.
#define TITLE_TEXT  "SEARCH AND RESCUE"
#define TITLE_SUB   "A MEGA65 DRONE SIMULATOR"

static uint8_t width_of(const char *s)
{
  uint8_t n = 0;

  while (s[n])
    n++;
  return n;
}

static void centre(uint8_t row, const char *s, uint8_t colour)
{
  uint8_t w = width_of(s);

  vic4_puts(w >= PANEL_COLS ? 0 : (uint8_t)((PANEL_COLS - w) / 2), row, s,
            colour);
}

// The mute key and what it has done, under every page's prompt.
//
// The rule is that M mutes whatever the place you are in sounds like: the
// tune on a page, the motors in the air. So this line says MUSIC -- the
// flight has no room for a line and says it on the panel's message row
// instead, at the moment the key is pressed.
//
// Kept here rather than passed to each page, because every page ends by
// drawing it and only main knows when it changes.
static uint8_t music_state = 1;

static void music_line(void)
{
  centre(SOUND_ROW, music_state ? "M   MUSIC ON " : "M   MUSIC OFF",
         PANEL_LABEL);
}

void screens_music(uint8_t on)
{
  music_state = on;
  music_line();
}

// Right-aligned digits, zero padded, which is what both a percentage and a
// clock want.
static void put_digits(uint8_t col, uint8_t row, uint16_t value, uint8_t width,
                       uint8_t colour)
{
  uint8_t i;

  for (i = width; i-- > 0;) {
    vic4_text_char((uint8_t)(col + i), row, (uint8_t)('0' + value % 10),
                   colour);
    value /= 10;
  }
}

// A fix as a pilot reads it: degrees, three decimals, hemisphere. Formatted
// from the mission's own numbers rather than written out again as text, so
// that moving a target moves what the briefing says about it.
static uint8_t put_fix(uint8_t col, uint8_t row, uint16_t mdeg, uint8_t width,
                       char hemisphere, uint8_t colour)
{
  put_digits(col, row, mdeg / 1000, width, colour);
  vic4_text_char((uint8_t)(col + width), row, '.', colour);
  put_digits((uint8_t)(col + width + 1), row, mdeg % 1000, 3, colour);
  vic4_text_char((uint8_t)(col + width + 4), row, vic4_screen_code(hemisphere),
                 colour);
  return (uint8_t)(col + width + 5);
}

static void put_position(uint8_t col, uint8_t row, const mission *m,
                         uint8_t colour)
{
  col = put_fix(col, row, m->lat, 2, 'N', colour);
  put_fix((uint8_t)(col + 1), row, m->lon, 3, 'E', colour);
}

// ---------------------------------------------------------------------------
// The boot screen.
//
// Loading has to happen before vic4_init -- a Kernal open fails outright
// afterwards -- so this is the ROM's own text display and not the game's. It
// is dressed to look like screens_title anyway: same forty columns, same rows,
// same words, white on black. The pilot sees one picture that gains a progress
// bar and then loses it again, rather than a BASIC screen followed by a title.
//
// **Nothing here prints.** printf goes through the ROM's screen editor, which
// lays a row out eighty bytes wide whatever the display is doing, offers no
// cursor addressing to put a word at row 8 with, and can only ever add to the
// bottom of the screen -- so the LOADING line could never be taken away again.
// Writing screen RAM directly answers all three, and it is not a hard thing to
// do: the C65's screen is 1000 bytes at $0800 with the colour for each cell at
// the same index into colour RAM.
//
// It also settles the old business of the invisible bar. The solid block is
// screen code 160 and printing it produced *nothing* for months -- Calypsi's
// output path drops that byte -- while storing 160 into screen RAM is just a
// byte, and draws the block it always should have.
#define BOOT_SCREEN ((uint8_t *)0x0800)  // the ROM's; ours starts at $2001
#define BOOT_CRAM   0xFF80000UL
#define BOOT_COLS   40
#define BOOT_ROWS   25
#define BOOT_BLOCK  160  // reverse space: the solid block, as a screen code

// The C65's default palette is still up here -- the game's own arrives with
// the first map -- so these are C64 colour numbers rather than the panel's.
// 15 is the nearest light grey to the (150,160,170) tools/convmap.py gives
// PANEL_LABEL, so the subtitle stays the quieter of the two lines.
#define BOOT_WHITE  1
#define BOOT_GREY   15

// The bar sits under the word, on the row the title screen puts PRESS SPACE
// on, so that finishing the load simply swaps one for the other in place.
#define BAR_ROW    (PROMPT_ROW + 1)
#define BAR_WIDTH  30
#define BAR_COL    ((BOOT_COLS - BAR_WIDTH) / 2)

static void boot_cell(uint8_t col, uint8_t row, uint8_t code, uint8_t colour)
{
  uint16_t cell = (uint16_t)row * BOOT_COLS + col;

  BOOT_SCREEN[cell] = code;
  ((uint8_t __far *)BOOT_CRAM)[(int16_t)cell] = colour;
}

static void boot_puts(uint8_t col, uint8_t row, const char *s, uint8_t colour)
{
  while (*s && col < BOOT_COLS) {
    boot_cell(col, row, vic4_screen_code(*s), colour);
    col++;
    s++;
  }
}

static void boot_centre(uint8_t row, const char *s, uint8_t colour)
{
  uint8_t w = width_of(s);

  boot_puts(w >= BOOT_COLS ? 0 : (uint8_t)((BOOT_COLS - w) / 2), row, s,
            colour);
}

static void boot_clear_row(uint8_t row)
{
  uint8_t col;

  for (col = 0; col < BOOT_COLS; col++)
    boot_cell(col, row, ' ', BOOT_WHITE);
}

// How many blocks of the loading bar are up. The bar only ever grows, so a
// count is all the state it needs.
static uint8_t bar_drawn;

void screens_boot(void)
{
  uint8_t row;

  // Forty columns, so the boot screen is the shape the title screen is. This
  // is a VIC-III register the ROM has already unlocked and the only display
  // register touched before loading: the whole of vic4_init has to wait until
  // the last file is read, and something in it leaves the Kernal unable to
  // open one at all.
  //
  // The editor goes on believing the display is eighty wide, which costs
  // nothing while nothing prints -- see screens_boot_restore for the one
  // thing that still does.
  VICIV.ctrlb &= (uint8_t)~0x80;  // H320
  VICIV.bordercol = 0;
  VICIV.screencol = 0;

  for (row = 0; row < BOOT_ROWS; row++)
    boot_clear_row(row);

  boot_centre(TITLE_ROW, TITLE_TEXT, BOOT_WHITE);
  boot_centre(TITLE_ROW + 2, TITLE_SUB, BOOT_GREY);
  boot_centre(PROMPT_ROW, "LOADING", BOOT_WHITE);
  bar_drawn = 0;
}

void screens_loading(uint8_t percent)
{
  uint8_t want;

  if (percent > 100)
    percent = 100;
  want = (uint8_t)((uint16_t)percent * BAR_WIDTH / 100);

  while (bar_drawn < want) {
    boot_cell((uint8_t)(BAR_COL + bar_drawn), BAR_ROW, BOOT_BLOCK, BOOT_WHITE);
    bar_drawn++;
  }
}

void screens_loaded(void)
{
  boot_clear_row(PROMPT_ROW);
  boot_clear_row(BAR_ROW);
}

void screens_load_failed(const char *why, const char *file)
{
  boot_clear_row(PROMPT_ROW);
  boot_clear_row(BAR_ROW);
  boot_centre(PROMPT_ROW, why ? why : "CANNOT READ", BOOT_WHITE);
  boot_centre(BAR_ROW, file ? file : "", BOOT_WHITE);
}

void screens_boot_restore(void)
{
  VICIV.ctrlb |= 0x80;  // H640, which is what the ROM's editor writes for
}

void screens_title(void)
{
  vic4_text_mode();
  centre(TITLE_ROW, TITLE_TEXT, PANEL_INK);
  centre(TITLE_ROW + 2, TITLE_SUB, PANEL_LABEL);
  centre(PROMPT_ROW, "PRESS SPACE", PANEL_INK);
  music_line();
}

void screens_missions(uint8_t selected)
{
  uint8_t i;

  vic4_text_mode();
  centre(2, "SELECT MISSION", PANEL_INK);

  for (i = 0; i < mission_count(); i++) {
    uint8_t row = (uint8_t)(6 + i * 2);
    uint8_t ink = i == selected ? PANEL_INK : PANEL_LABEL;

    vic4_text_char(6, row, i == selected ? '>' : ' ', PANEL_INK);
    put_digits(8, row, (uint16_t)(i + 1), 1, ink);
    vic4_puts(11, row, missions[i].name, ink);
  }

  centre(PROMPT_ROW, "W S   CHOOSE      SPACE   BRIEF", PANEL_LABEL);
  music_line();
}

void screens_briefing(uint8_t mission_no)
{
  const mission *m = &missions[mission_no];
  uint8_t i;

  vic4_text_mode();
  vic4_puts(2, 0, "MISSION", PANEL_LABEL);
  put_digits(10, 0, (uint16_t)(mission_no + 1), 1, PANEL_LABEL);
  vic4_puts(13, 0, m->name, PANEL_INK);

  // **The page starts on row 0**, which no other screen does and which is what
  // paid for the thermal camera's line. The page was already full -- four
  // labelled blocks, each with a blank row before it, and the controls list
  // ending on the row above the prompt -- so the seventeenth key had nowhere
  // to go but the top margin. The display's own border is the margin now.
  //
  // **A control the game does not name is a control nobody has**, and mission
  // three cannot be finished without this one, so this was not optional.
  //
  // The separators are rows 1, 5, 9 and 12, and there are no others.
  for (i = 0; i < BRIEF_LINES; i++)
    vic4_puts(2, (uint8_t)(2 + i), m->brief[i], PANEL_INK);

  vic4_puts(2, 6, "LAST KNOWN POSITION", PANEL_LABEL);
  put_position(22, 6, m, PANEL_INK);
  vic4_puts(2, 7, "CARGO", PANEL_LABEL);
  vic4_puts(22, 7, mission_cargo_name(m), PANEL_INK);
  vic4_puts(2, 8, "WEATHER", PANEL_LABEL);
  vic4_puts(22, 8, m->weather == WEATHER_RAIN ? "RAIN" : "CLEAR", PANEL_INK);

  vic4_puts(2, 10, "OBJECTIVE", PANEL_LABEL);
  vic4_puts(4, 11, m->objective, PANEL_INK);

  vic4_puts(2, 13, "CONTROLS", PANEL_LABEL);
  vic4_puts(4, 14, "W S      FORWARD    BACK", PANEL_INK);
  vic4_puts(4, 15, "A D      TURN LEFT  RIGHT", PANEL_INK);
  vic4_puts(4, 16, "R F      CLIMB      DESCEND", PANEL_INK);
  vic4_puts(4, 17, "Q E      CAMERA UP  DOWN", PANEL_INK);
  vic4_puts(4, 18, "1 2 3    SPEED  SLOW NORMAL SPORT", PANEL_INK);
  // Named on every briefing and not only on the mission that needs it: the
  // camera works over any of them, and a sensor nobody knows about is not a
  // sensor. The key column is nine wide, as below.
  vic4_puts(4, 19, "T        THERMAL CAMERA", PANEL_INK);
  // The one line that differs between the two kinds of mission, and it comes
  // out of the mission's cargo bay rather than out of a branch here.
  vic4_puts(4, 20, mission_action_name(m), PANEL_INK);
  vic4_puts(13, 20, mission_action_verb(m), PANEL_INK);
  // Split rather than one string, so the verb lines up with every row above
  // it. The key column is nine wide because RUN/STOP is eight and would
  // otherwise touch its verb.
  vic4_puts(4, 21, "RUN/STOP", PANEL_INK);
  vic4_puts(13, 21, "ABANDON MISSION", PANEL_INK);

  centre(PROMPT_ROW, "SPACE   LAUNCH", PANEL_LABEL);
  music_line();
}

void screens_debrief(uint8_t mission_no, flight_outcome how, uint16_t seconds)
{
  const mission *m = &missions[mission_no];
  const char *heading = "MISSION ACCOMPLISHED";
  const char *what = m->done;

  if (how == FLIGHT_LOST) {
    heading = "MISSION FAILED";
    // Only a mission with something in the bay can lose it, so `lost` is
    // always there; the fallback is for a table entry that forgot it.
    what = m->lost ? m->lost : "THE CARGO WENT DOWN IN THE WRONG PLACE";
  } else if (how == FLIGHT_CRASHED) {
    heading = "DRONE DESTROYED";
    what = "SPORT MODE HAS NO TERRAIN FOLLOWING";
  } else if (how == FLIGHT_FLAT) {
    heading = "BATTERY EMPTY";
    what = "THE DRONE CAME DOWN SHORT OF THE FIX";
  } else if (how == FLIGHT_ABORTED) {
    heading = "MISSION ABANDONED";
    what = "THE DRONE CAME HOME EMPTY HANDED";
  }

  vic4_text_mode();
  centre(6, heading, PANEL_INK);
  centre(9, what, PANEL_LABEL);
  put_position(12, 11, m, PANEL_INK);

  vic4_puts(12, 13, "FLIGHT TIME", PANEL_LABEL);
  put_digits(24, 13, seconds / 60, 2, PANEL_INK);
  vic4_text_char(26, 13, ':', PANEL_INK);
  put_digits(27, 13, seconds % 60, 2, PANEL_INK);

  centre(PROMPT_ROW, "SPACE   RETURN TO MISSIONS", PANEL_LABEL);
  music_line();
}
