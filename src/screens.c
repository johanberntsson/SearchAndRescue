#include "screens.h"

#include <mega65.h>

#include "drone.h"
#include "loader.h"
#include "mission.h"
#include "panel.h"
#include "vic4.h"

#define TITLE_ROW   8
#define PROMPT_ROW  22
#define SOUND_ROW   (PROMPT_ROW + 2)  // a blank row under the prompt

// The two lines the title screen and the boot screen both carry, written once
// so the boot screen cannot say something the title screen does not.
#define TITLE_TEXT   "SEARCH AND RESCUE"
#define TITLE_AUTHOR "BY JOHAN BERNTSSON"
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

// Where the line starts. Centred on its own; the briefing shares the row with
// the key for the controls page and moves it left to make room, and the mute
// key has to redraw it wherever the page put it.
#define MUSIC_TEXT_W 13
#define MUSIC_COL    ((PANEL_COLS - MUSIC_TEXT_W) / 2)
static uint8_t music_col = MUSIC_COL;

static void music_line(void)
{
  vic4_puts(music_col, SOUND_ROW,
            music_state ? "M   MUSIC ON " : "M   MUSIC OFF", PANEL_LABEL);
}

void screens_music(uint8_t on)
{
  music_state = on;
  music_line();
}

// The line as the controls page draws it, which is the one page without the
// key for itself beside it.
static void sound_line(void)
{
  music_col = MUSIC_COL;
  music_line();
}

// And as every other page does: the mute, then the key for the controls page,
// which is reachable from every page and from the air.
#define HELP_TEXT   "HELP/F1   CONTROLS"
#define SOUND_GAP   4
#define HELP_COL    (MUSIC_COL_HELP + MUSIC_TEXT_W + SOUND_GAP)
#define MUSIC_COL_HELP \
  ((PANEL_COLS - MUSIC_TEXT_W - SOUND_GAP - (sizeof HELP_TEXT - 1)) / 2)

static void sound_line_help(void)
{
  music_col = MUSIC_COL_HELP;
  music_line();
  vic4_puts(HELP_COL, SOUND_ROW, HELP_TEXT, PANEL_LABEL);
}

