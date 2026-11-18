#pragma once
// app_sea — LVGL side of the sea scene: five lanes of waves rolling right to left over the whole screen and a surfer the dial steers between them
// (private to the app component). LVGL lock held by the caller, as for app_screen. Motion maths
// lives in sea_model.c.
#include "lvgl.h"

// Create the sea and surfer on `parent` and start the animation timer. Call after the ring exists
// so the scene draws on top of it.
void app_sea_build(lv_obj_t *parent);

// One knob detent: +1 moves the surfer to a nearer lane, -1 to a farther one.
void app_sea_nudge(int dir);

// Put the surfer back on the farthest lane (boot state; used by the test reset hook).
void app_sea_reset(void);
