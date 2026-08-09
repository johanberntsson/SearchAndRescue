#include <stdio.h>

#include "input.h"
#include "loader.h"
#include "panel.h"
#include "profile.h"
#include "vic4.h"
#include "voxel.h"

#define TURN_RATE   2   // angle units per frame
#define THRUST      96  // 8.8 map cells per frame
#define CLIMB_RATE  2   // height units per frame
#define GROUND_GAP  12  // never fly closer than this to the terrain

// How long the startup benchmark report stays up if nobody presses a key.
// Long enough to read or photograph, short enough that an unattended run
// still spends most of its time rendering.
#define REPORT_SECONDS 20

static void fly(camera *cam, uint8_t keys)
{
  int16_t speed = 0;
  uint8_t ground;

  if (keys & KEY_A)
    cam->angle -= TURN_RATE;
  if (keys & KEY_D)
    cam->angle += TURN_RATE;

  if (keys & KEY_W)
    speed = THRUST;
  if (keys & KEY_S)
    speed = -THRUST;

  if (speed) {
    // sin/cos are 8.8, speed is 8.8, and the position is 8.8: one shift of 8
    // brings the product back to the position's scale.
    cam->x += (int16_t)(((int32_t)voxel_sin(cam->angle + 64) * speed) >> 8);
    cam->y += (int16_t)(((int32_t)voxel_sin(cam->angle) * speed) >> 8);
  }

  if (keys & KEY_R)
    cam->height += CLIMB_RATE;
  if (keys & KEY_F)
    cam->height -= CLIMB_RATE;

  ground = voxel_ground(cam->x, cam->y);
  if (cam->height < (int16_t)ground + GROUND_GAP)
    cam->height = (int16_t)ground + GROUND_GAP;
}

int main(void)
{
  camera cam;
  uint8_t back = 1;
  uint16_t fps10 = 0;

  if (load_resources()) {
    printf("RESOURCE LOAD FAILED\n");
    return 1;
  }

  // The benchmarks and their report come first, while the Kernal's text
  // screen still exists to print on: vic4_init takes it away, and real
  // hardware has no -dumpmem to read the results out of afterwards.
  profile_init();
  profile_calibrate();
  profile_bench();
  profile_report(REPORT_SECONDS);

  vic4_init();
  vic4_set_palette(loaded_palette());
  voxel_init();
  panel_init();
  panel_message("SAR DRONE READY");

  cam.x = 128 << 8;  // middle of the map
  cam.y = 128 << 8;
  cam.angle = 0;
  cam.horizon = FB_HEIGHT * 2 / 5;
  cam.height = voxel_ground(cam.x, cam.y) + 60;

  for (;;) {
    uint32_t frame_start = profile_now32();
    uint16_t t0 = PROF_NOW();

    fly(&cam, input_read());
    PROF_ADD(P_OTHER, t0);

    voxel_render(vic4_base(back), &cam);

    t0 = PROF_NOW();
    panel_status(cam.height, cam.angle, fps10);
    vic4_show(back);
    back ^= 1;
    PROF_ADD(P_OTHER, t0);

    profile_count(C_FRAMES, 1);  // always: the FPS readout needs it
    fps10 = profile_fps10(frame_start - profile_now32());
  }
}
