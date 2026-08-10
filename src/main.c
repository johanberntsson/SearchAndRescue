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

// How long the startup benchmark report stays up if nobody presses a key.
// Long enough to read or photograph, short enough that an unattended run
// still spends most of its time rendering.
#define REPORT_SECONDS 20

// Frames a message from the game stays up before the standby line comes back.
// About five seconds at the frame rates this runs at.
#define MESSAGE_FRAMES 60

#define STANDBY "SAR DRONE READY"

static uint8_t speed_mode = SPEED_DEFAULT;

static void fly(camera *cam, uint16_t held)
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

  ground = voxel_ground(cam->x, cam->y);
  if (cam->height < (int16_t)ground + GROUND_GAP)
    cam->height = (int16_t)ground + GROUND_GAP;
}

// 1, 2 and 3 pick the speed limiter. Held rather than edge-triggered: there
// is nothing to repeat, so pressing it twice is the same as pressing it once.
static void set_speed(uint16_t held)
{
  uint8_t mode = speed_mode;

  if (held & KEY_1)
    mode = 0;
  if (held & KEY_2)
    mode = 1;
  if (held & KEY_3)
    mode = 2;

  if (mode != speed_mode) {
    speed_mode = mode;
    panel_speed(mode);
  }
}

// Sit on a finished screen until the pilot presses space.
static void wait_for_space(void)
{
  uint16_t pressed;

  input_flush();
  do {
    input_scan(0, &pressed);
  } while (!(pressed & KEY_SPACE));
}

// One flight. Returns when the pilot has filed a report on the survivor.
static void flight(uint8_t mission_no, uint16_t *seconds)
{
  const mission *m = &missions[mission_no];
  camera cam;
  uint8_t back = 1;
  uint16_t fps10 = 0;
  uint16_t message_left = 0;
  uint32_t launched;

  sprite_place(FIX_TO_X(m->lon), FIX_TO_Y(m->lat));

  vic4_view_mode();
  panel_init();
  panel_message(STANDBY);
  panel_speed(speed_mode);

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

    input_scan(&held, &pressed);
    fly(&cam, held);
    set_speed(held);
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

    // The report button. sprite_reportable answers for the frame just drawn,
    // which is why this comes after the render rather than with the rest of
    // the input.
    if (pressed & KEY_SPACE) {
      if (sprite_reportable()) {
        uint32_t ticks = launched - profile_now32();

        *seconds = (uint16_t)(ticks / profile_ticks_per_second());
        return;
      }
      panel_message("NO SURVIVOR IN SIGHT");
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

#if !FLYNOW
    screens_missions(mission_no);
    wait_for_space();

    screens_briefing(mission_no);
    wait_for_space();
#endif

    flight(mission_no, &seconds);

    screens_accomplished(mission_no, seconds);
    wait_for_space();
  }
}
