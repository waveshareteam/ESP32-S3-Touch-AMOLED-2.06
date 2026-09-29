/*
 * 09_LVGL_App_Launcher
 *
 * A small watch-style launcher inspired by the ESP-Brookesia phone AppLauncher
 * (examples/esp-idf/03_esp-brookesia), rebuilt on plain LVGL 9 + Arduino:
 *   page 1: watch face (RTC/NTP time, battery, Wi-Fi, location)
 *   page 2: app icons -> AI Voice (placeholder), Settings (incl. OTA update)
 *
 * Source layout (one folder per module, see README.md):
 *   src/core  src/board  src/audio  src/protocol  src/server  src/apps  src/components  src/launcher
 * partitions.csv gives every module its own data partition plus A/B app slots for OTA.
 */
#include "src/core/system.h"

void setup() {
  system_setup();
}

void loop() {
  system_loop();
}
