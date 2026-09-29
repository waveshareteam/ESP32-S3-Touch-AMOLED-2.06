#pragma once

#include <Arduino.h>

enum SleepMode : uint8_t {
  SLEEP_30_SEC = 0,
  SLEEP_1_MIN = 1,
  SLEEP_NEVER = 2,
};

struct AppSettings {
  uint8_t brightness;      // 5 - 255, user-selected level
  bool dark_theme;
  uint8_t sleep_mode;      // SleepMode
  uint8_t volume;          // 0 - 100
  char tz_name[40];        // IANA name, e.g. "Asia/Bangkok"
  char location[32];       // free text shown on the watch face
  bool low_battery_mode;
  uint8_t watch_style;     // watch face phrase set, cycled by tapping the watch face
  char wifi_ssid[33];
  char wifi_pass[65];      // stored in NVS as plain text (NVS encryption is not enabled)
};

extern AppSettings g_settings;

void settings_load(void);
void settings_save(void);

// 0 means "never sleep"
uint32_t settings_sleep_timeout_ms(uint8_t sleep_mode);
