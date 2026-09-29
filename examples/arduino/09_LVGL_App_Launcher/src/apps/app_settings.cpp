#include <time.h>
#include "../audio/audio.h"
#include "../board/board.h"
#include "../protocol/net.h"
#include "../core/settings_store.h"
#include "../core/modules.h"
#include "../core/ota.h"
#include "../server/ota_server.h"
#include "../components/timezones.h"
#include "../components/ui_theme.h"
#include "../apps/apps.h"

enum SettingsPage : uint8_t {
  PAGE_ROOT,
  PAGE_DISPLAY,
  PAGE_SOUND,
  PAGE_TIME,
  PAGE_BATTERY,
  PAGE_WIFI,
  PAGE_WIFI_PASSWORD,
  PAGE_UPDATE,
  PAGE_UPDATE_SERVER,
};

static lv_obj_t *s_body = nullptr;
static lv_obj_t *s_keyboard = nullptr;
static lv_timer_t *s_timer = nullptr;
static SettingsPage s_page = PAGE_ROOT;
static SettingsPage s_pending_page = PAGE_ROOT;

// Page widgets (valid only while their page is shown)
static lv_obj_t *s_value_label = nullptr;
static lv_obj_t *s_status_label = nullptr;
static lv_obj_t *s_extra_label = nullptr;
static lv_obj_t *s_bar = nullptr;
static lv_obj_t *s_ta_tz = nullptr;
static lv_obj_t *s_ta_location = nullptr;
static lv_obj_t *s_ta_password = nullptr;
static lv_obj_t *s_net_list = nullptr;
static lv_obj_t *s_ta_server = nullptr;
static lv_obj_t *s_install_btn = nullptr;
static lv_obj_t *s_check_btn = nullptr;
static bool s_net_list_filled = false;

// Wi-Fi selection / pending credentials
static char s_sel_ssid[33];
static bool s_sel_secure = true;
static char s_pending_pass[65];
static bool s_pending_save = false;

static void build_page(void);

/* ---------------- Navigation ---------------- */

static void stop_timer(void) {
  if (s_timer != nullptr) {
    lv_timer_delete(s_timer);
    s_timer = nullptr;
  }
}

static void start_timer(lv_timer_cb_t cb, uint32_t period_ms) {
  stop_timer();
  s_timer = lv_timer_create(cb, period_ms, nullptr);
  cb(s_timer);
}

static void navigate_async(void *param) {
  (void)param;
  if (s_body == nullptr) {
    return;  // app was closed before the async call ran
  }
  stop_timer();
  if (s_keyboard != nullptr) {
    lv_obj_delete(s_keyboard);
    s_keyboard = nullptr;
  }
  lv_obj_clean(s_body);
  lv_obj_set_style_pad_bottom(s_body, 24, 0);
  lv_obj_scroll_to_y(s_body, 0, LV_ANIM_OFF);
  s_page = s_pending_page;
  build_page();
}

// Pages are switched asynchronously because the triggering widget is deleted by the switch
static void navigate(SettingsPage page) {
  s_pending_page = page;
  lv_async_call(navigate_async, nullptr);
}

static void on_nav_clicked(lv_event_t *e) {
  navigate((SettingsPage)(intptr_t)lv_event_get_user_data(e));
}

static void add_sub_header(const char *title, SettingsPage parent) {
  lv_obj_t *btn = lv_button_create(s_body);
  lv_obj_set_height(btn, 44);
  lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_set_style_pad_hor(btn, 4, 0);
  lv_obj_add_event_cb(btn, on_nav_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)parent);
  lv_obj_t *label = ui_label(btn, "", &lv_font_montserrat_22, g_ui.accent);
  lv_label_set_text_fmt(label, LV_SYMBOL_LEFT "  %s", title);
  lv_obj_center(label);
}

/* ---------------- On-screen keyboard ---------------- */

static void keyboard_hide(void) {
  if (s_keyboard == nullptr) {
    return;
  }
  lv_obj_t *ta = lv_keyboard_get_textarea(s_keyboard);
  if (ta != nullptr) {
    lv_obj_remove_state(ta, LV_STATE_FOCUSED);
  }
  lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_pad_bottom(s_body, 24, 0);
}

static void on_password_ready(void);

