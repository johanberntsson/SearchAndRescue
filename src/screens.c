#include "screens.h"

#include <stdio.h>

#include "mission.h"
#include "panel.h"
#include "vic4.h"

#define TITLE_ROW   8
#define PROMPT_ROW  22
#define BAR_WIDTH   30

// PETSCII 160 is a shifted space: a solid block, and a filled bar without
// shipping a font to draw one with.
#define BOOT_BLOCK 160

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

// How many blocks of the loading bar have been printed. The bar grows and is
// never redrawn, which is what lets it live on a screen we only have a
// character stream to.
static uint8_t bar_drawn;

void screens_boot(void)
{
  putchar(147);  // clear
  printf("\n\n     SEARCH AND RESCUE\n");
  printf("     A MEGA65 DRONE SIMULATOR\n\n\n");
  printf("     LOADING\n\n     ");
  bar_drawn = 0;
}

void screens_loading(uint8_t percent)
{
  uint8_t want;

  if (percent > 100)
    percent = 100;
  want = (uint8_t)((uint16_t)percent * BAR_WIDTH / 100);

  while (bar_drawn < want) {
    putchar(BOOT_BLOCK);
    bar_drawn++;
  }
}

void screens_load_failed(const char *why, const char *file)
{
  printf("\n\n     %s %s\n", why ? why : "CANNOT READ", file ? file : "");
}

void screens_title(void)
{
  vic4_text_mode();
  centre(TITLE_ROW, "SEARCH AND RESCUE", PANEL_INK);
  centre(TITLE_ROW + 2, "A MEGA65 DRONE SIMULATOR", PANEL_LABEL);
  centre(PROMPT_ROW, "PRESS SPACE", PANEL_INK);
}

void screens_missions(uint8_t selected)
{
  uint8_t i;

  vic4_text_mode();
  centre(2, "SELECT MISSION", PANEL_INK);

  for (i = 0; i < MISSION_COUNT; i++) {
    uint8_t row = (uint8_t)(6 + i * 2);
    uint8_t ink = i == selected ? PANEL_INK : PANEL_LABEL;

    vic4_text_char(6, row, i == selected ? '>' : ' ', PANEL_INK);
    put_digits(8, row, (uint16_t)(i + 1), 1, ink);
    vic4_puts(11, row, missions[i].name, ink);
  }

  centre(PROMPT_ROW, "W S   CHOOSE      SPACE   BRIEF", PANEL_LABEL);
}

void screens_briefing(uint8_t mission_no)
{
  const mission *m = &missions[mission_no];
  uint8_t i;

  vic4_text_mode();
  vic4_puts(2, 1, "MISSION", PANEL_LABEL);
  put_digits(10, 1, (uint16_t)(mission_no + 1), 1, PANEL_LABEL);
  vic4_puts(13, 1, m->name, PANEL_INK);

  for (i = 0; i < BRIEF_LINES; i++)
    vic4_puts(2, (uint8_t)(4 + i), m->brief[i], PANEL_INK);

  vic4_puts(2, 8, "LAST KNOWN POSITION", PANEL_LABEL);
  put_position(22, 8, m, PANEL_INK);
  vic4_puts(2, 9, "CARGO", PANEL_LABEL);
  vic4_puts(22, 9, mission_cargo_name(m), PANEL_INK);

  vic4_puts(2, 11, "OBJECTIVE", PANEL_LABEL);
  vic4_puts(4, 12, m->objective, PANEL_INK);

  vic4_puts(2, 14, "CONTROLS", PANEL_LABEL);
  vic4_puts(4, 15, "W S     FORWARD    BACK", PANEL_INK);
  vic4_puts(4, 16, "A D     TURN LEFT  RIGHT", PANEL_INK);
  vic4_puts(4, 17, "R F     CLIMB      DESCEND", PANEL_INK);
  vic4_puts(4, 18, "Q E     CAMERA UP  DOWN", PANEL_INK);
  vic4_puts(4, 19, "1 2 3   SPEED  SLOW NORMAL SPORT", PANEL_INK);
  // The one line that differs between the two kinds of mission, and it comes
  // out of the mission's cargo bay rather than out of a branch here.
  vic4_puts(4, 20, mission_action_name(m), PANEL_INK);
  vic4_puts(12, 20, mission_action_verb(m), PANEL_INK);
  // Split so that the verb starts at column 12 like every row above it:
  // RUN/STOP is exactly as wide as the key column.
  vic4_puts(4, 21, "RUN/STOP", PANEL_INK);
  vic4_puts(12, 21, "ABANDON MISSION", PANEL_INK);

  centre(PROMPT_ROW, "SPACE   LAUNCH", PANEL_LABEL);
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
}
