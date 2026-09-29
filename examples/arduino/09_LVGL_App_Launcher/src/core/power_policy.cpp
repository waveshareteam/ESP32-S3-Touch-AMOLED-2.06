#include "power_policy.h"

// Low Battery Mode only kicks in on battery power and when the level is low.
// Each tier caps the user's values; it never makes the screen brighter or stay on longer.
#define LOW_BATTERY_PERCENT 40
#define CRITICAL_BATTERY_PERCENT 20

#define LOW_BATTERY_MAX_BRIGHTNESS 100  // ~40 %
#define CRITICAL_BATTERY_MAX_BRIGHTNESS 40  // ~15 %
#define LOW_BATTERY_MAX_SLEEP_MS (30 * 1000)
#define CRITICAL_BATTERY_MAX_SLEEP_MS (15 * 1000)

static void cap_policy(PowerPolicy &policy, uint8_t max_brightness, uint32_t max_sleep_ms) {
  if (policy.brightness > max_brightness) {
    policy.brightness = max_brightness;
  }
  // 0 means "never sleep", which is longer than any cap
  if (policy.sleep_timeout_ms == 0 || policy.sleep_timeout_ms > max_sleep_ms) {
    policy.sleep_timeout_ms = max_sleep_ms;
  }
}

PowerPolicy power_policy_compute(const AppSettings &settings, const BatteryInfo &battery) {
  PowerPolicy policy;
  policy.brightness = settings.brightness;
  policy.sleep_timeout_ms = settings_sleep_timeout_ms(settings.sleep_mode);

  bool on_battery = battery.present && battery.percent >= 0 && !battery.charging && !battery.vbus_in;
  if (!settings.low_battery_mode || !on_battery) {
    return policy;
  }

  if (battery.percent <= CRITICAL_BATTERY_PERCENT) {
    cap_policy(policy, CRITICAL_BATTERY_MAX_BRIGHTNESS, CRITICAL_BATTERY_MAX_SLEEP_MS);
  } else if (battery.percent <= LOW_BATTERY_PERCENT) {
    cap_policy(policy, LOW_BATTERY_MAX_BRIGHTNESS, LOW_BATTERY_MAX_SLEEP_MS);
  }
  return policy;
}
