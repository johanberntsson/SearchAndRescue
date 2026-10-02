#include <stdio.h>

#include "audio.h"
#include "drone.h"
#include "engine.h"
#include "input.h"
#include "lifeboat.h"
#include "loader.h"
#include "mission.h"
#include "music.h"
#include "panel.h"
#include "profile.h"
#include "screens.h"
#include "sprite.h"
#include "thermal.h"
#include "vic4.h"
#include "voxel.h"
#include "weather.h"

#define TURN_RATE   2   // angle units per frame
#define CLIMB_RATE  2   // height units per frame
#define GROUND_GAP  12  // never fly closer than this to the terrain
// Landed is sitting on the terrain following's floor, give or take two
// presses of climb: the drone cannot go lower than GROUND_GAP, so that is
// where holding F puts it.
#define LANDED      (GROUND_GAP + 2 * CLIMB_RATE)
// How near the fix a landing has to be to collect what is waiting there, in
// 8.8 cells either way. The pickup is a building several cells across, and
// any of its roof will do.
#define PICKUP_RANGE 0x300

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
// still spends most of its time rendering. `make REPORT=n` overrides it for a
// session at the real machine, where the report is the only way to read the
// attic RAM figures at all.
#ifndef REPORT_SECONDS
#define REPORT_SECONDS 20
#endif

// Frames a message from the game stays up before the standby line comes back.
// About five seconds at the frame rates this runs at.
#define MESSAGE_FRAMES 60

#define STANDBY "SAR DRONE READY"

// Filled with a canary by src/bank.s before anything uses the stack, and
// counted by cstack_measure(). **The game's high-water mark is 144 bytes** --
// measured at the boot, which is deeper than the flight's 120 -- against the
// toolchain's default of 4096. That is where the 3.5 KB in the Makefile's
// CSTACK_GAME came from. To re-take it after anything that adds call depth:
// call cstack_measure() and print cstack_unused.
uint16_t cstack_unused;
static uint8_t speed_mode = SPEED_DEFAULT;

// `M` mutes whatever the place you are in sounds like: the tune on a page,
// the motors in the air. Two settings rather than one, because wanting a
// quiet flight and wanting a quiet menu are different wants -- and both are
// kept for the whole session, so muting once is enough.
static uint8_t music_wanted = 1;
static uint8_t engine_wanted = 1;

// And `P` shows the frame rate, which starts hidden and is kept for the
// session the same way. Deliberately undocumented -- it is a thing to watch
// while working on the renderer, not an instrument on a drone -- so nothing
// on any screen mentions it.
static uint8_t fps_wanted;

// The mute key on every screen that is not a flight. The flight has its own,
// two lines of it, down in the loop.
static void music_key(keymask pressed)
{
  if (!(pressed & KEY_M))
    return;
  music_wanted = !music_wanted;
  music_set(music_wanted);
  screens_music(music_wanted);
}

// The battery, in 8.8 percent -- so the figure the panel shows is simply the
// high byte and there is no divide anywhere in the drain.
#define BATTERY_FULL (100 << 8)
#define BATTERY_LOW  (20 << 8)  // where the warning comes up, once

// What a frame costs, by speed mode. Sport is what a real drone's sport mode
// is: the props work harder whatever the sticks are doing, so it empties the
// pack in about a quarter of the time rather than only when you are moving.
// At around 11.6 fps these are roughly five minutes, four, and a minute and a
// quarter -- an unhurried search, or a fast one you have to finish.
static const uint16_t battery_drain[SPEED_MODES] = {7, 10, 28};

// And the thermal camera doubles it, whatever the speed: a second sensor is
// a second load on the pack. Without a cost it was simply left on for the
// whole of every flight, which made the optical camera pointless -- played on
// the machine, 2 Oct 2026. A shift, so it costs nothing to apply.
#define THERMAL_DRAIN_SHIFT 1

static uint16_t battery;
static uint8_t battery_warned;

// Which of the panel's three battery colours is showing. Kept so that the
// beep fires on the change and not on every frame that is still red.
static uint8_t battery_level;

