#include <calypsi/stubs.h>
#include <fcntl.h>
#include <stdio.h>

#include "dma.h"
#include "exo.h"
#include "loader.h"

#define CHUNK 2048

// Kernal reads land in the 64K address space, so everything is bounced
// through here and DMAd up into the banks above.
static uint8_t bounce[CHUNK];

// Held until the display is in full-colour mode: uploading it while the text
// screen is still up would recolour the loading message out of existence.
static uint8_t palette[768];

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
    printf("CANNOT OPEN %s\n", name);
  return fd;
}

// Read `length` bytes from an already-open file into `dest`, through the
// bounce buffer.
static int read_far(int fd, const char *name, uint32_t dest, uint32_t length)
{
  while (length) {
    uint16_t want = length > CHUNK ? CHUNK : (uint16_t)length;
    size_t got = _Stub_read(fd, bounce, want);

    if (got != want) {
      printf("%s: SHORT BY %u BYTES\n", name, (unsigned)(want - got));
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

static int load_palette(const char *name)
{
  int fd = open_resource(name);
  size_t n;

  if (fd < 0)
    return -1;
  n = _Stub_read(fd, palette, sizeof palette);
  _Stub_close(fd);

  if (n != sizeof palette) {
    printf("%s: GOT %u BYTES, WANTED 768\n", name, (unsigned)n);
    return -1;
  }
  return 0;
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
  if (load_palette("TERRAIN.PAL"))
    return -1;
  if (load_far("TERRAIN.OVR", OVERVIEW, OVERVIEW_BYTES))
    return -1;
  return 0;
}

const uint8_t *loaded_palette(void)
{
  return palette;
}
