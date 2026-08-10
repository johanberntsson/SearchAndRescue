#include <calypsi/stubs.h>
#include <fcntl.h>
#include <stdio.h>

#include "dma.h"
#include "exo.h"
#include "loader.h"
#include "sprite.h"

// Down from 2048 when the survivor sprite's tables went in: the program, its
// data and its stack share 32K and the slack is now under a hundred bytes at
// WIDE=1, where the per-ray tables are twice the size. More Kernal calls per
// resource costs nothing measurable against half a kilobyte of copying each.
#define CHUNK 512

// Kernal reads land in the 64K address space, so everything is bounced
// through here and DMAd up into the banks above.
static uint8_t bounce[CHUNK];

// Straight to the Kernal stubs rather than through stdio: the buffering the
// FILE layer adds is pure overhead when every read is a whole chunk.
//
// Reading a SEQ file through the Kernal reports EOF exactly 256 bytes early,
// whatever the file's size, so nothing here relies on reaching the end: each
// resource is read to a known length and tools/convmap.py pads the files past
// the unreachable tail.
static int open_resource(const char *name)
{
  int fd;

  // Named as they go by: the maps take the best part of a minute between
  // them, and without this there is no way to tell a slow load from a stuck
  // one.
  printf("%s\n", name);
  fd = _Stub_open(name, O_RDONLY | O_BINARY);

  if (fd < 0)
    printf("CANNOT OPEN %s\n", name);
  return fd;
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
// bounce buffer.
static int read_far(int fd, const char *name, uint32_t dest, uint32_t length)
{
  while (length) {
    uint16_t want = length > CHUNK ? CHUNK : (uint16_t)length;

    if (read_exact(fd, bounce, want) != want) {
      printf("%s: SHORT READ\n", name);
      return -1;
    }
    dma_copy((uint32_t)(uint16_t)bounce, dest, want);
    dest += want;
    length -= want;
  }
  return 0;
}

// The maps are exomizer-crunched (tools/convmap.py), each prefixed with its
// own crunched length: read that, stage the stream in attic RAM, unpack it to
// `dest`. Nothing here looks for EOF, which the Kernal reports early.
static int load_crunched(const char *name, uint32_t dest)
{
  int fd = open_resource(name);
  uint32_t length;

  if (fd < 0)
    return -1;

  if (_Stub_read(fd, &length, sizeof length) != sizeof length) {
    _Stub_close(fd);
    printf("%s: NO LENGTH\n", name);
    return -1;
  }
  if (read_far(fd, name, ATTIC_STAGE, length)) {
    _Stub_close(fd);
    return -1;
  }
  _Stub_close(fd);

  exo_decrunch(ATTIC_STAGE, dest);
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

int load_resources(void)
{
  printf("LOADING TERRAIN...\n");
  if (load_crunched("TERRAIN.HGT", HEIGHTMAP))
    return -1;
  if (load_crunched("TERRAIN.COL", COLOURMAP))
    return -1;
  // The palette waits in banked RAM until the display is in full-colour mode:
  // uploading it while the text screen is still up would recolour the loading
  // message out of existence.
  //
  // Read into the bounce buffer and moved up afterwards, rather than through
  // load_far, which HANGS on this one file: the Kernal never returns from the
  // open or the read that follows it. Same file, same 768 bytes, same buffer,
  // any destination -- only the call path differs, and load_far reads
  // TERRAIN.OVR two lines below perfectly happily. Not understood; the
  // arrangement below is the one that works.
  if (load_small("TERRAIN.PAL", bounce, PALETTE_BYTES) != PALETTE_BYTES)
    return -1;
  dma_copy((uint32_t)(uint16_t)bounce, PALETTE_BUF, PALETTE_BYTES);
  if (load_far("TERRAIN.OVR", OVERVIEW, OVERVIEW_BYTES))
    return -1;
  if (sprite_load())
    return -1;
  return 0;
}

const uint8_t __far *loaded_palette(void)
{
  return (const uint8_t __far *)PALETTE_BUF;
}
