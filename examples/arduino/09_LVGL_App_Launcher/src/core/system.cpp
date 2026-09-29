// Core module: boots every other module, owns the LVGL port and the main loop.
#include <Wire.h>
#include <Arduino.h>
#include "pin_config.h"
#include <lvgl.h>
#include "lv_conf.h"
#include "HWCDC.h"

#include "../board/board.h"
#include "../audio/audio.h"
#include "../protocol/net.h"
#include "../components/timezones.h"
#include "../components/ui_theme.h"
#include "../launcher/app_launcher.h"
#include "../apps/apps.h"
#include "settings_store.h"
#include "power_policy.h"
#include "modules.h"
#include "ota.h"
#include "wrist_gesture.h"
#include "system.h"

HWCDC USBSerial;

static lv_display_t *disp = nullptr;
static uint16_t *frame_buf = nullptr;

static PowerPolicy s_policy = { 180, 30000 };
static bool s_touch_wake_guard = false;  // swallow the touch that woke the screen

// Idle dimming: after DIM_AFTER_MS without touch the panel drops to ~10 %; the auto-sleep
// timeout from Settings then counts from that moment. A touch, wrist raise or PWR restores it.
#define DIM_AFTER_MS (30 * 1000)
#define DIM_BRIGHTNESS 26  // ~10 % of 255
#define IMU_POLL_MS 50     // accelerometer runs at ~21 Hz
#define IMU_LOG_MS (10 * 1000)

static bool s_dimmed = false;
static WristGesture s_wrist;

static uint8_t dim_brightness(void) {
  return s_policy.brightness < DIM_BRIGHTNESS ? s_policy.brightness : DIM_BRIGHTNESS;
}

static void screen_set_dimmed(bool dimmed) {
  s_dimmed = dimmed;
  board_display_set_brightness(dimmed ? dim_brightness() : s_policy.brightness);
}

// Back to full brightness (and on, if asleep) and restart the idle timer
static void screen_wake(void) {
  if (s_dimmed) {
    screen_set_dimmed(false);  // while asleep this only stores the level used by wake
  }
  if (board_display_is_sleeping()) {
    board_display_wake();
    lv_obj_invalidate(lv_screen_active());  // frames were not pushed while asleep
  }
  lv_display_trigger_activity(NULL);
}

static uint32_t millis_cb(void) {
  return millis();
}

// DIRECT render mode: LVGL draws into frame_buf; push the whole frame once per refresh
static void disp_flush(lv_display_t *display, const lv_area_t *area, uint8_t *px_map) {
  (void)area;
  if (lv_display_flush_is_last(display)) {
    board_display_push((const uint16_t *)px_map);
  }
  lv_display_flush_ready(display);
}