static void on_keyboard_event(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY && s_page == PAGE_WIFI_PASSWORD) {
    keyboard_hide();
    on_password_ready();
  } else if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
    keyboard_hide();
  }
}

static void keyboard_show(lv_obj_t *ta) {
  if (s_keyboard == nullptr) {
    s_keyboard = lv_keyboard_create(lv_obj_get_screen(s_body));
    lv_obj_set_size(s_keyboard, lv_pct(100), lv_pct(45));
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(s_keyboard, on_keyboard_event, LV_EVENT_ALL, nullptr);
    ui_no_gesture(s_keyboard);
  }
  lv_keyboard_set_textarea(s_keyboard, ta);
  lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_state(ta, LV_STATE_FOCUSED);

  // Leave room under the content so the focused field can scroll above the keyboard
  lv_obj_update_layout(s_keyboard);
  lv_obj_set_style_pad_bottom(s_body, lv_obj_get_height(s_keyboard) + 24, 0);
  lv_obj_update_layout(s_body);
  lv_obj_scroll_to_view_recursive(ta, LV_ANIM_ON);
}

static void on_textarea_clicked(lv_event_t *e) {
  keyboard_show((lv_obj_t *)lv_event_get_target(e));
}

static lv_obj_t *add_textarea(lv_obj_t *parent, const char *text, const char *placeholder, size_t max_len) {
  lv_obj_t *ta = lv_textarea_create(parent);
  lv_obj_set_width(ta, lv_pct(100));
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_max_length(ta, max_len);
  lv_textarea_set_placeholder_text(ta, placeholder);
  lv_textarea_set_text(ta, text);
  lv_obj_add_event_cb(ta, on_textarea_clicked, LV_EVENT_CLICKED, nullptr);
  ui_no_gesture(ta);
  return ta;
}

/* ---------------- Root ---------------- */

static void add_root_item(lv_obj_t *card, const char *symbol, uint32_t color, const char *text, SettingsPage page) {
  lv_obj_t *btn = lv_button_create(card);
  lv_obj_set_size(btn, lv_pct(100), 56);
  lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_set_style_pad_all(btn, 0, 0);
  lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(btn, 14, 0);
  lv_obj_add_event_cb(btn, on_nav_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)page);

  lv_obj_t *badge = lv_obj_create(btn);
  lv_obj_remove_style_all(badge);
  lv_obj_set_size(badge, 40, 40);
  lv_obj_set_style_radius(badge, 10, 0);
  lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(badge, lv_color_hex(color), 0);
  lv_obj_add_flag(badge, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_t *icon = ui_label(badge, symbol, &lv_font_montserrat_20, lv_color_white());
  lv_obj_center(icon);

  lv_obj_t *label = ui_label(btn, text, &lv_font_montserrat_22, g_ui.text);
  lv_obj_set_flex_grow(label, 1);
  ui_label(btn, LV_SYMBOL_RIGHT, &lv_font_montserrat_18, g_ui.muted);
}

static void build_root(void) {
  lv_obj_t *card = ui_card(s_body);
  lv_obj_set_style_pad_ver(card, 6, 0);
  lv_obj_set_style_pad_row(card, 0, 0);
  add_root_item(card, LV_SYMBOL_IMAGE, 0x0A84FF, "Display", PAGE_DISPLAY);
  add_root_item(card, LV_SYMBOL_VOLUME_MAX, 0xFF375F, "Sound", PAGE_SOUND);
  add_root_item(card, LV_SYMBOL_GPS, 0xFF9F0A, "Time & Location", PAGE_TIME);
  add_root_item(card, LV_SYMBOL_BATTERY_FULL, 0x30D158, "Battery", PAGE_BATTERY);
  add_root_item(card, LV_SYMBOL_WIFI, 0x64D2FF, "Wi-Fi", PAGE_WIFI);
  add_root_item(card, LV_SYMBOL_DOWNLOAD, 0xBF5AF2, "Software Update", PAGE_UPDATE);
}

/* ---------------- Display ---------------- */

static void on_brightness_changed(lv_event_t *e) {
  lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
  g_settings.brightness = (uint8_t)lv_slider_get_value(slider);
  lv_label_set_text_fmt(s_value_label, "%d%%", g_settings.brightness * 100 / 255);
  system_apply_settings();
  if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
    settings_save();
  }
}

