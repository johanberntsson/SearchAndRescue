#include "engine.h"

#include <mega65.h>

#include "audio.h"

// A SID frequency register value for a pitch in Hz on a PAL machine, where
// the register is f * 2^24 / 985248 = f * 17.028. 17 is 0.17% flat, which is
// three cents -- nobody is tuning an instrument to a quadcopter.
#define HZ(f) ((uint16_t)((f) * 17L))

// What the motors sound like at each speed mode with the sticks centred, and
// what asking for movement adds. Sport is louder in pitch before the drone
// has moved at all, the same way it drains the battery faster before the
// drone has moved at all: the mode is how hard the props are being worked.
//
// **These are guesses that were never listened to while they were written**,
// so they are named and gathered here rather than spread through the code.
// This block, RATE and DETUNE are the whole of what the engine sounds like.
static const uint16_t idle_hz[3] = {HZ(85), HZ(100), HZ(125)};
static const uint16_t move_hz[3] = {HZ(35), HZ(50), HZ(85)};
#define CLIMB_HZ   HZ(20)  // lifting a drone costs the motors more
#define DESCEND_HZ HZ(12)  // and dropping it costs them less
#define COLD_HZ    HZ(30)  // where the motors start at launch

// Voice 2 runs this far above voice 1, so the two beat against each other at
// about four times a second. That throb is most of what makes the note read
// as rotors rather than as an organ.
#define DETUNE 60

// Frequency units the interrupt moves per tick. Fifty ticks a second, so
// opening the throttle in normal mode (850 units) takes about two thirds of a
// second and a launch from cold takes rather more than one.
#define RATE 24

// Shared with src/engine_asm.s, which is the interrupt's half of this.
uint8_t engine_on;       // whether the interrupt does anything
uint16_t engine_freq;    // where the note is now; the interrupt owns it
uint16_t engine_target;  // where it should be; set below, once a frame

// From src/engine_asm.s. Called once here as well as from the interrupt, to
// get a frequency into the voices before their gates open.
void engine_tick(void);

// Every write goes to both SIDs: one per stereo channel. See audio.h.
static void sid_put(uint8_t reg, uint8_t value)
{
  ((volatile uint8_t *)SID_BASE)[reg] = value;
  ((volatile uint8_t *)SID2_BASE)[reg] = value;
}

// Clear both SIDs and hold the gates low for about a frame.
//
// **This is what makes the engine audible at all**, and the first version
// without it produced exactly one click at launch and then silence for the
// whole flight. Two SID rules meet here, and either one alone is enough to
// kill the note:
//
//   - **an envelope only triggers on a 0 -> 1 edge of the gate bit.** The
//     tune leaves all three of its voices gated ON -- read out of the
//     player's own record of what it last wrote -- and music_set(0) used to
//     take only the master volume away. So the gates were still high, and
//     writing a waveform with the gate bit set changes the tone without ever
//     starting a note.
//   - **raising the sustain level during the sustain phase drains the
//     envelope to zero.** The phase holds only while the counter EQUALS the
//     sustain register; anything else keeps it falling. The tune's bass and
//     lead sit at sustain 10 and 11, the engine asks for 14 and 12, and with
//     no gate edge to start a fresh attack both voices simply drained away.
//     That is the click: the volume coming back up over envelopes on their
//     way to nothing.
//
// Clearing the whole block rather than just the gates also puts $D417 back,
// which routes voices into the filter. A voice filtered with no filter mode
// selected in $D418 is a third way to write a note and hear nothing.
//
// The wait is the hard restart the tune's own player does before every note.
// With SR zeroed the release is at its fastest, so a frame is far more than
// the envelopes need to reach zero before they are asked to attack.
static void gate_low(void)
{
  uint8_t r;
  uint16_t lines = 320;  // a PAL frame is 312
  uint8_t last;

  for (r = 0; r <= 0x18; r++)
    sid_put(r, 0);

  last = VICII.rasterline;
  while (lines) {
    uint8_t now;
    uint16_t guard = 0;

    // Bounded on purpose. A raster that never moves would otherwise hang the
    // game on the launch of every flight, which is a far worse fault than a
    // missing engine note; the guard wraps and gives up on the line instead.
    do {
      now = VICII.rasterline;
    } while (now == last && ++guard);

    last = now;
    lines--;
  }
}

// Waveform and gate. Sawtooth for the note, a narrow pulse beside it for the
// beat, and a triangle an octave down for the body of it.
#define WAVE_1 0x21  // sawtooth, gate on
#define WAVE_2 0x41  // pulse
#define WAVE_3 0x11  // triangle

void engine_start(void)
{
  uint8_t v;

  // Sustain is the only per-voice volume a SID has -- there is one master
  // level and nothing else -- so the balance between the three voices is
  // these three numbers. Attack is short but not zero, to keep the launch
  // from starting with a click.
  static const uint8_t sustain[3] = {0xE4, 0xC4, 0xA4};
  static const uint8_t wave[3] = {WAVE_1, WAVE_2, WAVE_3};

  engine_freq = COLD_HZ;
  engine_target = idle_hz[1];

  // Whoever had the SID before this leaves its voices gated on, so the gates
  // have to fall before they can rise. See gate_low: without it the envelopes
  // never trigger and a flight is silent.
  gate_low();

  // A pitch in the voices before their gates open, or the first twentieth of
  // a second of the note is at frequency zero. This is the same routine the
  // interrupt runs, and it is safe to call from here: leaf assembly with no
  // stack or zero page of its own.
  engine_tick();

  for (v = 0; v < 3; v++) {
    uint8_t base = (uint8_t)(v * 7);

    sid_put((uint8_t)(base + 2), 0x00);  // pulse width, voice 2's alone
    sid_put((uint8_t)(base + 3), 0x05);  // matters: 12.5%, thin and reedy
    sid_put((uint8_t)(base + 5), 0x20);  // attack 2, decay 0
    sid_put((uint8_t)(base + 6), sustain[v]);
    sid_put((uint8_t)(base + 4), wave[v]);  // and the gate opens
  }

  sid_put(0x18, 0x0F);
  engine_on = 1;
}

void engine_stop(void)
{
  uint8_t v;

  // Stop the interrupt before touching the SID, or it would write a frequency
  // over the top of the silence.
  engine_on = 0;
  for (v = 0; v < 3; v++)
    sid_put((uint8_t)(v * 7 + 4), 0x00);  // gate off
  sid_put(0x18, 0x00);
}

void engine_throttle(uint8_t mode, uint8_t moving, int8_t climb)
{
  uint16_t f = idle_hz[mode];

  if (moving)
    f += move_hz[mode];
  if (climb > 0)
    f += CLIMB_HZ;
  else if (climb < 0)
    f -= DESCEND_HZ;

  // Written in two halves while the interrupt may be reading it, so a tick
  // can catch a target that is half old. It costs one tick of walking in the
  // wrong direction -- at most RATE units, a fifth of a semitone -- and the
  // next tick corrects it. Guarding it would cost an SEI every frame.
  engine_target = f;
}
