#pragma once

#include <stdint.h>

// Raise-to-view detector fed with accelerometer samples (in g, sensor axes).
// Plain C++ with no Arduino dependency so it can be unit-tested on the host.
//
// A raise is reported when the screen turns from "not facing the user" (arm hanging, screen
// sideways) to "facing up", and then holds that pose steadily. Lying still face-up on a desk
// never fires because there is no transition from the lowered pose.

// Which sign of the sensor Z axis points out of the screen. Flip if raises are never detected
// (check the "IMU accel" log: a watch lying face-up on a desk should show z close to this sign).
#ifndef WRIST_FACE_UP_SIGN
#define WRIST_FACE_UP_SIGN 1.0f
#endif

struct WristGesture {
  float gx, gy, gz;           // low-pass filtered gravity
  bool have_sample;
  bool armed;                 // false after a raise until the wrist is lowered again
  uint32_t last_lowered_ms;   // last time the screen was clearly not facing up
  bool has_lowered;
  uint32_t view_since_ms;     // start of the current facing-up period
  bool in_view;
};

void wrist_gesture_reset(WristGesture *g);

// Feed one sample. Returns true once per raise.
bool wrist_gesture_update(WristGesture *g, float ax, float ay, float az, uint32_t now_ms);