// Take a frame out of the pack. Returns non-zero when it is flat.
static uint8_t battery_step(void)
{
  uint16_t drain = battery_drain[speed_mode];
  uint8_t was = (uint8_t)(battery >> 8);

  if (thermal_on())
    drain <<= THERMAL_DRAIN_SHIFT;
  if (battery <= drain) {
    battery = 0;
    panel_battery(0);
    return 1;
  }
  battery -= drain;
  // Only when the figure actually moves: at the fastest drain -- sport, with
  // the thermal camera on -- that is every fifth frame, and at the slowest
  // every thirty-seventh.
  if ((uint8_t)(battery >> 8) != was) {
    uint8_t level = panel_battery((uint8_t)(battery >> 8));

    // The panel says what colour it is and this decides what it sounds like:
    // once, on the way down, on each of the two thresholds.
    if (level > battery_level) {
      battery_level = level;
      engine_beep(level);
    }
  }
  return 0;
}

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

// What a wind of WIND_MAX does to a snowflake: 8.8 screen columns of sideways
// drift for every row it falls, at full strength and full across the view. At
// 70 a flake crossing the whole 152-row picture is carried about forty
// columns of the hundred and sixty, which is a plain slant without being a
// blizzard blowing horizontally. Rain does not use it -- see weather.h.
#define SNOW_DRIFT 7

// The direction it comes FROM, which is how a weather report, an airfield and
// a drone controller all name a wind -- so 270 is a westerly and it pushes you
// east.
static uint8_t wind_from;
static int16_t wind_speed;
static uint16_t wind_next;  // frames until it shifts again

// The gusts come out of weather_rnd, the same stream the rain's drops do --
// one generator for all the weather rather than two.
static void wind_start(void)
{
  wind_from = (uint8_t)weather_rnd();
  wind_speed = WIND_MIN + (int16_t)(weather_rnd() & (WIND_SPAN - 1));
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

  wind_from = (uint8_t)(wind_from + (weather_rnd() & 15) - 8);
  wind_speed += (weather_rnd() & 1) ? 1 : -1;
  if (wind_speed < WIND_MIN)
    wind_speed = WIND_MIN;
  else if (wind_speed > WIND_MAX)
    wind_speed = WIND_MAX;

  wind_next = WIND_GUST;
  panel_wind(wind_from, WIND_MPS(wind_speed));
}

// Fly one frame. Returns non-zero if the drone has hit the hillside, which
// only sport mode lets happen.
static uint8_t fly(camera *cam, keymask held)
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

  // What the props are being asked for, which is not the same as what the
  // drone is doing: the speed mode moves the note on its own, and the wind
  // below does not move it at all. The interrupt walks the note there; this
  // only says where there is.
  engine_throttle(speed_mode, speed != 0,
                  (held & KEY_R) ? 1 : ((held & KEY_F) ? -1 : 0));

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

    // And what the same wind does to falling snow, which is not what it does
    // to the drone. A flake is blown *across the picture*, so what matters is
    // the wind's component along the camera's lateral axis rather than its
    // whole vector -- and that axis is (-sin, cos) of the heading, exactly as
    // src/sprite.c projects a figure, so the component of a wind blowing
    // towards `to` is sin(to - angle). Fly into the wind and the snow comes
    // straight down; turn across it and it streaks off to one side.
    //
    // Worked out every frame rather than when the wind veers, because turning
    // the drone moves it just as much. Two table lookups and two multiplies.
    weather_drift(voxel_mul_shift8(
        voxel_sin((uint8_t)(to - cam->angle)),
        (int16_t)(wind_speed * SNOW_DRIFT)));
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
static uint8_t set_speed(keymask held)
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

// One scan of a page: a frame apart, so the joystick's button settles (see
// input_frame), and with the button standing in for SPACE, which is what "go
// on" is on every page. The stick needs nothing here -- it already arrives as
// W and S.
static keymask page_scan(void)
{
  keymask pressed;

  input_frame();
  // The title's drone flies on. Every page comes through here a frame at a
  // time, and on any page but the title its sprites are off, so this moves
  // something nobody can see -- cheaper than every page saying which it is.
  drone_tick(1);
  input_scan(0, &pressed);
  if (pressed & KEY_FIRE)
    pressed |= KEY_SPACE;
  music_key(pressed);
  return pressed;
}

// Sit on a finished screen until the pilot presses one of `keys`, and say
// which it was.
static keymask wait_for_key(keymask keys)
{
  keymask pressed;

  input_flush();
  do {
    pressed = page_scan();
  } while (!(pressed & keys));
  return pressed & keys;
}

