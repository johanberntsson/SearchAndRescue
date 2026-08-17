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

// Master volume, out of the SID's fifteen. Half, because the first version at
// full was too loud to fly under. The tune keeps its own -- the two never
// sound at the same time, so each sets the level it wants when it starts.
#define ENGINE_VOLUME 7

// Sustain is the only per-voice volume a SID has -- there is one master level
// and nothing else -- so the balance between the three voices is these three
// numbers. Attack is short but not zero, to keep the launch from starting
// with a click. At file scope because the battery beep borrows voice 3 and
// has to be able to hand it back exactly as it found it.
static const uint8_t sustain[3] = {0xE4, 0xC4, 0xA4};
#define ENGINE_AD 0x20  // attack 2, decay 0, for all three

static void engine_arm(void)
{
  uint8_t v;

  static const uint8_t wave[3] = {WAVE_1, WAVE_2, WAVE_3};

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
    sid_put((uint8_t)(base + 5), ENGINE_AD);
    sid_put((uint8_t)(base + 6), sustain[v]);
    sid_put((uint8_t)(base + 4), wave[v]);  // and the gate opens
  }

  sid_put(0x18, ENGINE_VOLUME);
  engine_on = 1;
}

static void engine_silence(void)
{
  uint8_t v;

  // Stop the interrupt before touching the SID, or it would write a frequency
  // over the top of the silence.
  engine_on = 0;
  for (v = 0; v < 3; v++)
    sid_put((uint8_t)(v * 7 + 4), 0x00);  // gate off
  sid_put(0x18, 0x00);
}

void engine_start(uint8_t heard)
{
  engine_freq = COLD_HZ;
  engine_target = idle_hz[1];

  if (heard)
    engine_arm();
}

void engine_set(uint8_t on)
{
  if ((on != 0) == (engine_on != 0))
    return;

  if (on)
    engine_arm();  // from wherever the throttle has got to, not from cold
  else
    engine_silence();
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

// The battery warning, on the engine's third voice.
//
// **Shared with src/engine_asm.s**, which leaves voice 3's frequency alone
// while this is non-zero -- otherwise the interrupt would write the note's
// pitch over the beep fifty times a second. Counted down here, once a frame,
// rather than in the interrupt: every SID write in a flight is then on this
// side of the fence and there is nothing to race.
uint8_t engine_beep_left;

// Voice 3's registers. The SID gives each voice seven, so the third's begin
// at 14: frequency, then pulse width, control, attack/decay, sustain/release.
#define V3_FREQ 14
#define V3_CTRL 18
#define V3_AD   19
#define V3_SR   20

// A third of a second at the frame rates a flight runs at, and half again
// for the second warning. The pitches are a fifth apart, which is enough to
// tell them apart without either sounding like the motors.
#define BEEP_WARN_HZ    HZ(660)
#define BEEP_ALARM_HZ   HZ(990)
#define BEEP_WARN_FRAMES  4
#define BEEP_ALARM_FRAMES 7

void engine_beep(uint8_t level)
{
  uint16_t hz = level >= 2 ? BEEP_ALARM_HZ : BEEP_WARN_HZ;

  // Nothing at all when the flight is muted: this is the flight's own
  // channel, and M means a quiet one.
  if (!engine_on)
    return;

  engine_beep_left = level >= 2 ? BEEP_ALARM_FRAMES : BEEP_WARN_FRAMES;

  // Gate down first and then up, because an envelope triggers only on that
  // edge -- the whole reason the engine has a gate_low at all. Voice 3 has
  // been gated on since launch, so without the falling edge this would change
  // its pitch and nothing else.
  sid_put(V3_CTRL, WAVE_3 & (uint8_t)~1);
  sid_put(V3_AD, 0x08);   // straight to full, then a slow decay
  sid_put(V3_SR, 0xF0);   // held while the gate is up, and gone the moment
  sid_put(V3_FREQ, (uint8_t)hz);
  sid_put(V3_FREQ + 1, (uint8_t)(hz >> 8));
  sid_put(V3_CTRL, WAVE_3);  // it is not
}

void engine_beep_step(void)
{
  if (!engine_beep_left || --engine_beep_left)
    return;

  // Voice 3 goes back exactly as engine_arm left it, gate edge and all, and
  // the interrupt starts writing its pitch again on the next tick.
  sid_put(V3_CTRL, WAVE_3 & (uint8_t)~1);
  sid_put(V3_AD, ENGINE_AD);
  sid_put(V3_SR, sustain[2]);
  sid_put(V3_CTRL, WAVE_3);
}