static void rebuild_for_theme(void *param) {
  (void)param;
  ui_theme_apply(g_settings.dark_theme);
  launcher_rebuild();
}

static void on_theme_changed(lv_event_t *e) {
  lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
  g_settings.dark_theme = lv_obj_has_state(sw, LV_STATE_CHECKED);
  settings_save();
  lv_async_call(rebuild_for_theme, nullptr);
}

static void on_sleep_changed(lv_event_t *e) {
  lv_obj_t *dd = (lv_obj_t *)lv_event_get_target(e);
  g_settings.sleep_mode = (uint8_t)lv_dropdown_get_selected(dd);
  settings_save();
  system_apply_settings();
}

static void build_display(void) {
  add_sub_header("Display", PAGE_ROOT);

  lv_obj_t *card = ui_card(s_body);
  lv_obj_t *row = ui_row(card);
  ui_label(row, LV_SYMBOL_EYE_OPEN "  Brightness", &lv_font_montserrat_20, g_ui.text);
  s_value_label = ui_label(row, "", &lv_font_montserrat_20, g_ui.muted);
  lv_label_set_text_fmt(s_value_label, "%d%%", g_settings.brightness * 100 / 255);
  lv_obj_t *slider = lv_slider_create(card);
  lv_obj_set_width(slider, lv_pct(100));
  lv_slider_set_range(slider, 5, 255);
  lv_slider_set_value(slider, g_settings.brightness, LV_ANIM_OFF);
  lv_obj_add_event_cb(slider, on_brightness_changed, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(slider, on_brightness_changed, LV_EVENT_RELEASED, nullptr);
  ui_no_gesture(slider);

  card = ui_card(s_body);
  row = ui_row(card);
  ui_label(row, "Dark theme", &lv_font_montserrat_20, g_ui.text);
  lv_obj_t *sw = lv_switch_create(row);
  if (g_settings.dark_theme) {
    lv_obj_add_state(sw, LV_STATE_CHECKED);
  }
  lv_obj_add_event_cb(sw, on_theme_changed, LV_EVENT_VALUE_CHANGED, nullptr);

  card = ui_card(s_body);
  row = ui_row(card);
  ui_label(row, "Auto sleep", &lv_font_montserrat_20, g_ui.text);
  lv_obj_t *dd = lv_dropdown_create(row);
  lv_obj_set_width(dd, 150);
  lv_dropdown_set_options_static(dd, "30 Sec\n1 Min\nNever");
  lv_dropdown_set_selected(dd, g_settings.sleep_mode);
  lv_obj_add_event_cb(dd, on_sleep_changed, LV_EVENT_VALUE_CHANGED, nullptr);
  if (g_settings.low_battery_mode) {
    ui_label(card, "Low Battery Mode may override these values", &lv_font_montserrat_14, g_ui.muted);
  }
}

/* ---------------- Sound ---------------- */

static void on_volume_changed(lv_event_t *e) {
  lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
  g_settings.volume = (uint8_t)lv_slider_get_value(slider);
  lv_label_set_text_fmt(s_value_label, "%d%%", g_settings.volume);
  if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
    settings_save();
    audio_set_volume(g_settings.volume);
    audio_beep();
  }
}

static void on_test_sound(lv_event_t *e) {
  (void)e;
  audio_beep();
}

