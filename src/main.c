#include <stdio.h>

#include "hud.h"
#include "input.h"
#include "loader.h"
#include "profile.h"
#include "vic4.h"
#include "voxel.h"

#define TURN_RATE   2   // angle units per frame
#define THRUST      96  // 8.8 map cells per frame
#define CLIMB_RATE  2   // height units per frame
#define GROUND_GAP  12  // never fly closer than this to the terrain

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

  vic4_init();
  vic4_set_palette(loaded_palette());
  voxel_init();

  cam.x = 128 << 8;  // middle of the map
  cam.y = 128 << 8;
  cam.angle = 0;
  cam.horizon = FB_HEIGHT * 2 / 5;
  cam.height = voxel_ground(cam.x, cam.y) + 60;

  profile_init();
  profile_calibrate();
  profile_bench();

  for (;;) {
    uint32_t frame_start = profile_now32();
    uint16_t t0 = PROF_NOW();

    fly(&cam, input_read());
    PROF_ADD(P_OTHER, t0);

    voxel_render(vic4_base(back), &cam);

    t0 = PROF_NOW();
    hud_fps(vic4_base(back), fps10);
    vic4_show(back);
    back ^= 1;
    PROF_ADD(P_OTHER, t0);

    profile_count(C_FRAMES, 1);  // always: the FPS readout needs it
    fps10 = profile_fps10(frame_start - profile_now32());
  }
}
