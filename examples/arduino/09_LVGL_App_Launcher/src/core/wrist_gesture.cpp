#include "wrist_gesture.h"

#include <math.h>

// Tuning (gravity component along the screen normal, 1.0 = screen flat and facing up)
#define LOWERED_Z 0.35f        // below: screen not facing the user (~70 deg or more from flat)
#define VIEW_Z 0.60f           // at or above: screen facing the user (~53 deg or less from flat)
#define STEADY_TOLERANCE_G 0.35f  // |a| further than this from 1 g = arm still moving
#define HOLD_MS 250            // facing-up pose must be steady this long
#define RAISE_WINDOW_MS 2000   // ... and reached within this time after the wrist was lowered
#define FILTER_ALPHA 0.35f     // gravity low-pass weight for a new sample (~20 Hz input)

void wrist_gesture_reset(WristGesture *g) {
  g->gx = 0.0f;
  g->gy = 0.0f;
  g->gz = 0.0f;
  g->have_sample = false;
  g->armed = true;
  g->last_lowered_ms = 0;
  g->has_lowered = false;
  g->view_since_ms = 0;
  g->in_view = false;
}

bool wrist_gesture_update(WristGesture *g, float ax, float ay, float az, uint32_t now_ms) {
  if (!g->have_sample) {
    g->gx = ax;
    g->gy = ay;
    g->gz = az;
    g->have_sample = true;
  } else {
    g->gx += FILTER_ALPHA * (ax - g->gx);
    g->gy += FILTER_ALPHA * (ay - g->gy);
    g->gz += FILTER_ALPHA * (az - g->gz);
  }

  float z = g->gz * WRIST_FACE_UP_SIGN;
  float magnitude = sqrtf(ax * ax + ay * ay + az * az);
  bool steady = fabsf(magnitude - 1.0f) <= STEADY_TOLERANCE_G;

  if (z < LOWERED_Z) {
    g->last_lowered_ms = now_ms;
    g->has_lowered = true;
    g->in_view = false;
    g->armed = true;
    return false;
  }
  if (z < VIEW_Z) {
    g->in_view = false;  // in between: neither lowered nor viewing
    return false;
  }

  if (!g->in_view || !steady) {
    g->in_view = true;
    g->view_since_ms = now_ms;  // (re)start the hold once the arm settles
    return false;
  }

  if (g->armed && g->has_lowered && now_ms - g->view_since_ms >= HOLD_MS &&
      now_ms - g->last_lowered_ms <= RAISE_WINDOW_MS) {
    g->armed = false;
    return true;
  }
  return false;
}
