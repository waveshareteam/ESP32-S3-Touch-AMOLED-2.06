#pragma once

#include "../board/board.h"
#include "../core/settings_store.h"

struct PowerPolicy {
  uint8_t brightness;         // level actually sent to the panel
  uint32_t sleep_timeout_ms;  // 0 = never sleep
};

// Decides the effective brightness / auto-sleep from user settings and battery state.
PowerPolicy power_policy_compute(const AppSettings &settings, const BatteryInfo &battery);
