#pragma once

#include <Arduino.h>

// Board module: display, touch, PMU, RTC and IMU. Audio lives in src/audio.

struct BatteryInfo {
  bool present;
  int percent;        // -1 when no battery is detected
  uint16_t voltage_mv;
  bool charging;
  bool vbus_in;
};

bool board_display_begin(void);
uint16_t board_display_width(void);
uint16_t board_display_height(void);
void board_display_push(const uint16_t *frame);
void board_display_set_brightness(uint8_t level);
void board_display_sleep(void);
void board_display_wake(void);
bool board_display_is_sleeping(void);

bool board_touch_begin(void);
// Returns true while the panel reports a touch; fills x/y.
bool board_touch_read(int32_t *x, int32_t *y);

bool board_pmu_begin(void);
BatteryInfo board_battery(void);
// Returns true once per short press of the PWR key.
bool board_power_key_pressed(void);

// QMI8658 accelerometer only (gyro off), low-power ~21 Hz
bool board_imu_begin(void);
// Latest acceleration in g (sensor axes). Returns false when the IMU is missing or has no new data.
bool board_imu_read_accel(float *x, float *y, float *z);

bool board_rtc_begin(void);
// RTC stores UTC. Copies RTC -> system clock when the RTC holds a sane date.
bool board_time_load_from_rtc(void);
void board_time_save_to_rtc(void);
void board_time_apply_tz(const char *posix_tz);
