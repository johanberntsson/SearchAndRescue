// The interrupt everything audible hangs off.
//
// The ROM's own handler is saved and chained to, so its raster compare, its
// keyboard scan and its jiffy clock go on exactly as before and nothing here
// has to know which line it asks for. See src/audio_irq.s.
//
// Two things use it and never at the same time: music.h's tune, which belongs
// to the menus, and engine.h's note, which belongs to the flight.
#ifndef AUDIO_H
#define AUDIO_H

// Take the interrupt over. Call once, as early as there is anything to look
// at; nothing is heard until something arms itself.
void audio_begin(void);

// Both SIDs. The MEGA65 has one per stereo channel, $D400 on the left and
// $D420 on the right, so everything that makes a sound writes both -- a C64's
// $D420 is a partly decoded mirror of $D400, so the same code still runs
// there. If it ever comes out of one speaker on real hardware, this is the
// constant to move.
#define SID_BASE  0xD400
#define SID2_BASE 0xD420

#endif
