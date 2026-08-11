#include <stdio.h>

#include "input.h"
#include "loader.h"
#include "mission.h"
#include "panel.h"
#include "profile.h"
#include "screens.h"
#include "sprite.h"
#include "vic4.h"
#include "voxel.h"

#define TURN_RATE   2   // angle units per frame
#define CLIMB_RATE  2   // height units per frame
#define GROUND_GAP  12  // never fly closer than this to the terrain

// The gimbal, in screen rows of horizon. Down means the horizon climbs out of
// the top of the picture, so tilting down lowers the number.
#define TILT_RATE   2
#define TILT_MIN    (-40)
#define TILT_MAX    140
#define TILT_LEVEL  (FB_HEIGHT * 2 / 5)

// 8.8 map cells a frame at full stick: cinematic, normal, sport, which is
// what the speed switch on a real drone offers.
#define SPEED_MODES 3
static const int16_t speed_limit[SPEED_MODES] = {40, 96, 176};
#define SPEED_DEFAULT 1
// Sport is the mode a real drone turns its obstacle sensors off in, and this
// one does the same: the terrain following below only holds in the other two.
#define SPEED_SPORT 2

// How long the startup benchmark report stays up if nobody presses a key.
// Long enough to read or photograph, short enough that an unattended run
// still spends most of its time rendering.
#define REPORT_SECONDS 20

// Frames a message from the game stays up before the standby line comes back.
// About five seconds at the frame rates this runs at.
#define MESSAGE_FRAMES 60

#define STANDBY "SAR DRONE READY"

static uint8_t speed_mode = SPEED_DEFAULT;

// The wind: the one thing in the flight model that is not the pilot's. It
// blows the drone about whatever it is doing, including a hover, and it veers
// every few seconds so that trimming for it once is not enough.
//
// Strength is 8.8 map cells a frame, the same units as speed_limit above, so
// the range here is between a tenth and a third of cinematic speed -- enough
// to carry you off a mark while you line up a shot, not enough to fight.
#define WIND_MIN   3
#define WIND_SPAN  8  // a power of two, so picking one is a mask rather than
                      // a call into the 16-bit modulo routine
#define WIND_MAX  (WIND_MIN + WIND_SPAN - 1)
#define WIND_GUST 96  // frames between shifts, about eight seconds

// Cells a frame as the panel reports it. Nothing in this world is to scale --
// sport is 176 cells a frame, which at a hundred metres a cell would be
// several hundred metres a second -- so rather than convert honestly and
// print a hurricane, this is simply a scale on which a wind reads like one:
// WIND_MIN..WIND_MAX becomes 1..5 m/s, a light air to a gentle breeze.
#define WIND_MPS(s) ((uint8_t)((s) / 2))

// The direction it comes FROM, which is how a weather report, an airfield and
// a drone controller all name a wind -- so 270 is a westerly and it pushes you
// east.
static uint8_t wind_from;
static int16_t wind_speed;
static uint16_t wind_next;  // frames until it shifts again

// xorshift16, seeded off the profiler's free-running clock when a flight
// launches so that no two get the same weather. The state must never be zero,
// which is the one value this generator cannot leave.
static uint16_t rng_state = 1;

static uint16_t rnd(void)
{
  uint16_t x = rng_state;

  x ^= (uint16_t)(x << 7);
  x ^= x >> 9;
  x ^= (uint16_t)(x << 8);
  rng_state = x;
  return x;
}

static void wind_start(void)
{
  rng_state = (uint16_t)profile_now32() | 1;
  wind_from = (uint8_t)rnd();
  wind_speed = WIND_MIN + (int16_t)(rnd() & (WIND_SPAN - 1));
  wind_next = WIND_GUST;
  panel_wind(wind_from, WIND_MPS(wind_speed));
}

// A small shift every few seconds: about eleven degrees of veer either way and
// a step of speed. Small on purpose -- the wind is meant to be something the
// pilot corrects for continuously, not an event that happens to them.
static void wind_drift(void)
{
  if (--wind_next)
    return;

  wind_from = (uint8_t)(wind_from + (rnd() & 15) - 8);
  wind_speed += (rnd() & 1) ? 1 : -1;
  if (wind_speed < WIND_MIN)
    wind_speed = WIND_MIN;
  else if (wind_speed > WIND_MAX)
    wind_speed = WIND_MAX;

  wind_next = WIND_GUST;
  panel_wind(wind_from, WIND_MPS(wind_speed));
}

