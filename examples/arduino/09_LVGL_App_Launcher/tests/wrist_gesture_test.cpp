// Host test for the raise-to-view detector (not part of the sketch build):
//   c++ -std=c++17 -I../src/core tests/wrist_gesture_test.cpp src/core/wrist_gesture.cpp -o /tmp/wg && /tmp/wg
#include <math.h>
#include <stdio.h>

#include "../src/core/wrist_gesture.h"

static int s_failures = 0;

#define CHECK(cond)                                         \
  do {                                                      \
    if (!(cond)) {                                          \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_failures++;                                         \
    }                                                       \
  } while (0)

// Feeds a gravity vector tilted `deg` from face-up (0 = flat, 90 = screen vertical) for ms at 20 Hz.
// Returns how many raises fired.
static int feed(WristGesture *g, uint32_t *now, float deg, uint32_t ms, float extra_g = 0.0f) {
  int fired = 0;
  float rad = deg * 3.14159265f / 180.0f;
  for (uint32_t t = 0; t < ms; t += 50) {
    float ax = sinf(rad) * (1.0f + extra_g);
    float az = cosf(rad) * WRIST_FACE_UP_SIGN * (1.0f + extra_g);
    if (wrist_gesture_update(g, ax, 0.0f, az, *now)) {
      fired++;
    }
    *now += 50;
  }
  return fired;
}

static void test_raise_fires_once(void) {
  WristGesture g;
  wrist_gesture_reset(&g);
  uint32_t now = 1000;
  CHECK(feed(&g, &now, 90, 1000) == 0);  // arm hanging, screen sideways
  CHECK(feed(&g, &now, 30, 1000) == 1);  // raised and held: exactly one raise
  CHECK(feed(&g, &now, 30, 3000) == 0);  // keeps looking: no repeat
}

static void test_flat_on_desk_never_fires(void) {
  WristGesture g;
  wrist_gesture_reset(&g);
  uint32_t now = 1000;
  CHECK(feed(&g, &now, 0, 5000) == 0);
}

static void test_slow_turn_does_not_fire(void) {
  WristGesture g;
  wrist_gesture_reset(&g);
  uint32_t now = 1000;
  CHECK(feed(&g, &now, 90, 1000) == 0);
  CHECK(feed(&g, &now, 60, 3000) == 0);  // lingers in between longer than the raise window
  CHECK(feed(&g, &now, 20, 1000) == 0);
}

static void test_moving_arm_waits_until_steady(void) {
  WristGesture g;
  wrist_gesture_reset(&g);
  uint32_t now = 1000;
  CHECK(feed(&g, &now, 90, 1000) == 0);
  CHECK(feed(&g, &now, 30, 400, 0.6f) == 0);  // facing up but still swinging
  CHECK(feed(&g, &now, 30, 600) == 1);
}

static void test_rearms_after_lowering(void) {
  WristGesture g;
  wrist_gesture_reset(&g);
  uint32_t now = 1000;
  CHECK(feed(&g, &now, 90, 1000) == 0);
  CHECK(feed(&g, &now, 30, 1000) == 1);
  CHECK(feed(&g, &now, 90, 1000) == 0);
  CHECK(feed(&g, &now, 30, 1000) == 1);
}

static void test_upside_down_does_not_fire(void) {
  WristGesture g;
  wrist_gesture_reset(&g);
  uint32_t now = 1000;
  CHECK(feed(&g, &now, 90, 1000) == 0);
  CHECK(feed(&g, &now, 180, 2000) == 0);  // screen facing the floor
}

int main(void) {
  test_raise_fires_once();
  test_flat_on_desk_never_fires();
  test_slow_turn_does_not_fire();
  test_moving_arm_waits_until_steady();
  test_rearms_after_lowering();
  test_upside_down_does_not_fire();
  if (s_failures == 0) {
    printf("wrist_gesture: all tests passed\n");
  }
  return s_failures == 0 ? 0 : 1;
}
