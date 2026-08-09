#include <calypsi/stubs.h>
#include <fcntl.h>
#include <stdio.h>

#include "dma.h"
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

static int load_far(const char *name, uint32_t dest, uint32_t length)
{
  int fd = open_resource(name);

  if (fd < 0)
    return -1;

  while (length) {
    uint16_t want = length > CHUNK ? CHUNK : (uint16_t)length;
    size_t got = _Stub_read(fd, bounce, want);

    if (got != want) {
      _Stub_close(fd);
      printf("%s: SHORT BY %u BYTES\n", name, (unsigned)(want - got));
      return -1;
    }
    dma_copy((uint32_t)(uint16_t)bounce, dest, want);
    dest += want;
    length -= want;
  }
  _Stub_close(fd);
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

int load_resources(void)
{
  printf("LOADING TERRAIN...\n");
  if (load_far("TERRAIN.HGT", HEIGHTMAP, MAP_BYTES))
    return -1;
  if (load_far("TERRAIN.COL", COLOURMAP, COL_BYTES))
    return -1;
  if (load_palette("TERRAIN.PAL"))
    return -1;
  return 0;
}

const uint8_t *loaded_palette(void)
{
  return palette;
}