static void wait_for_space(void)
{
  wait_for_key(KEY_SPACE);
}

// The controls page over a flight, which is the pause. Nothing in the flight
// moves while it is up -- the loop is simply not running, so the battery, the
// wind and the clock all stop with it -- and the motors go quiet, since the
// page is not the air. Coming back puts the view and the panel back as they
// were; neither was redrawn, only covered.
//
// Its own scan rather than page_scan, which would take M as the menus' mute
// and start the tune over a paused flight.
static void pause_flight(uint8_t mission_no)
{
  keymask pressed;

  engine_set(0);
  screens_controls(mission_no, 1);
  input_flush();
  do {
    input_frame();
    input_scan(0, &pressed);
  } while (!(pressed & (KEY_SPACE | KEY_FIRE | KEY_HELP)));

  vic4_view_mode();
  panel_restore();
  engine_set(engine_wanted);
  input_flush();
}

// The mission list, until one is chosen or the pilot backs out to the title.
// Returns which mission to brief, or mission_count() for "none of them".
static uint8_t choose_mission(uint8_t selected)
{
  screens_missions(selected);
  input_flush();

  for (;;) {
    keymask pressed = page_scan();
    uint8_t moved = selected;

    if (pressed & KEY_SPACE)
      return selected;
    if (pressed & KEY_STOP)
      return mission_count();
    if ((pressed & KEY_W) && selected)
      moved = (uint8_t)(selected - 1);
    if ((pressed & KEY_S) && selected + 1 < mission_count())
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
  const keymask action = mission_action_key(m);
  camera cam;
  uint8_t back = 1;
  uint16_t fps10 = 0;
  uint16_t message_left = 0;
  uint32_t launched;
  // Whether the cargo is aboard. It always was, until a mission could leave
  // it waiting at the fix to be collected.
  uint8_t loaded = !m->pickup;

  // The mission's own map: the renderer's plane tables, the palette its
  // climate wants and the panel's overview, all from attic RAM and none of it
  // reloaded from the disk. This is the whole of what flying somewhere else
  // costs.
  map_use(m->map);

  sprite_select(m->figure);
  {
    uint16_t x = FIX_TO_X(m->found_lon), y = FIX_TO_Y(m->found_lat);

    // Into the map before the figure is stood on it, which reads the ground.
    // The flight's caller sinks it again, whichever way the flight ends.
    if (m->lifeboat)
      lifeboat_launch(&x, &y);
    sprite_place(x, y);
  }

  vic4_view_mode();
  panel_init();
  panel_message(STANDBY);
  message_left = MESSAGE_FRAMES;  // it fades to the fix, like any other
  panel_speed(speed_mode);
  panel_cargo(loaded ? mission_cargo_name(m) : "EMPTY");
  // Seeded before anything asks for a random number, and the weather set
  // before the wind because arming the rain scatters its first drops.
  weather_seed((uint16_t)profile_now32());
  weather_set(m->weather);
  // Every flight starts on the optical camera, whatever the last one ended
  // on: thermal is a thing the pilot reaches for. After the weather, because
  // stowing the camera hands the sky back to it. A figure the ground is over
  // is therefore not on the screen at all until the pilot reaches -- and
  // sprite_show is what the report button ends up asking.
  thermal_set(0);
  sprite_show(m->hidden != HIDDEN_THERMAL);

  // Both after panel_init, which would otherwise blank the readouts they set.
  battery = BATTERY_FULL;
  battery_warned = 0;
  battery_level = BATTERY_OK;
  panel_battery(BATTERY_FULL >> 8);
  panel_fps(fps_wanted);
  wind_start();

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
    keymask held, pressed;
    uint8_t hit, flat;

    input_scan(&held, &pressed);

    // HELP or F1: the controls, and the flight stands still under them. The
    // time spent there is taken off the clock, and the frame is started over
    // so the frame rate does not count it either.
    if (pressed & KEY_HELP) {
      uint32_t paused = profile_now32();

      pause_flight(mission_no);
      launched -= paused - profile_now32();  // the clock counts down
      continue;
    }

    // The joystick's button is whichever of SPACE and RETURN this mission
    // wants, so a stick can fly any of them -- and it can never be the other
    // one, the cargo bay's door on a camera mission.
    if (pressed & KEY_FIRE)
      pressed |= action;
    engine_beep_step();
    wind_drift();
    flat = battery_step();
    hit = fly(&cam, held);
    // Down on the roof at the fix, with the bay still empty: take it on.
    if (!loaded
        && cam.height <= (int16_t)voxel_ground(cam.x, cam.y) + LANDED) {
      int16_t dx = (int16_t)(cam.x - FIX_TO_X(m->lon));
      int16_t dy = (int16_t)(cam.y - FIX_TO_Y(m->lat));

      if (dx < PICKUP_RANGE && dx > -PICKUP_RANGE
          && dy < PICKUP_RANGE && dy > -PICKUP_RANGE) {
        loaded = 1;
        panel_cargo(m->cargo);
        panel_message("CARGO LOADED");
        message_left = MESSAGE_FRAMES;
      }
    }
    if (set_speed(held)) {
      panel_message("SPORT: NO TERRAIN FOLLOW");
      message_left = MESSAGE_FRAMES;
    }
    // The same key as the menus', on the other of the two settings. The panel
    // says which, since a flight has no room for a line about it and nothing
    // else would tell the pilot the key had been noticed.
    // P, and nothing anywhere says so: the frame rate is a thing to look at
    // while working on the renderer, not an instrument on a drone. Kept for
    // the session like the mute, and off when the game starts.
    if (pressed & KEY_P) {
      fps_wanted = !fps_wanted;
      panel_fps(fps_wanted);
    }
    if (pressed & KEY_M) {
      engine_wanted = !engine_wanted;
      engine_set(engine_wanted);
      panel_message(engine_wanted ? "ENGINE SOUND ON" : "ENGINE SOUND OFF");
      message_left = MESSAGE_FRAMES;
    }
    // The second camera. There is no readout for it and it needs none -- the
    // whole picture goes cold, which no instrument could say better -- so
    // this is a message like the mute's and nothing more. A figure the ground
    // is over comes and goes with it.
    if (pressed & KEY_T) {
      thermal_set(!thermal_on());
      if (m->hidden == HIDDEN_THERMAL)
        sprite_show(thermal_on());
      panel_message(thermal_on() ? "THERMAL CAMERA ON"
                                 : "THERMAL CAMERA OFF");
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

    // The alert is over: the two top boxes go back to the fix and the
    // battery, which is what panel_message(0) means now that a message
    // covers them rather than having a row of its own.
    if (message_left && !--message_left)
      panel_message(0);

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
    if (flat) {
      *seconds = elapsed(launched);
      return FLIGHT_FLAT;
    }
    // Once, on the way past: a warning that repeated every frame below a
    // fifth would sit on the message line for the rest of the flight.
    if (!battery_warned && battery <= BATTERY_LOW) {
      battery_warned = 1;
      panel_message("BATTERY LOW -- COME HOME");
      message_left = MESSAGE_FRAMES;
    }
    if (pressed & KEY_STOP) {
      *seconds = elapsed(launched);
      return FLIGHT_ABORTED;
    }

    // The mission's own button: the camera's shutter, or the cargo release.
    // sprite_reportable answers for the frame just drawn, which is why this
    // comes after the render rather than with the rest of the input.
    // A collected cargo can be landed with as well as dropped: setting down
    // beside the figure hands it over. sprite_in_range is the frame's own.
    if (m->pickup && loaded && sprite_in_range()
        && cam.height <= (int16_t)voxel_ground(cam.x, cam.y) + LANDED) {
      *seconds = elapsed(launched);
      return FLIGHT_DONE;
    }
    if ((pressed & action) && !loaded) {
      // Nothing in the bay to drop yet, and opening it costs nothing.
      panel_message("COLLECT THE CARGO FIRST");
      message_left = MESSAGE_FRAMES;
    } else if (pressed & action) {
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
  // FLYNOW=n launches straight into mission n - 1, so FLYNOW=2 is the way to
  // see the second mission's map without a keyboard. See the note below.
  uint8_t mission_no = FLYNOW ? (uint8_t)(FLYNOW - 1) : 0;

  // Loading comes first, and on the ROM's own text screen. Both halves of
  // that are forced:
  //
  //   - profile_init takes CIA2's two timers over as its clock, and the
  //     Kernal needs them to talk to a disk. Anything read after it fails to
  //     open at all.
  //   - vic4_init leaves the Kernal unable to open a file either.
  //
  // So the only place a resource can be read is here, before both -- on the
  // ROM's screen, dressed up to look like the title screen that follows it.
  screens_boot();

  // The interrupt goes in now and the tune does not start until the title.
  // **The load is silent on purpose**: the Kernal's disk routines hold
  // interrupts off while they read, and on a real MEGA65 that chopped the
  // tune into pieces -- heard on 1 Oct 2026 -- where xemu played it cleanly.
  // Nothing of ours masks them, so the fix would be reading the disk without
  // the Kernal. The same interrupt carries the engine note later.
  audio_begin();

  if (load_resources(screens_loading)) {
    screens_load_failed(loader_error(), loader_error_file());
    for (;;)
      ;
  }
  screens_loaded();

  // The benchmarks while the text screen is still up: vic4_init takes it away,
  // and real hardware has no -dumpmem to read the results out of afterwards.
  profile_init();
  profile_calibrate();
  profile_bench();

  // The report is the one thing left that prints, and printing scribbles over
  // a screen that is now worth looking at -- so it happens only when somebody
  // has asked to read it. The results are in memory either way, which is where
  // tools/profread.py takes them from.
#if REPORT_SECONDS
  screens_boot_restore();
  profile_report(REPORT_SECONDS);
#endif

  vic4_init();
  voxel_init();
  // The first map, so the menus have a palette and the panel an overview.
  // A flight calls this again with its own mission's map: see map_use().
  map_use(0);

  // FLYNOW=1 launches straight into the flight. It exists for the headless
  // profiling run, which has no way to press a key and would otherwise dump a
  // memory image with no frames rendered in it. FLYNOW=2 does the same for
  // mission two, which is the only way to see the second map from a headless
  // run -- and therefore the way the several-maps-on-one-disk arrangement is
  // checked at all.
#if !FLYNOW
  music_set(music_wanted);
  screens_title();
  wait_for_space();
#endif

  for (;;) {
    uint16_t seconds;
    flight_outcome how;

#if !FLYNOW
    // Every page is a musical one. This is where the tune picks up again
    // after a flight -- coming off the debrief it is already playing, so
    // saying so once more costs nothing and covers the first time round.
    music_set(music_wanted);
    mission_no = choose_mission(mission_no);
    if (mission_no >= mission_count()) {  // backed out of the list
      screens_title();
      wait_for_space();
      mission_no = 0;
      continue;
    }

    // RUN/STOP reads the same on the briefing as it does in the air: this is
    // not the job, take me back. HELP or F1 is the controls page, and back.
    {
      keymask key;

      do {
        screens_briefing(mission_no);
        key = wait_for_key(KEY_SPACE | KEY_STOP | KEY_HELP);
        if (key & KEY_HELP) {
          screens_controls(mission_no, 0);
          wait_for_key(KEY_SPACE | KEY_HELP);
        }
      } while (key & KEY_HELP);
      if (key & KEY_STOP)
        continue;
    }
#endif

    // **The flight is the one quiet place.** Every page has the tune under
    // it, including the briefing and the debrief; the air has the motors and
    // the wind and nothing else. The tune comes back for the debrief, which
    // is a page like any other.
    //
    // The motors run for exactly as long as the drone is up, and only if the
    // pilot wants to hear them.
    music_set(0);
    engine_start(engine_wanted);
    how = flight(mission_no, &seconds);
    lifeboat_sink();  // the island is The Lost Hiker's too
    engine_set(0);
    music_set(music_wanted);

    // Recorded for the list, however many times it has been flown before.
    if (how == FLIGHT_DONE)
      missions_cleared |= (uint8_t)(1 << mission_no);

    screens_debrief(mission_no, how, seconds);
    wait_for_space();
#if !FLYNOW
    // The last one cleared: the win page, then the whole campaign again from
    // nothing -- the game goes round for ever, and the record goes with it.
    // Straight on to the list from there: the win page already is the title,
    // logo and drone and all, and showing the title after it is the same
    // page twice.
    if (missions_cleared == MISSIONS_ALL) {
      screens_won();
      wait_for_space();
      missions_cleared = 0;
      mission_no = 0;
      continue;
    }
    // Back by way of the title rather than straight to the list: a flight is
    // over, and the game goes round again from its front page. The list still
    // opens on the mission just flown.
    screens_title();
    wait_for_space();
#endif
  }
}
