#include "mission.h"

#include "input.h"
#include "loader.h"

// The campaign as it comes off the disk: a header, one fixed record per
// mission, and a pool of strings the records point into by offset. Written by
// tools/campaign.py, which owns the layout below and checks everything about
// it -- the text is uppercased and wrapped there, the fixes are checked
// against the map's own bounds there, and a campaign too big for this buffer
// is refused there. Nothing here validates the words; there is no display to
// report anything on and no room for the code to do it.
//
// **The fixes are the whole of a mission's geography**: src/sprite.c stands
// the figure at one and the briefing reads it out loud, because the panel's
// own LAT/LON readout is the only way to navigate and a search area you
// cannot name is not a search area. A fix is a cell of one particular map and
// moves when that map is re-rolled -- the mission files say so where a pilot
// will read it.
static uint8_t campaign[CAMPAIGN_BYTES];

// The header. Four bytes of magic, so a stale disk is caught rather than
// flown, and then the three counts that say what else is on the disk.
#define HEAD_MAGIC    0
#define HEAD_MISSIONS 4
#define HEAD_MAPS     5
#define HEAD_FIGURES  6
#define HEAD_BYTES    8

// A record, and **every one of these is a byte offset**, including the
// sixteen-bit ones. They were field indices for one commit and the three
// bytes at the end were read three fields early -- the figure came out 170,
// the weather 31, and the second mission flew with no billboard and no rain.
// Keep them beside the writer's own list in tools/campaign.py.
//
//   0 name   2 brief[0..2]   8 objective   10 cargo   12 done   14 lost
//  16 lat   18 lon           20 figure     21 weather 22 map    23 spare
#define REC_BYTES     24
#define REC_NAME      0
#define REC_BRIEF     2   // and two more, two bytes apart
#define REC_OBJECTIVE 8
#define REC_CARGO     10
#define REC_DONE      12
#define REC_LOST      14
#define REC_LAT       16
#define REC_LON       18
#define REC_FIGURE    20
#define REC_WEATHER   21
#define REC_MAP       22

mission missions[MISSION_MAX];

static uint8_t count, maps, figures;

uint8_t mission_count(void)
{
  return count;
}

uint8_t campaign_maps(void)
{
  return maps;
}

uint8_t campaign_figures(void)
{
  return figures;
}

// The sixteen-bit field at a byte offset into a record, little-endian as the
// tool wrote it.
//
// **The two halves are hoisted into locals on purpose.** Written as the one
// expression it wants to be --
//
//     return (uint16_t)rec[at] | (uint16_t)rec[at + 1] << 8;
//
// -- Calypsi 5.18 emits ONE indirect load, for the high byte, and fills the
// low byte with the high byte of the address it just computed. Every string
// offset and both fixes came back with the right high byte and a low byte of
// $82, which is where the buffer happens to sit. The listing shows it in one
// look (`--list-file`): no second `lda (ptr),z`. Widening `at` to uint16_t
// cures it too; this form is simply the clearest of the three that work. One
// more for the family under Gotchas in CLAUDE.md.
static uint16_t word(const uint8_t *rec, uint8_t at)
{
  uint16_t lo = rec[at];
  uint16_t hi = rec[at + 1];

  return lo | hi << 8;
}

// An offset into the blob as a pointer, and zero as "there is none". Nothing
// can live at offset 0 -- the magic is there -- so the empty case comes for
// free rather than needing a flag beside it.
static const char *str(uint16_t offset)
{
  return offset ? (const char *)(campaign + offset) : 0;
}

const char *campaign_load(void)
{
  uint8_t n;

  if (load_small("CAMPAIGN.BIN", campaign, CAMPAIGN_BYTES) != CAMPAIGN_BYTES)
    return "SHORT READ";
  if (campaign[HEAD_MAGIC] != 'S' || campaign[HEAD_MAGIC + 1] != 'A'
      || campaign[HEAD_MAGIC + 2] != 'R' || campaign[HEAD_MAGIC + 3] != 1)
    return "NOT A CAMPAIGN";

  count = campaign[HEAD_MISSIONS];
  maps = campaign[HEAD_MAPS];
  figures = campaign[HEAD_FIGURES];
  if (!count || count > MISSION_MAX)
    return "MISSION COUNT";

  for (n = 0; n < count; n++) {
    const uint8_t *rec = campaign + HEAD_BYTES + (uint16_t)n * REC_BYTES;
    mission *m = &missions[n];
    uint8_t i;

    m->name = str(word(rec, REC_NAME));
    for (i = 0; i < BRIEF_LINES; i++)
      m->brief[i] = str(word(rec, (uint8_t)(REC_BRIEF + i * 2)));
    m->objective = str(word(rec, REC_OBJECTIVE));
    m->cargo = str(word(rec, REC_CARGO));
    m->done = str(word(rec, REC_DONE));
    m->lost = str(word(rec, REC_LOST));
    m->lat = word(rec, REC_LAT);
    m->lon = word(rec, REC_LON);
    m->figure = rec[REC_FIGURE];
    m->weather = rec[REC_WEATHER];
    m->map = rec[REC_MAP];
  }
  return 0;
}

uint16_t mission_action_key(const mission *m)
{
  return m->cargo ? KEY_RETURN : KEY_SPACE;
}

const char *mission_action_name(const mission *m)
{
  return m->cargo ? "RETURN" : "SPACE";
}

const char *mission_action_verb(const mission *m)
{
  return m->cargo ? "RELEASE CARGO" : "FILE REPORT";
}

const char *mission_cargo_name(const mission *m)
{
  return m->cargo ? m->cargo : "EMPTY";
}
