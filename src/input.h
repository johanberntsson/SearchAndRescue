// Held-key state for flight controls, read straight off the CIA1 matrix.
#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

// A set of keys. **It was sixteen bits and every one of them was taken**, so
// the thermal camera's `T` is what widened it: the game has seventeen keys
// now. Nothing on a 6502 likes a 32-bit word, but this one is anded and tested
// a handful of times a frame and never in a loop, and the alternative was two
// words with a rule about which key lives in which.
typedef uint32_t keymask;

#define KEY_W     0x0001
#define KEY_A     0x0002
#define KEY_S     0x0004
#define KEY_D     0x0008
#define KEY_R     0x0010
#define KEY_F     0x0020
#define KEY_Q     0x0040  // gimbal up
#define KEY_E     0x0080  // gimbal down
#define KEY_1     0x0100  // speed: cinematic
#define KEY_2     0x0200  // speed: normal
#define KEY_3     0x0400  // speed: sport
#define KEY_SPACE  0x0800  // file a report, and "go on" on every screen
#define KEY_RETURN 0x1000  // release the cargo
#define KEY_STOP   0x2000  // RUN/STOP: give up and fly home
#define KEY_M      0x4000  // mute: the music on a page, the engine in the air
#define KEY_P      0x8000  // performance: show the frame rate. Undocumented
#define KEY_T    0x010000  // arm the thermal camera; see src/thermal.h

// Scan the matrix once. `held` gets every key down now, which is what flight
// wants; `pressed` gets the ones that went down since the last scan, which is
// what a menu wants and what the report button wants, since holding it must
// not file twice. Either may be null.
//
// One call for both because an edge only means anything against the scan
// before it: two separate scans in the same frame would leave the second one
// seeing no edges at all.
void input_scan(keymask *held, keymask *pressed);

// Forget what is held, so that a key still down from the last screen does not
// read as a fresh press on the next one.
void input_flush(void);

#endif