static void build_sound(void) {
  add_sub_header("Sound", PAGE_ROOT);

  lv_obj_t *card = ui_card(s_body);
  lv_obj_t *row = ui_row(card);
  ui_label(row, LV_SYMBOL_VOLUME_MAX "  Speaker volume", &lv_font_montserrat_20, g_ui.text);
  s_value_label = ui_label(row, "", &lv_font_montserrat_20, g_ui.muted);
  lv_label_set_text_fmt(s_value_label, "%d%%", g_settings.volume);
  lv_obj_t *slider = lv_slider_create(card);
  lv_obj_set_width(slider, lv_pct(100));
  lv_slider_set_range(slider, 0, 100);
  lv_slider_set_value(slider, g_settings.volume, LV_ANIM_OFF);
  lv_obj_add_event_cb(slider, on_volume_changed, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(slider, on_volume_changed, LV_EVENT_RELEASED, nullptr);
  ui_no_gesture(slider);

  lv_obj_t *btn = ui_button(s_body, LV_SYMBOL_PLAY "  Test sound", on_test_sound, nullptr);
  lv_obj_set_width(btn, lv_pct(100));
}

/* ---------------- Time zone & location ---------------- */

static void update_clock_preview(lv_timer_t *t) {
  (void)t;
  time_t now = time(nullptr);
  struct tm local;
  localtime_r(&now, &local);
  char buf[40];
  strftime(buf, sizeof(buf), "%H:%M:%S  %d/%m/%Y", &local);
  lv_label_set_text(s_value_label, buf);
}

static void on_tz_picked(lv_event_t *e) {
  lv_obj_t *dd = (lv_obj_t *)lv_event_get_target(e);
  uint32_t index = lv_dropdown_get_selected(dd);
  if (index < TZ_TABLE_COUNT) {
    lv_textarea_set_text(s_ta_tz, TZ_TABLE[index].name);
  }
}

static void on_time_save(lv_event_t *e) {
  (void)e;
  keyboard_hide();
  const TzEntry *tz = tz_find(lv_textarea_get_text(s_ta_tz));
  if (tz == nullptr) {
    lv_label_set_text(s_status_label, LV_SYMBOL_WARNING "  Unknown time zone");
    lv_obj_set_style_text_color(s_status_label, g_ui.danger, 0);
    return;
  }
  strlcpy(g_settings.tz_name, tz->name, sizeof(g_settings.tz_name));  // canonical spelling
  strlcpy(g_settings.location, lv_textarea_get_text(s_ta_location), sizeof(g_settings.location));
  lv_textarea_set_text(s_ta_tz, g_settings.tz_name);
  settings_save();
  system_apply_settings();
  lv_label_set_text(s_status_label, LV_SYMBOL_OK "  Saved");
  lv_obj_set_style_text_color(s_status_label, g_ui.accent, 0);
}

static void build_time(void) {
  static char tz_options[TZ_TABLE_COUNT * 24];
  if (tz_options[0] == '\0') {
    for (size_t i = 0; i < TZ_TABLE_COUNT; i++) {
      if (i > 0) {
        strlcat(tz_options, "\n", sizeof(tz_options));
      }
      strlcat(tz_options, TZ_TABLE[i].name, sizeof(tz_options));
    }
  }

  add_sub_header("Time & Location", PAGE_ROOT);

  lv_obj_t *card = ui_card(s_body);
  ui_label(card, "Local time", &lv_font_montserrat_16, g_ui.muted);
  s_value_label = ui_label(card, "", &lv_font_montserrat_24, g_ui.text);

  card = ui_card(s_body);
  ui_label(card, "Time zone (e.g. Asia/Bangkok)", &lv_font_montserrat_16, g_ui.muted);
  s_ta_tz = add_textarea(card, g_settings.tz_name, "Region/City", sizeof(g_settings.tz_name) - 1);
  lv_obj_t *dd = lv_dropdown_create(card);
  lv_obj_set_width(dd, lv_pct(100));
  lv_dropdown_set_text(dd, "Pick from list");
  lv_dropdown_set_options_static(dd, tz_options);
  lv_obj_add_event_cb(dd, on_tz_picked, LV_EVENT_VALUE_CHANGED, nullptr);

  ui_label(card, "Location name", &lv_font_montserrat_16, g_ui.muted);
  s_ta_location = add_textarea(card, g_settings.location, "City", sizeof(g_settings.location) - 1);

  lv_obj_t *btn = ui_button(s_body, LV_SYMBOL_SAVE "  Save", on_time_save, nullptr);
  lv_obj_set_width(btn, lv_pct(100));
  s_status_label = ui_label(s_body, "", &lv_font_montserrat_18, g_ui.muted);

  start_timer(update_clock_preview, 1000);
}

/* ---------------- Battery ---------------- */

static void update_battery(lv_timer_t *t) {
  (void)t;
  BatteryInfo bat = board_battery();
  if (bat.present && bat.percent >= 0) {
    lv_label_set_text_fmt(s_value_label, "%d%%", bat.percent);
    lv_bar_set_value(s_bar, bat.percent, LV_ANIM_ON);
    lv_label_set_text_fmt(s_status_label, "%u mV", bat.voltage_mv);
  } else {
    lv_label_set_text(s_value_label, "--");
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_label_set_text(s_status_label, "No battery detected");
  }
  lv_label_set_text_fmt(s_extra_label, "%s   USB: %s", bat.charging ? LV_SYMBOL_CHARGE " Charging" : "Not charging",
                        bat.vbus_in ? "yes" : "no");
}

static void on_low_battery_changed(lv_event_t *e) {
  lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
  g_settings.low_battery_mode = lv_obj_has_state(sw, LV_STATE_CHECKED);
  settings_save();
  system_apply_settings();
}

static void build_battery(void) {
  add_sub_header("Battery", PAGE_ROOT);

  lv_obj_t *card = ui_card(s_body);
  lv_obj_t *row = ui_row(card);
  ui_label(row, LV_SYMBOL_BATTERY_FULL "  Level", &lv_font_montserrat_20, g_ui.text);
  s_value_label = ui_label(row, "--", &lv_font_montserrat_28, g_ui.text);
  s_bar = lv_bar_create(card);
  lv_obj_set_size(s_bar, lv_pct(100), 16);
  lv_bar_set_range(s_bar, 0, 100);
  s_status_label = ui_label(card, "", &lv_font_montserrat_18, g_ui.muted);
  s_extra_label = ui_label(card, "", &lv_font_montserrat_18, g_ui.muted);

  card = ui_card(s_body);
  row = ui_row(card);
  ui_label(row, "Low Battery Mode", &lv_font_montserrat_20, g_ui.text);
  lv_obj_t *sw = lv_switch_create(row);
  if (g_settings.low_battery_mode) {
    lv_obj_add_state(sw, LV_STATE_CHECKED);
  }
  lv_obj_add_event_cb(sw, on_low_battery_changed, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_t *hint = ui_label(card, "On battery only. At 40% or less: brightness up to 40%, screen off within 30 s. At 20% or less: brightness up to 15%, screen off after 15 s.",
                            &lv_font_montserrat_16, g_ui.muted);
  lv_obj_set_width(hint, lv_pct(100));
  lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);

  start_timer(update_battery, 2000);
}

/* ---------------- Wi-Fi ---------------- */

static void update_wifi_status(void) {
  NetState state = net_state();
  switch (state) {
    case NET_CONNECTED:
      lv_label_set_text_fmt(s_status_label, LV_SYMBOL_OK "  %s\n%s", net_ssid().c_str(), net_ip().c_str());
      break;
    case NET_CONNECTING:
      lv_label_set_text(s_status_label, LV_SYMBOL_REFRESH "  Connecting...");
      break;
    case NET_FAILED:
      lv_label_set_text(s_status_label, LV_SYMBOL_WARNING "  Connection failed");
      break;
    default:
      lv_label_set_text(s_status_label, "Not connected");
      break;
  }
  lv_obj_set_style_text_color(s_status_label, state == NET_FAILED ? g_ui.danger : g_ui.text, 0);

  // Save credentials only after the connection has been proven to work
  if (s_pending_save && state == NET_CONNECTED) {
    s_pending_save = false;
    strlcpy(g_settings.wifi_ssid, s_sel_ssid, sizeof(g_settings.wifi_ssid));
    strlcpy(g_settings.wifi_pass, s_pending_pass, sizeof(g_settings.wifi_pass));
    memset(s_pending_pass, 0, sizeof(s_pending_pass));
    settings_save();
  } else if (s_pending_save && state == NET_FAILED) {
    s_pending_save = false;
    memset(s_pending_pass, 0, sizeof(s_pending_pass));
  }
}

static void connect_selected(const char *password) {
  strlcpy(s_pending_pass, password, sizeof(s_pending_pass));
  s_pending_save = true;
  net_connect(s_sel_ssid, s_pending_pass);
}

static void on_network_clicked(lv_event_t *e) {
  const NetScanEntry *entry = net_scan_entry((int)(intptr_t)lv_event_get_user_data(e));
  if (entry == nullptr) {
    return;
  }
  strlcpy(s_sel_ssid, entry->ssid, sizeof(s_sel_ssid));
  s_sel_secure = entry->secure;
  if (s_sel_secure) {
    navigate(PAGE_WIFI_PASSWORD);
  } else {
    connect_selected("");
  }
}

static void wifi_page_tick(lv_timer_t *t) {
  (void)t;
  update_wifi_status();
  if (s_net_list_filled) {
    return;
  }
  int count = net_scan_result();
  if (count == NET_SCAN_RUNNING) {
    lv_label_set_text(s_extra_label, LV_SYMBOL_REFRESH "  Scanning...");
    return;
  }
  if (count == NET_SCAN_FAILED) {
    lv_label_set_text(s_extra_label, LV_SYMBOL_WARNING "  Scan failed");
    s_net_list_filled = true;
    return;
  }
  lv_label_set_text_fmt(s_extra_label, "%d networks", count);
  for (int i = 0; i < count; i++) {
    const NetScanEntry *entry = net_scan_entry(i);
    lv_obj_t *btn = lv_list_add_button(s_net_list, entry->secure ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_WIFI, entry->ssid);
    lv_obj_add_event_cb(btn, on_network_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_t *rssi = ui_label(btn, "", &lv_font_montserrat_14, g_ui.muted);
    lv_label_set_text_fmt(rssi, "%d dBm", (int)entry->rssi);
  }
  s_net_list_filled = true;
}

static void on_scan_clicked(lv_event_t *e) {
  (void)e;
  lv_obj_clean(s_net_list);
  s_net_list_filled = false;
  net_scan_start();
}

static void on_forget_clicked(lv_event_t *e) {
  (void)e;
  g_settings.wifi_ssid[0] = '\0';
  memset(g_settings.wifi_pass, 0, sizeof(g_settings.wifi_pass));
  settings_save();
  net_disconnect();
  navigate(PAGE_WIFI);
}

static void build_wifi(void) {
  add_sub_header("Wi-Fi", PAGE_ROOT);

  lv_obj_t *card = ui_card(s_body);
  s_status_label = ui_label(card, "", &lv_font_montserrat_20, g_ui.text);
  if (g_settings.wifi_ssid[0] != '\0') {
    lv_obj_t *saved = ui_label(card, "", &lv_font_montserrat_16, g_ui.muted);
    lv_label_set_text_fmt(saved, "Saved: %s", g_settings.wifi_ssid);
    lv_obj_t *forget = ui_button(card, LV_SYMBOL_TRASH "  Forget", on_forget_clicked, nullptr);
    lv_obj_set_style_bg_color(forget, g_ui.danger, 0);
  }

  lv_obj_t *row = ui_row(s_body);
  s_extra_label = ui_label(row, "", &lv_font_montserrat_18, g_ui.muted);
  ui_button(row, LV_SYMBOL_REFRESH "  Scan", on_scan_clicked, nullptr);

  s_net_list = lv_list_create(s_body);
  lv_obj_set_size(s_net_list, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(s_net_list, g_ui.card, 0);
  lv_obj_set_style_border_width(s_net_list, 0, 0);
  lv_obj_set_style_radius(s_net_list, 18, 0);

  s_net_list_filled = false;
  net_scan_start();
  start_timer(wifi_page_tick, 500);
}

static void on_password_ready(void) {
  connect_selected(lv_textarea_get_text(s_ta_password));
  lv_textarea_set_text(s_ta_password, "");
}

static void on_connect_clicked(lv_event_t *e) {
  (void)e;
  keyboard_hide();
  on_password_ready();
}

static void on_show_password(lv_event_t *e) {
  lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
  lv_textarea_set_password_mode(s_ta_password, !lv_obj_has_state(sw, LV_STATE_CHECKED));
}

static void password_page_tick(lv_timer_t *t) {
  (void)t;
  update_wifi_status();
}

static void build_wifi_password(void) {
  add_sub_header("Wi-Fi", PAGE_WIFI);

  lv_obj_t *card = ui_card(s_body);
  lv_obj_t *title = ui_label(card, "", &lv_font_montserrat_22, g_ui.text);
  lv_label_set_text_fmt(title, LV_SYMBOL_WIFI "  %s", s_sel_ssid);
  s_ta_password = add_textarea(card, "", "Password", 64);
  lv_textarea_set_password_mode(s_ta_password, true);
  lv_obj_t *row = ui_row(card);
  ui_label(row, "Show password", &lv_font_montserrat_18, g_ui.muted);
  lv_obj_t *sw = lv_switch_create(row);
  lv_obj_add_event_cb(sw, on_show_password, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t *btn = ui_button(s_body, "Connect", on_connect_clicked, nullptr);
  lv_obj_set_width(btn, lv_pct(100));
  s_status_label = ui_label(s_body, "", &lv_font_montserrat_18, g_ui.text);

  start_timer(password_page_tick, 500);
  keyboard_show(s_ta_password);
}

/* ---------------- Software update (OTA) ---------------- */

static void update_page_tick(lv_timer_t *t) {
  (void)t;
  OtaStatus st = ota_get_status();
  bool busy = (st.stage == OTA_CHECKING || st.stage == OTA_INSTALLING || st.stage == OTA_REBOOTING);

  lv_label_set_text(s_status_label, st.message[0] ? st.message : "Tap Check for updates");
  lv_obj_set_style_text_color(s_status_label, st.stage == OTA_FAILED ? g_ui.danger : g_ui.text, 0);
  lv_label_set_text(s_extra_label, st.available);
  lv_bar_set_value(s_bar, st.stage == OTA_INSTALLING || st.stage == OTA_REBOOTING ? st.progress : 0, LV_ANIM_OFF);

  if (busy) {
    lv_obj_add_state(s_check_btn, LV_STATE_DISABLED);
  } else {
    lv_obj_remove_state(s_check_btn, LV_STATE_DISABLED);
  }
  if (st.stage == OTA_AVAILABLE) {
    lv_obj_remove_flag(s_install_btn, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_install_btn, LV_OBJ_FLAG_HIDDEN);
  }
}

static void on_check_clicked(lv_event_t *e) {
  (void)e;
  ota_check_async();
}

static void on_install_clicked(lv_event_t *e) {
  (void)e;
  ota_install_async();
}

static void on_rollback_clicked(lv_event_t *e) {
  (void)e;
  ota_rollback_and_reboot();
}

static void build_update(void) {
  add_sub_header("Software Update", PAGE_ROOT);

  // Installed firmware + per-module versions
  lv_obj_t *card = ui_card(s_body);
  lv_obj_t *title = ui_label(card, "", &lv_font_montserrat_22, g_ui.text);
  lv_label_set_text_fmt(title, "Firmware %s", FIRMWARE_VERSION);
  lv_obj_t *slot = ui_label(card, "", &lv_font_montserrat_16, ota_is_pending_verify() ? g_ui.danger : g_ui.muted);
  lv_label_set_text_fmt(slot, "Running from %s%s", ota_running_partition(),
                        ota_is_pending_verify() ? " (verifying new image)" : "");
  for (size_t i = 0; i < MODULE_COUNT; i++) {
    lv_obj_t *row = ui_row(card);
    ui_label(row, MODULES[i].name, &lv_font_montserrat_16, g_ui.text);
    lv_obj_t *ver = ui_label(row, "", &lv_font_montserrat_16, g_ui.muted);
    if (MODULES[i].data_partition != nullptr) {
      String data = ota_data_version(MODULES[i].data_partition);
      lv_label_set_text_fmt(ver, "%s  data:%s", MODULES[i].version, data.length() ? data.c_str() : "-");
    } else {
      lv_label_set_text(ver, MODULES[i].version);
    }
  }

  // Server
  card = ui_card(s_body);
  ui_label(card, "Update server (manifest)", &lv_font_montserrat_16, g_ui.muted);
  String url = server_manifest_url();
  lv_obj_t *url_label = ui_label(card, url.length() ? url.c_str() : "Not configured", &lv_font_montserrat_14,
                                 g_ui.text);
  lv_obj_set_width(url_label, lv_pct(100));
  lv_label_set_long_mode(url_label, LV_LABEL_LONG_WRAP);
  lv_obj_t *edit = ui_button(card, LV_SYMBOL_EDIT "  Edit server", on_nav_clicked, (void *)(intptr_t)PAGE_UPDATE_SERVER);
  lv_obj_set_width(edit, lv_pct(100));

  // Actions + progress
  s_check_btn = ui_button(s_body, LV_SYMBOL_REFRESH "  Check for updates", on_check_clicked, nullptr);
  lv_obj_set_width(s_check_btn, lv_pct(100));
  s_install_btn = ui_button(s_body, LV_SYMBOL_DOWNLOAD "  Install", on_install_clicked, nullptr);
  lv_obj_set_width(s_install_btn, lv_pct(100));
  lv_obj_add_flag(s_install_btn, LV_OBJ_FLAG_HIDDEN);

  s_bar = lv_bar_create(s_body);
  lv_obj_set_size(s_bar, lv_pct(100), 12);
  lv_bar_set_range(s_bar, 0, 100);
  s_status_label = ui_label(s_body, "", &lv_font_montserrat_18, g_ui.text);
  lv_obj_set_width(s_status_label, lv_pct(100));
  lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_WRAP);
  s_extra_label = ui_label(s_body, "", &lv_font_montserrat_16, g_ui.muted);
  lv_obj_set_width(s_extra_label, lv_pct(100));
  lv_label_set_long_mode(s_extra_label, LV_LABEL_LONG_WRAP);

  if (ota_can_rollback()) {
    lv_obj_t *rb = ui_button(s_body, LV_SYMBOL_LOOP "  Roll back to previous", on_rollback_clicked, nullptr);
    lv_obj_set_width(rb, lv_pct(100));
    lv_obj_set_style_bg_color(rb, g_ui.danger, 0);
  }

  start_timer(update_page_tick, 300);
}

static void on_server_save(lv_event_t *e) {
  (void)e;
  keyboard_hide();
  const char *url = lv_textarea_get_text(s_ta_server);
  if (url[0] != '\0' && strncmp(url, "https://", 8) != 0) {
    lv_label_set_text(s_status_label, LV_SYMBOL_WARNING "  URL must start with https://");
    lv_obj_set_style_text_color(s_status_label, g_ui.danger, 0);
    return;
  }
  server_set_manifest_url(url);
  navigate(PAGE_UPDATE);
}

static void build_update_server(void) {
  add_sub_header("Software Update", PAGE_UPDATE);

  lv_obj_t *card = ui_card(s_body);
  ui_label(card, "Manifest URL (https://...)", &lv_font_montserrat_16, g_ui.muted);
  String url = server_manifest_url();
  s_ta_server = add_textarea(card, url.c_str(), "https://host/manifest.json", 255);
  lv_textarea_set_one_line(s_ta_server, false);
  lv_obj_set_height(s_ta_server, 110);

  lv_obj_t *btn = ui_button(s_body, LV_SYMBOL_SAVE "  Save", on_server_save, nullptr);
  lv_obj_set_width(btn, lv_pct(100));
  s_status_label = ui_label(s_body, "", &lv_font_montserrat_18, g_ui.muted);
  keyboard_show(s_ta_server);
}

/* ---------------- App hooks ---------------- */

static void build_page(void) {
  switch (s_page) {
    case PAGE_DISPLAY: build_display(); break;
    case PAGE_SOUND: build_sound(); break;
    case PAGE_TIME: build_time(); break;
    case PAGE_BATTERY: build_battery(); break;
    case PAGE_WIFI: build_wifi(); break;
    case PAGE_WIFI_PASSWORD: build_wifi_password(); break;
    case PAGE_UPDATE: build_update(); break;
    case PAGE_UPDATE_SERVER: build_update_server(); break;
    default: build_root(); break;
  }
}

static void settings_create(lv_obj_t *body, bool restore) {
  s_body = body;
  s_keyboard = nullptr;
  if (!restore || s_page == PAGE_WIFI_PASSWORD) {
    s_page = restore ? PAGE_WIFI : PAGE_ROOT;
  }
  build_page();
}

static void settings_close(void) {
  stop_timer();
  s_body = nullptr;
  s_keyboard = nullptr;  // deleted together with the app screen
  s_pending_save = false;
  memset(s_pending_pass, 0, sizeof(s_pending_pass));
}

const LauncherApp APP_SETTINGS = {
  "Settings",
  LV_SYMBOL_SETTINGS,
  0x8E8E93,
  0x3A3A3C,
  settings_create,
  settings_close,
  nullptr,
};