// What the briefing calls the mission's weather. A table would want keeping in
// step with weather.h by hand; three names do not.
static const char *weather_name(uint8_t weather)
{
  if (weather == WEATHER_RAIN)
    return "RAIN";
  if (weather == WEATHER_SNOW)
    return "SNOW";
  return "CLEAR";
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

static void put_position(uint8_t col, uint8_t row, uint16_t lat, uint16_t lon,
                         uint8_t colour)
{
  col = put_fix(col, row, lat, 2, 'N', colour);
  put_fix((uint8_t)(col + 1), row, lon, 3, 'E', colour);
}

// ---------------------------------------------------------------------------
// The title: the logo, and the line under it.
//
// The logo is LOGO_COLS x LOGO_ROWS full-colour characters at LOGO in bank 5,
// written by tools/convlogo.py, and putting it on a page is naming their
// numbers -- the display picks text or full colour per character number, so a
// picture among the letters costs what the letters do. Its colours are
// LOGO_COLOURS entries borrowed from the sky and the panel artwork, which no
// page shows; a flight's map_use() puts the whole palette back, so every page
// that draws the logo has to load its colours again, and this does.
//
// If the logo did not load, the title is the words it always was: a disk
// that has lost one file should still say what it is.
#define LOGO_ROW (TITLE_ROW - LOGO_ROWS + 1)  // its foot on the title row
#define LOGO_COL ((PANEL_COLS - LOGO_COLS) / 2)

static uint8_t logo_ok;

static void title_lines(void)
{
  if (logo_ok) {
    const uint8_t __far *pal = (const uint8_t __far *)LOGO_PALETTE;
    uint8_t row, col, i;

    for (i = 0; i < LOGO_COLOURS; i++) {
      PALETTE.red[LOGO_BASE + i] = pal[i];
      PALETTE.green[LOGO_BASE + i] = pal[LOGO_COLOURS + i];
      PALETTE.blue[LOGO_BASE + i] = pal[2 * LOGO_COLOURS + i];
    }
    for (row = 0; row < LOGO_ROWS; row++)
      for (col = 0; col < LOGO_COLS; col++)
        vic4_tile((uint8_t)(LOGO_COL + col), (uint8_t)(LOGO_ROW + row),
                  (uint16_t)(LOGO_CHAR + row * LOGO_COLS + col));
  } else {
    centre(TITLE_ROW, TITLE_TEXT, PANEL_INK);
    centre(TITLE_ROW + 1, TITLE_AUTHOR, PANEL_INK);
  }
  centre(TITLE_ROW + 2, TITLE_SUB, PANEL_LABEL);
  centre(TITLE_ROW + 3, TITLE_AUTHOR, PANEL_LABEL);
}

// ---------------------------------------------------------------------------
// The boot screen.
//
// It is the title screen with a loading bar under it, and since the logo it
// is drawn on the game's own display rather than the ROM's: a picture needs
// full-colour characters, and those need 16-bit character numbers, which the
// ROM's 8-bit screen at $0800 cannot name. vic4_boot sets up only the part of
// the display a page needs -- forty columns of the two screen tables in bank
// 5 -- and leaves the rest of vic4_init, which the Kernal cannot read a disk
// after, until the last file is in.
//
// **Nothing here prints**, and nothing could: printf goes through the ROM's
// screen editor, which still believes the screen is the one at $0800. Every
// word is a store into the screen tables, exactly as on every other page.
#define BOOT_BLOCK  160  // reverse space: the solid block, as a screen code

// The bar sits under the word, on the row the title screen puts PRESS SPACE
// on, so that finishing the load simply swaps one for the other in place.
#define BAR_ROW    (PROMPT_ROW + 1)
#define BAR_WIDTH  30
#define BAR_COL    ((PANEL_COLS - BAR_WIDTH) / 2)

static void clear_row(uint8_t row)
{
  uint8_t col;

  for (col = 0; col < PANEL_COLS; col++)
    vic4_text_char(col, row, ' ', PANEL_INK);
}

// How many blocks of the loading bar are up. The bar only ever grows, so a
// count is all the state it needs.
static uint8_t bar_drawn;

void screens_boot(void)
{
  vic4_boot();

  // The C65's palette is still up -- the game's arrives with the first map --
  // so the page's two inks are given the colours tools/convmap.py gives them.
  vic4_set_entry(PANEL_INK, 255, 255, 255);
  vic4_set_entry(PANEL_LABEL, 150, 160, 170);

  centre(PROMPT_ROW, "LOADING", PANEL_INK);
  bar_drawn = 0;

  // The logo is the first thing off the disk, so the boot screen can carry
  // it: four kilobytes crunched, a moment's read.
  logo_ok = !load_logo();
  title_lines();

  // And the drone, hovering in the middle of the screen while the rest
  // loads. Its rotors turn as the bar grows; the title sets it flying.
  drone_load();
  drone_show();
}

void screens_loading(uint8_t percent)
{
  uint8_t want;

  drone_tick(0);
  if (percent > 100)
    percent = 100;
  want = (uint8_t)((uint16_t)percent * BAR_WIDTH / 100);

  while (bar_drawn < want) {
    vic4_text_char((uint8_t)(BAR_COL + bar_drawn), BAR_ROW, BOOT_BLOCK,
                   PANEL_INK);
    bar_drawn++;
  }
}

void screens_loaded(void)
{
  clear_row(PROMPT_ROW);
  clear_row(BAR_ROW);
}

void screens_load_failed(const char *why, const char *file)
{
  screens_loaded();
  centre(PROMPT_ROW, why ? why : "CANNOT READ", PANEL_INK);
  centre(BAR_ROW, file ? file : "", PANEL_INK);
}

// Back to the ROM's own eighty-column screen at $0800, for the one thing
// left that prints: the benchmark report, when REPORT_SECONDS asks for it.
void screens_boot_restore(void)
{
  VICIV.ctrlc &= (uint8_t)~(VIC4_CHR16_MASK | VIC4_FCLRHI_MASK);
  VICIV.scrnptr = 0x0800;
  VICIV.linestep = 80;
  VICIV.chrcount = 80;
  VICIV.ctrlb |= 0x80;  // H640, which is what the ROM's editor writes for
}

void screens_title(void)
{
  vic4_text_mode();
  title_lines();
  drone_show();
  centre(PROMPT_ROW, "PRESS SPACE OR FIRE", PANEL_INK);
  sound_line_help();
}

// The selected line is white whatever its state; the others are grey, or the
// panel's own green once cleared. DONE after the name is what still says so
// on the selected line. A cleared mission can still be chosen: flying one
// again is allowed, and only the record of it is fixed. The columns leave room for the longest name
// tools/campaign.py allows, 24, between the number and the tag.
#define LIST_ARROW 3
#define LIST_NUM   5
#define LIST_NAME  8
#define LIST_TAG   34
#define LIST_DONE  PANEL_TEXT

void screens_won(void)
{
  vic4_text_mode();
  title_lines();
  drone_show();
  // The drone flies over the banner as well as under it, which is what it
  // does on the title too.
  centre(13, "* * * * * * * * * * * * * *", PANEL_TEXT);
  centre(15, "ALL MISSIONS COMPLETE", PANEL_WARN);
  centre(17, "WELL DONE, PILOT", PANEL_INK);
  centre(19, "* * * * * * * * * * * * * *", PANEL_TEXT);
  centre(PROMPT_ROW, "PRESS SPACE OR FIRE", PANEL_INK);
  sound_line_help();
}

void screens_missions(uint8_t selected)
{
  uint8_t i;

  vic4_text_mode();
  centre(2, "SELECT MISSION", PANEL_INK);

  for (i = 0; i < mission_count(); i++) {
    uint8_t row = (uint8_t)(6 + i * 2);
    uint8_t cleared = (uint8_t)(missions_cleared >> i & 1);
    uint8_t ink = i == selected ? PANEL_INK
                                : (cleared ? LIST_DONE : PANEL_LABEL);

    vic4_text_char(LIST_ARROW, row, i == selected ? '>' : ' ', PANEL_INK);
    put_digits(LIST_NUM, row, (uint16_t)(i + 1), 1, ink);
    vic4_puts(LIST_NAME, row, missions[i].name, ink);
    if (cleared)
      vic4_puts(LIST_TAG, row, "DONE", LIST_DONE);
  }

  centre(PROMPT_ROW, "W S   CHOOSE      SPACE OR FIRE   BRIEF", PANEL_LABEL);
  sound_line_help();
}

void screens_briefing(uint8_t mission_no)
{
  const mission *m = &missions[mission_no];
  uint8_t i;

  vic4_text_mode();
  vic4_puts(2, 2, "MISSION", PANEL_LABEL);
  put_digits(10, 2, (uint16_t)(mission_no + 1), 1, PANEL_LABEL);
  vic4_puts(13, 2, m->name, PANEL_INK);

  // The controls are a page of their own now (screens_controls), so this is
  // only the job: what happened, where, with what, and what to do about it.
  for (i = 0; i < BRIEF_LINES; i++)
    vic4_puts(2, (uint8_t)(5 + i), m->brief[i], PANEL_INK);

  vic4_puts(2, 10, m->pickup ? "COLLECT FROM" : "LAST KNOWN POSITION",
            PANEL_LABEL);
  put_position(22, 10, m->lat, m->lon, PANEL_INK);
  vic4_puts(2, 11, "CARGO", PANEL_LABEL);
  vic4_puts(22, 11, mission_cargo_name(m), PANEL_INK);
  vic4_puts(2, 12, "WEATHER", PANEL_LABEL);
  vic4_puts(22, 12, weather_name(m->weather), PANEL_INK);

  vic4_puts(2, 15, "OBJECTIVE", PANEL_LABEL);
  vic4_puts(4, 16, m->objective, PANEL_INK);

  centre(PROMPT_ROW, "SPACE OR FIRE   LAUNCH", PANEL_LABEL);
  sound_line_help();
}

void screens_controls(uint8_t mission_no, uint8_t paused)
{
  const mission *m = &missions[mission_no];
  // Below the list and the briefing there is no mission yet, so no one
  // button: both are named, and RUN/STOP and the rest move down a row.
  uint8_t general = mission_no >= mission_count();
  uint8_t row = (uint8_t)(13 + general);

  vic4_text_mode();
  centre(2, paused ? "FLIGHT PAUSED" : "CONTROLS", PANEL_INK);

  // **A control the game does not name is a control nobody has**, and mission
  // three cannot be finished without T, so every key is here and the page is
  // reachable from every page and from the air.
  //
  // A third column for the joystick, at 31 where it clears the longest line
  // it shares: the stick is W A S D and the button is the mission's own key,
  // whichever of SPACE and RETURN that is. The key column is nine wide,
  // because RUN/STOP is eight and would otherwise touch its verb.
  vic4_puts(4, 5, "W S      FORWARD    BACK", PANEL_INK);
  vic4_puts(31, 5, "STICK", PANEL_LABEL);
  vic4_puts(4, 6, "A D      TURN LEFT  RIGHT", PANEL_INK);
  vic4_puts(31, 6, "STICK", PANEL_LABEL);
  vic4_puts(4, 7, "R F      CLIMB      DESCEND", PANEL_INK);
  vic4_puts(4, 8, "Q E      CAMERA UP  DOWN", PANEL_INK);
  vic4_puts(4, 9, "1 2 3    SPEED  SLOW NORMAL SPORT", PANEL_INK);

  // Named on every mission and not only the one that needs it: the camera
  // works over any of them, and a sensor nobody knows about is not a sensor.
  vic4_puts(4, 11, "T        THERMAL CAMERA", PANEL_INK);
  // The one line that differs between the two kinds of mission, and it comes
  // out of the mission's cargo bay rather than out of a branch here.
  if (general) {
    vic4_puts(4, 12, "SPACE    FILE REPORT", PANEL_INK);
    vic4_puts(4, 13, "RETURN   RELEASE CARGO", PANEL_INK);
    vic4_puts(31, 12, "FIRE", PANEL_LABEL);
    vic4_puts(31, 13, "FIRE", PANEL_LABEL);
  } else {
    vic4_puts(4, 12, mission_action_name(m), PANEL_INK);
    vic4_puts(13, 12, mission_action_verb(m), PANEL_INK);
    vic4_puts(31, 12, "FIRE", PANEL_LABEL);
  }
  vic4_puts(4, row, "RUN/STOP", PANEL_INK);
  vic4_puts(13, row, "ABANDON MISSION", PANEL_INK);

  vic4_puts(4, (uint8_t)(row + 2), "M        ENGINE SOUND", PANEL_INK);
  vic4_puts(4, (uint8_t)(row + 3), "HELP/F1  PAUSE AND CONTROLS", PANEL_INK);

  // From the air the page is a pause, and there is nothing for M to mute:
  // the tune is not playing and the motors are stopped until the flight goes
  // on. So the line under the prompt is only drawn off the ground.
  if (paused) {
    centre(PROMPT_ROW, "SPACE OR FIRE   RESUME", PANEL_LABEL);
  } else {
    centre(PROMPT_ROW, "SPACE OR FIRE   BACK", PANEL_LABEL);
    sound_line();
  }
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
  // Where the figure was, not where the briefing sent you: the raft drifted
  // off the wreck, and a transplant ends at the hospital it was taken to.
  put_position(12, 11, m->found_lat, m->found_lon, PANEL_INK);

  vic4_puts(12, 13, "FLIGHT TIME", PANEL_LABEL);
  put_digits(24, 13, seconds / 60, 2, PANEL_INK);
  vic4_text_char(26, 13, ':', PANEL_INK);
  put_digits(27, 13, seconds % 60, 2, PANEL_INK);

  centre(PROMPT_ROW, "SPACE OR FIRE   CONTINUE", PANEL_LABEL);
  sound_line_help();
}