// Fly one frame. Returns non-zero if the drone has hit the hillside, which
// only sport mode lets happen.
static uint8_t fly(camera *cam, uint16_t held)
{
  int16_t speed = 0;
  uint8_t ground;

  if (held & KEY_A)
    cam->angle -= TURN_RATE;
  if (held & KEY_D)
    cam->angle += TURN_RATE;

  if (held & KEY_W)
    speed = speed_limit[speed_mode];
  if (held & KEY_S)
    speed = (int16_t)-speed_limit[speed_mode];

  if (speed) {
    // sin/cos are 8.8, speed is 8.8, and the position is 8.8: one shift of 8
    // brings the product back to the position's scale.
    cam->x += (int16_t)(((int32_t)voxel_sin(cam->angle + 64) * speed) >> 8);
    cam->y += (int16_t)(((int32_t)voxel_sin(cam->angle) * speed) >> 8);
  }

  if (held & KEY_R)
    cam->height += CLIMB_RATE;
  if (held & KEY_F)
    cam->height -= CLIMB_RATE;

  // The renderer rebuilds its horizon table whenever this moves, so tilting
  // costs a frame's worth of table and nothing per pixel.
  if (held & KEY_Q)
    cam->horizon += TILT_RATE;
  if (held & KEY_E)
    cam->horizon -= TILT_RATE;
  if (cam->horizon > TILT_MAX)
    cam->horizon = TILT_MAX;
  if (cam->horizon < TILT_MIN)
    cam->horizon = TILT_MIN;

  // The wind, on top of wherever the pilot has got to, and before the ground
  // check so that being blown into a hillside counts exactly as flying into
  // one. It blows towards wind_from + 128, half a turn from the direction it
  // is named for.
  //
  // voxel_mul_shift8 rather than the C multiply the pilot's own motion uses,
  // because this happens every frame whether the drone is moving or not: 85
  // cycles on the hardware multiplier against the compiler's 2203.
  {
    uint8_t to = (uint8_t)(wind_from + 128);

    cam->x += voxel_mul_shift8(voxel_sin((uint8_t)(to + 64)), wind_speed);
    cam->y += voxel_mul_shift8(voxel_sin(to), wind_speed);
  }

  // The same test does both jobs: below the gap you are in the hill. In the
  // two slower modes the drone simply refuses to go there, which is what the
  // terrain following has always done; in sport there is nothing holding it
  // off and the flight is over.
  //
  // The camera is put back on top of the terrain either way, so the last
  // frame the pilot sees is the hillside they flew into rather than a picture
  // taken from inside it.
  ground = voxel_ground(cam->x, cam->y);
  if (cam->height < (int16_t)ground + GROUND_GAP) {
    cam->height = (int16_t)ground + GROUND_GAP;
    return speed_mode == SPEED_SPORT;
  }
  return 0;
}

// 1, 2 and 3 pick the speed limiter. Held rather than edge-triggered: there
// is nothing to repeat, so pressing it twice is the same as pressing it once.
// Returns non-zero when the pilot has just armed sport mode, which is worth
// saying out loud because it takes the terrain following away.
static uint8_t set_speed(uint16_t held)
{
  uint8_t mode = speed_mode;

  if (held & KEY_1)
    mode = 0;
  if (held & KEY_2)
    mode = 1;
  if (held & KEY_3)
    mode = SPEED_SPORT;

  if (mode == speed_mode)
    return 0;
  speed_mode = mode;
  panel_speed(mode);
  return mode == SPEED_SPORT;
}

// Seconds since a profiler timestamp. The subtraction is on a line of its own
// because Calypsi 5.18 emits a call to _FillZPQ -- a runtime helper that is in
// none of its libraries -- whenever a function call turns up inside a 32-bit
// expression.
static uint16_t elapsed(uint32_t since)
{
  uint32_t ticks = since - profile_now32();

  return (uint16_t)(ticks / profile_ticks_per_second());
}

// Sit on a finished screen until the pilot presses one of `keys`, and say
// which it was.
static uint16_t wait_for_key(uint16_t keys)
{
  uint16_t pressed;

  input_flush();
  do {
    input_scan(0, &pressed);
  } while (!(pressed & keys));
  return pressed & keys;
}

static void wait_for_space(void)
{
  wait_for_key(KEY_SPACE);
}

// The mission list, until one is chosen or the pilot backs out to the title.
// Returns which mission to brief, or MISSION_COUNT for "none of them".
static uint8_t choose_mission(uint8_t selected)
{
  screens_missions(selected);
  input_flush();

  for (;;) {
    uint16_t pressed;
    uint8_t moved = selected;

    input_scan(0, &pressed);
    if (pressed & KEY_SPACE)
      return selected;
    if (pressed & KEY_STOP)
      return MISSION_COUNT;
    if ((pressed & KEY_W) && selected)
      moved = (uint8_t)(selected - 1);
    if ((pressed & KEY_S) && selected + 1 < MISSION_COUNT)
      moved = (uint8_t)(selected + 1);

    // Redrawn only when it changes: the page is a rewrite of screen RAM, and
    // doing it every scan would flicker the highlight.
    if (moved != selected) {
      selected = moved;
      screens_missions(selected);
    }
  }
}

