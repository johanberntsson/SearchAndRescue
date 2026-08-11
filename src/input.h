// Held-key state for flight controls, read straight off the CIA1 matrix.
#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

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

// Scan the matrix once. `held` gets every key down now, which is what flight
// wants; `pressed` gets the ones that went down since the last scan, which is
// what a menu wants and what the report button wants, since holding it must
// not file twice. Either may be null.
//
// One call for both because an edge only means anything against the scan
// before it: two separate scans in the same frame would leave the second one
// seeing no edges at all.
void input_scan(uint16_t *held, uint16_t *pressed);

// Forget what is held, so that a key still down from the last screen does not
// read as a fresh press on the next one.
void input_flush(void);

#endif