// CO5300 needs even-aligned update areas
static void rounder_event_cb(lv_event_t *e) {
  lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
  area->x1 = (area->x1 >> 1) << 1;
  area->y1 = (area->y1 >> 1) << 1;
  area->x2 = ((area->x2 >> 1) << 1) + 1;
  area->y2 = ((area->y2 >> 1) << 1) + 1;
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data) {
  (void)indev;
  int32_t x = 0;
  int32_t y = 0;
  bool pressed = board_touch_read(&x, &y);

  // The touch that wakes or brightens the screen is not passed on to the UI
  if (board_display_is_sleeping() || s_dimmed) {
    if (pressed) {
      screen_wake();
      s_touch_wake_guard = true;
    }
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  if (s_touch_wake_guard) {
    if (!pressed) {
      s_touch_wake_guard = false;
    }
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }

  if (pressed) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = x;
    data->point.y = y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

void system_apply_settings(void) {
  s_policy = power_policy_compute(g_settings, board_battery());
  board_display_set_brightness(s_dimmed ? dim_brightness() : s_policy.brightness);
  audio_set_volume(g_settings.volume);

  const TzEntry *tz = tz_find(g_settings.tz_name);
  board_time_apply_tz(tz ? tz->posix : "UTC0");
}

// Raise-to-view: only sampled while the screen is dimmed or asleep
static void wrist_tick(void) {
  static uint32_t last_poll_ms = 0;
  static uint32_t last_log_ms = 0;
  bool asleep = board_display_is_sleeping();
  if (!asleep && !s_dimmed) {
    wrist_gesture_reset(&s_wrist);
    return;
  }
  if (millis() - last_poll_ms < IMU_POLL_MS) {
    return;
  }
  last_poll_ms = millis();

  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  if (!board_imu_read_accel(&x, &y, &z)) {
    return;
  }
  if (millis() - last_log_ms > IMU_LOG_MS) {
    last_log_ms = millis();
    USBSerial.printf("IMU accel x=%.2f y=%.2f z=%.2f\n", x, y, z);
  }
  if (wrist_gesture_update(&s_wrist, x, y, z, millis())) {
    USBSerial.printf("Wrist raise (x=%.2f y=%.2f z=%.2f)\n", x, y, z);
    screen_wake();
    if (asleep) {
      launcher_go_home();  // raised to check the time
    }
  }
}

// Idle dim, auto-sleep, PWR key and periodic power-policy refresh
static void power_tick(void) {
  static uint32_t last_policy_ms = 0;
  if (millis() - last_policy_ms > 5000) {
    last_policy_ms = millis();
    PowerPolicy policy = power_policy_compute(g_settings, board_battery());
    bool changed = policy.brightness != s_policy.brightness;
    s_policy = policy;
    if (changed) {
      board_display_set_brightness(s_dimmed ? dim_brightness() : s_policy.brightness);
    }
  }

  // PWR short press: wake up / brighten / back to the watch face; on a bright watch face it
  // turns the screen off
  if (board_power_key_pressed()) {
    if (board_display_is_sleeping() || s_dimmed) {
      screen_wake();
      launcher_go_home();
    } else if (launcher_go_home()) {
      lv_display_trigger_activity(NULL);
    } else {
      board_display_sleep();
    }
    return;
  }

  wrist_tick();

  if (board_display_is_sleeping()) {
    return;
  }
  uint32_t inactive_ms = lv_display_get_inactive_time(NULL);
  if (!s_dimmed && inactive_ms > DIM_AFTER_MS) {
    screen_set_dimmed(true);
  } else if (s_dimmed && inactive_ms < DIM_AFTER_MS) {
    screen_set_dimmed(false);  // activity from elsewhere, e.g. launcher_go_home()
  }
  if (s_policy.sleep_timeout_ms > 0 && inactive_ms > DIM_AFTER_MS + s_policy.sleep_timeout_ms) {
    board_display_sleep();
  }
}

static bool lvgl_begin(void) {
  lv_init();
  lv_tick_set_cb(millis_cb);

  uint32_t width = board_display_width();
  uint32_t height = board_display_height();
  size_t buf_bytes = width * height * sizeof(uint16_t);

  // A full frame (~400 KB) only fits in PSRAM
  frame_buf = (uint16_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (frame_buf == nullptr) {
    frame_buf = (uint16_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_8BIT);
  }
  if (frame_buf == nullptr) {
    return false;
  }

  disp = lv_display_create(width, height);
  lv_display_set_flush_cb(disp, disp_flush);
  lv_display_set_buffers(disp, frame_buf, NULL, buf_bytes, LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_add_event_cb(disp, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touch_read);
  return true;
}

void system_setup(void) {
  USBSerial.begin(115200);
  USBSerial.printf("09_LVGL_App_Launcher firmware %s (running from %s)\n", FIRMWARE_VERSION, ota_running_partition());
  ota_begin();

  settings_load();

  if (!board_display_begin()) {
    USBSerial.println("Display init failed");
  }
  Wire.begin(IIC_SDA, IIC_SCL);
  if (!board_touch_begin()) {
    USBSerial.println("FT3168 init failed");
  }
  if (!board_pmu_begin()) {
    USBSerial.println("AXP2101 init failed");
  }
  if (!board_imu_begin()) {
    USBSerial.println("QMI8658 init failed (no raise-to-view)");
  }
  if (!board_rtc_begin()) {
    USBSerial.println("PCF85063 init failed");
  }
  if (!audio_begin()) {
    USBSerial.println("ES8311 init failed");
  }

  board_time_load_from_rtc();
  system_apply_settings();

  if (!lvgl_begin()) {
    USBSerial.println("LVGL frame buffer allocation failed");
    return;
  }

  wrist_gesture_reset(&s_wrist);
  ui_theme_apply(g_settings.dark_theme);
  launcher_install(&APP_AI_VOICE);
  launcher_install(&APP_SETTINGS);
  launcher_begin();

  net_begin();
  USBSerial.println("Setup done");
}

void system_loop(void) {
  if (disp == nullptr) {
    delay(1000);
    return;
  }
  lv_timer_handler();
  net_poll();
  power_tick();
  ota_confirm_tick();
  delay(5);
}