// One flight, and how it ended. `seconds` is filled in whichever way that is.
static flight_outcome flight(uint8_t mission_no, uint16_t *seconds)
{
  const mission *m = &missions[mission_no];
  const uint16_t action = mission_action_key(m);
  camera cam;
  uint8_t back = 1;
  uint16_t fps10 = 0;
  uint16_t message_left = 0;
  uint32_t launched;

  sprite_select(m->figure);
  sprite_place(FIX_TO_X(m->lon), FIX_TO_Y(m->lat));

  vic4_view_mode();
  panel_init();
  panel_message(STANDBY);
  panel_speed(speed_mode);
  panel_cargo(mission_cargo_name(m));
  wind_start();  // after panel_init, which would otherwise blank the readout

  cam.x = 128 << 8;  // middle of the map
  cam.y = 128 << 8;
  cam.angle = 0;
  cam.horizon = TILT_LEVEL;
  cam.height = voxel_ground(cam.x, cam.y) + 60;

  input_flush();
  launched = profile_now32();

  for (;;) {
    uint32_t frame_start = profile_now32();
    uint16_t t0 = PROF_NOW();
    uint16_t held, pressed;
    uint8_t hit;

    input_scan(&held, &pressed);
    wind_drift();
    hit = fly(&cam, held);
    if (set_speed(held)) {
      panel_message("SPORT: NO TERRAIN FOLLOWING");
      message_left = MESSAGE_FRAMES;
    }
    PROF_ADD(P_OTHER, t0);

    voxel_render(vic4_base(back), &cam);

    t0 = PROF_NOW();
    panel_status(cam.height, cam.angle, cam.x, cam.y, fps10);
    vic4_show(back);
    back ^= 1;
    PROF_ADD(P_OTHER, t0);

    profile_count(C_FRAMES, 1);  // always: the FPS readout needs it
    fps10 = profile_fps10(frame_start - profile_now32());

    if (message_left && !--message_left)
      panel_message(STANDBY);

    // Every way out of the loop reports the flight that actually happened, so
    // the debrief times an abandoned one too.
    //
    // The crash is tested down here rather than where `fly` reports it, so
    // that the frame showing what was flown into is on the screen before the
    // debrief replaces it.
    if (hit) {
      *seconds = elapsed(launched);
      return FLIGHT_CRASHED;
    }
    if (pressed & KEY_STOP) {
      *seconds = elapsed(launched);
      return FLIGHT_ABORTED;
    }

    // The mission's own button: the camera's shutter, or the cargo release.
    // sprite_reportable answers for the frame just drawn, which is why this
    // comes after the render rather than with the rest of the input.
    if (pressed & action) {
      if (m->cargo ? sprite_in_range() : sprite_reportable()) {
        *seconds = elapsed(launched);
        return FLIGHT_DONE;
      }
      // A photograph can be taken again; there is only one EpiPen, and it is
      // now lying wherever the drone was when the bay opened.
      if (m->cargo) {
        panel_cargo("EMPTY");
        *seconds = elapsed(launched);
        return FLIGHT_LOST;
      }
      panel_message("NO SURVIVOR IN SIGHT");
      message_left = MESSAGE_FRAMES;
    } else if (pressed & (KEY_SPACE | KEY_RETURN)) {
      // The other one of the two. Saying which key this mission wants beats
      // saying nothing at all.
      panel_message(m->cargo ? "RETURN RELEASES THE CARGO"
                             : "THE CARGO BAY IS EMPTY");
      message_left = MESSAGE_FRAMES;
    }
  }
}

int main(void)
{
  uint8_t mission_no = 0;

  // Loading comes first, and on the ROM's own text screen. Both halves of
  // that are forced:
  //
  //   - profile_init takes CIA2's two timers over as its clock, and the
  //     Kernal needs them to talk to a disk. Anything read after it fails to
  //     open at all.
  //   - vic4_init leaves the Kernal unable to open a file either.
  //
  // So the only place a resource can be read is here, before both, which is
  // why the loading bar is printed rather than drawn.
  screens_boot();
  if (load_resources(screens_loading)) {
    screens_load_failed(loader_error(), loader_error_file());
    for (;;)
      ;
  }

  // The benchmarks and their report while the text screen is still up:
  // vic4_init takes it away, and real hardware has no -dumpmem to read the
  // results out of afterwards.
  profile_init();
  profile_calibrate();
  profile_bench();
  profile_report(REPORT_SECONDS);

  vic4_init();
  vic4_set_palette(loaded_palette());
  voxel_init();
  // Once, before any crosshair has been drawn into the overview map: this is
  // the copy every later crosshair is lifted with.
  vic4_overview_ready();

  // FLYNOW=1 launches straight into the flight. It exists for the headless
  // profiling run, which has no way to press a key and would otherwise dump a
  // memory image with no frames rendered in it.
#if !FLYNOW
  screens_title();
  wait_for_space();
#endif

  for (;;) {
    uint16_t seconds;
    flight_outcome how;

#if !FLYNOW
    mission_no = choose_mission(mission_no);
    if (mission_no >= MISSION_COUNT) {  // backed out of the list
      screens_title();
      wait_for_space();
      mission_no = 0;
      continue;
    }

    screens_briefing(mission_no);
    // RUN/STOP reads the same on the briefing as it does in the air: this is
    // not the job, take me back.
    if (wait_for_key(KEY_SPACE | KEY_STOP) & KEY_STOP)
      continue;
#endif

    how = flight(mission_no, &seconds);

    screens_debrief(mission_no, how, seconds);
    wait_for_space();
  }
}
