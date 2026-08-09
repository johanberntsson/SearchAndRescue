// Exomizer decruncher, unpacking straight into attic RAM.
#ifndef EXO_H
#define EXO_H

#include <stdint.h>

// Decrunch the stream at `src` to `dst`. Both are full 28-bit addresses, so
// either can be chip RAM or attic RAM. The stream carries its own end marker;
// the caller is trusted to have put a matching one there.
void exo_decrunch(uint32_t src, uint32_t dst);

#endif
