#include <Preferences.h>
#include "../core/settings_store.h"

AppSettings g_settings;

static const char *NVS_NAMESPACE = "launcher";

static void copy_str(char *dst, size_t size, const String &src) {
  strlcpy(dst, src.c_str(), size);
}

void settings_load(void) {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true);

  g_settings.brightness = prefs.getUChar("bright", 180);
  g_settings.dark_theme = prefs.getBool("dark", true);
  g_settings.sleep_mode = prefs.getUChar("sleep", SLEEP_30_SEC);
  g_settings.volume = prefs.getUChar("vol", 70);
  copy_str(g_settings.tz_name, sizeof(g_settings.tz_name), prefs.getString("tz", "Asia/Bangkok"));
  copy_str(g_settings.location, sizeof(g_settings.location), prefs.getString("loc", "Bangkok"));
  g_settings.low_battery_mode = prefs.getBool("lowbat", false);
  g_settings.watch_style = prefs.getUChar("wstyle", 0);
  copy_str(g_settings.wifi_ssid, sizeof(g_settings.wifi_ssid), prefs.getString("ssid", ""));
  copy_str(g_settings.wifi_pass, sizeof(g_settings.wifi_pass), prefs.getString("pass", ""));

  prefs.end();

  if (g_settings.brightness < 5) {
    g_settings.brightness = 5;
  }
  if (g_settings.sleep_mode > SLEEP_NEVER) {
    g_settings.sleep_mode = SLEEP_30_SEC;
  }
  if (g_settings.volume > 100) {
    g_settings.volume = 100;
  }
}

void settings_save(void) {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);

  prefs.putUChar("bright", g_settings.brightness);
  prefs.putBool("dark", g_settings.dark_theme);
  prefs.putUChar("sleep", g_settings.sleep_mode);
  prefs.putUChar("vol", g_settings.volume);
  prefs.putString("tz", g_settings.tz_name);
  prefs.putString("loc", g_settings.location);
  prefs.putBool("lowbat", g_settings.low_battery_mode);
  prefs.putUChar("wstyle", g_settings.watch_style);
  prefs.putString("ssid", g_settings.wifi_ssid);
  prefs.putString("pass", g_settings.wifi_pass);

  prefs.end();
}

uint32_t settings_sleep_timeout_ms(uint8_t sleep_mode) {
  switch (sleep_mode) {
    case SLEEP_30_SEC:
      return 30 * 1000;
    case SLEEP_1_MIN:
      return 60 * 1000;
    default:
      return 0;
  }
}
