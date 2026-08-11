#include <calypsi/stubs.h>
#include <fcntl.h>

#include "dma.h"
#include "exo.h"
#include "loader.h"
#include "sprite.h"

// How much of the staging buffer one Kernal read fills. Down from 2048 when
// the survivor sprite's tables went in: the program, its data and its stack
// share 32K. More Kernal calls per resource costs nothing measurable against
// half a kilobyte of copying each.
//
// This is a *chunk size*, not the buffer size. The palette is read whole and
// is larger; see LOAD_STAGING.
#define CHUNK 512

// Kernal reads land in the 64K address space, so everything is bounced
// through here and DMAd up into the banks above. Shared with src/sprite.c,
// which draws the flight's figure straight out of it -- see LOAD_STAGING in
// loader.h for why the two can never collide.
uint8_t load_staging[LOAD_STAGING];

// What went wrong, for the screen to show. Loading happens with the game's
// own display up, so a printf goes somewhere nobody can see -- and worse,
// through the Kernal's screen editor, which writes the colour RAM the game is
// using. This is the only channel for a failure.
static const char *failure;
static const char *failed_file;

static int fail(const char *why, const char *name)
{
  failure = why;
  failed_file = name;
  return -1;
}

const char *loader_error(void)
{
  return failure;
}

const char *loader_error_file(void)
{
  return failed_file;
}

// Straight to the Kernal stubs rather than through stdio: the buffering the
// FILE layer adds is pure overhead when every read is a whole chunk.
//
// Reading a SEQ file through the Kernal reports EOF exactly 256 bytes early,
// whatever the file's size, so nothing here relies on reaching the end: each
// resource is read to a known length and tools/convmap.py pads the files past
// the unreachable tail.
static int open_resource(const char *name)
{
  int fd = _Stub_open(name, O_RDONLY | O_BINARY);

  if (fd < 0)
    fail("CANNOT OPEN", name);
  return fd;
}

// Loading takes most of a minute, so the title screen shows how far along it
// is. Each resource owns a slice of the bar and reports its own bytes within
// that slice. The slices are the crunched sizes at the default map settings
// -- at other settings the bar moves unevenly, which is all a bar owes
// anybody as long as it only ever goes forwards.
static load_progress prog_report;
static uint8_t prog_base, prog_span;
static uint32_t prog_total, prog_done;

static void progress(void)
{
  if (!prog_report)
    return;
  prog_report((uint8_t)(prog_base + (prog_total
                                         ? (uint8_t)((uint32_t)prog_span *
                                                     prog_done / prog_total)
                                         : prog_span)));
}

static void progress_to(uint8_t percent)
{
  prog_base = percent;
  prog_total = 0;
  prog_span = 0;
  progress();
}

// Read `length` bytes into a near buffer, returning how many arrived.
//
// Nothing promises that one Kernal read delivers everything it was asked for,
// so keep asking until it does and treat a read of nothing at all as the end
// of the file. (Every read this program makes has in fact come back full, so
// the loop is belt and braces rather than a fix for anything observed.)
static uint16_t read_exact(int fd, uint8_t *dest, uint16_t length)
{
  uint16_t left = length;

  while (left) {
    size_t got = _Stub_read(fd, dest, left);

    if (!got || got > left)
      break;
    dest += got;
    left -= (uint16_t)got;
  }
  return length - left;
}

// Read `length` bytes from an already-open file into `dest`, through the
// staging buffer, CHUNK bytes at a time.
static int read_far(int fd, const char *name, uint32_t dest, uint32_t length)
{
  while (length) {
    uint16_t want = length > CHUNK ? CHUNK : (uint16_t)length;

    if (read_exact(fd, load_staging, want) != want)
      return fail("SHORT READ", name);
    dma_copy((uint32_t)(uint16_t)load_staging, dest, want);
    dest += want;
    length -= want;
    prog_done += want;
    progress();
  }
  return 0;
}

// The maps are exomizer-crunched (tools/convmap.py), each prefixed with its
// own crunched length: read that, stage the stream in attic RAM, unpack it to
// `dest`. Nothing here looks for EOF, which the Kernal reports early.
static int load_crunched(const char *name, uint32_t dest, uint8_t base,
                         uint8_t span)
{
  int fd = open_resource(name);
  uint32_t length;

  if (fd < 0)
    return -1;

  if (_Stub_read(fd, &length, sizeof length) != sizeof length) {
    _Stub_close(fd);
    return fail("NO LENGTH IN", name);
  }

  // Three quarters of the slice for the read and the last quarter for the
  // decrunch, which is not free and would otherwise be a stalled bar.
  prog_base = base;
  prog_span = (uint8_t)(span * 3 / 4);
  prog_total = length;
  prog_done = 0;

  if (read_far(fd, name, ATTIC_STAGE, length)) {
    _Stub_close(fd);
    return -1;
  }
  _Stub_close(fd);

  exo_decrunch(ATTIC_STAGE, dest);
  progress_to((uint8_t)(base + span));
  return 0;
}

int load_small(const char *name, void *dest, uint16_t length)
{
  int fd = open_resource(name);
  uint16_t got;

  if (fd < 0)
    return -1;

  // Getting less than was asked for is not a failure here: the Kernal stops
  // 256 bytes before the real end of a SEQ file and tools/convmap.py pads
  // past that, so asking for more than the file holds is the normal way to
  // read a whole one. The caller checks it got what it needed.
  got = read_exact(fd, dest, length);
  _Stub_close(fd);
  return (int)got;
}

// A resource that is neither crunched nor small enough to want in a C array:
// straight off the disk into banked RAM at a known length.
static int load_far(const char *name, uint32_t dest, uint32_t length)
{
  int fd = open_resource(name);
  int r;

  if (fd < 0)
    return -1;
  r = read_far(fd, name, dest, length);
  _Stub_close(fd);
  return r;
}

int load_resources(load_progress report)
{
  prog_report = report;
  progress_to(0);

  if (load_crunched("TERRAIN.HGT", HEIGHTMAP, 0, 20))
    return -1;
  if (load_crunched("TERRAIN.COL", COLOURMAP, 20, 76))
    return -1;
  // The palette waits in banked RAM until the display is in full-colour mode:
  // uploading it while the text screen is still up would recolour the loading
  // message out of existence.
  //
  // Read into the staging buffer and moved up afterwards, rather than through
  // load_far, which HANGS on this one file: the Kernal never returns from the
  // open or the read that follows it. Same file, same 768 bytes, same buffer,
  // any destination -- only the call path differs, and load_far reads
  // TERRAIN.OVR two lines below perfectly happily. Not understood; the
  // arrangement below is the one that works.
  // Whole rather than in chunks, and it is the reason LOAD_STAGING is sized
  // the way it is: 768 bytes went into a 512-byte buffer here for several
  // commits, overrunning it by 256 every boot.
  if (load_small("TERRAIN.PAL", load_staging, PALETTE_BYTES) != PALETTE_BYTES)
    return fail("SHORT READ", "TERRAIN.PAL");
  dma_copy((uint32_t)(uint16_t)load_staging, PALETTE_BUF, PALETTE_BYTES);
  progress_to(97);
  if (load_far("TERRAIN.OVR", OVERVIEW, OVERVIEW_BYTES))
    return -1;
  progress_to(98);
  {
    const char *bad = sprite_load();

    if (bad)
      return fail("BAD SPRITE IN", bad);
  }
  progress_to(100);
  return 0;
}

const uint8_t __far *loaded_palette(void)
{
  return (const uint8_t __far *)PALETTE_BUF;
}
