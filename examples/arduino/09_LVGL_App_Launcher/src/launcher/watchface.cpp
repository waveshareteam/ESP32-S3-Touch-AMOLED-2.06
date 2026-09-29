#include <ctype.h>
#include <time.h>
#include "../board/board.h"
#include "../protocol/net.h"
#include "../core/settings_store.h"
#include "../components/ui_theme.h"
#include "watchface.h"

// Typographic watch faces. Tapping the face cycles through the styles (saved in NVS):
//  - phrase styles: big left-aligned words, the time in orange
//      It's fucking / 10:29 / on GOD DAMN / Monday / 28 September 2026 !!!
//  - split style:   IT'S | MON, SEP 28 / 10:29 ... AND / (M)ONDAY / STILL SUCKS

#define WATCH_ACCENT 0xFF7A45
#define WATCH_STRIKE 0xFF3B30
#define WATCH_MARGIN 26
#define WATCH_TICK_MS 500      // blink rate of the critical battery icon
#define WATCH_STATUS_RIGHT 56  // keeps the status icons clear of the rounded top-right corner

#define BAT_GREEN 0x30D158
#define BAT_YELLOW 0xFFD60A
#define BAT_RED 0xFF3B30

struct WatchPhrase {
  const char *before_time;  // line above the time
  const char *before_day;   // line between time and weekday
  const char *after_date;   // appended to the date line
  const char *date_format;  // strftime format of the date line
};

static const WatchPhrase PHRASES[] = {
  { "It's fucking", "on GOD DAMN", " !!!", "%d %B %Y" },
  { "Holy shit, it's", "on bloody", ". Move!", "%d %b %Y" },
  { "Get up, it's", "on freaking", ". No excuses!", "%d %b %Y" },
  { "Relax, it's only", "on a lazy", " :)", "%d %B %Y" },
};
static const uint8_t PHRASE_COUNT = sizeof(PHRASES) / sizeof(PHRASES[0]);
static const uint8_t STYLE_SPLIT = PHRASE_COUNT;  // last style in the cycle
static const uint8_t STYLE_COUNT = PHRASE_COUNT + 1;

static lv_timer_t *s_timer = nullptr;
static lv_obj_t *s_content = nullptr;
static lv_obj_t *s_before_time = nullptr;
static lv_obj_t *s_time = nullptr;
static lv_obj_t *s_before_day = nullptr;
static lv_obj_t *s_day_first = nullptr;  // split style: struck-through first letter
static lv_obj_t *s_day = nullptr;
static lv_obj_t *s_date = nullptr;
static lv_obj_t *s_wifi = nullptr;
static lv_obj_t *s_battery = nullptr;
static int s_last_minute = -1;
static uint8_t s_tick = 0;
static bool s_battery_blink = false;

static uint8_t current_style(void) {
  return g_settings.watch_style % STYLE_COUNT;
}

static const char *battery_symbol(int percent, bool charging) {
  if (charging) return LV_SYMBOL_CHARGE;
  if (percent >= 90) return LV_SYMBOL_BATTERY_FULL;
  if (percent >= 60) return LV_SYMBOL_BATTERY_3;
  if (percent >= 35) return LV_SYMBOL_BATTERY_2;
  if (percent >= 10) return LV_SYMBOL_BATTERY_1;
  return LV_SYMBOL_BATTERY_EMPTY;
}

static void to_upper(char *s) {
  for (; *s != '\0'; s++) {
    *s = (char)toupper((unsigned char)*s);
  }
}

/* ---------------- Text update ---------------- */

static void update_split(const struct tm &local, bool time_valid) {
  if (!time_valid) {
    lv_label_set_text(s_date, "SET TIME");
    lv_label_set_text(s_time, "--:--");
    lv_label_set_text(s_day_first, "S");
    lv_label_set_text(s_day, "OMEDAY");
    return;
  }
  char buf[32];
  strftime(buf, sizeof(buf), "%a, %b %d", &local);
  to_upper(buf);
  lv_label_set_text(s_date, buf);
  strftime(buf, sizeof(buf), "%H:%M", &local);
  lv_label_set_text(s_time, buf);
  strftime(buf, sizeof(buf), "%A", &local);
  to_upper(buf);
  char first[2] = { buf[0], '\0' };
  lv_label_set_text(s_day_first, first);
  lv_label_set_text(s_day, buf + 1);
}

static void update_phrase(const struct tm &local, bool time_valid) {
  const WatchPhrase &phrase = PHRASES[current_style()];
  lv_label_set_text(s_before_time, phrase.before_time);
  lv_label_set_text(s_before_day, phrase.before_day);
  if (!time_valid) {
    lv_label_set_text(s_time, "--:--");
    lv_label_set_text(s_day, "someday");
    lv_label_set_text(s_date, "Set the time via Wi-Fi");
    return;
  }
  char buf[48];
  strftime(buf, sizeof(buf), "%H:%M", &local);
  lv_label_set_text(s_time, buf);
  strftime(buf, sizeof(buf), "%A", &local);
  lv_label_set_text(s_day, buf);
  strftime(buf, sizeof(buf), phrase.date_format, &local);
  lv_label_set_text_fmt(s_date, "%s%s", buf, phrase.after_date);
}

static void update_text(bool force) {
  time_t now = time(nullptr);
  struct tm local;
  localtime_r(&now, &local);
  if (!force && local.tm_min == s_last_minute) {
    return;  // the text only changes once a minute
  }
  s_last_minute = local.tm_min;
  bool time_valid = (local.tm_year + 1900 >= 2024);
  if (current_style() == STYLE_SPLIT) {
    update_split(local, time_valid);
  } else {
    update_phrase(local, time_valid);
  }
}

// Battery colour rules:
//   full on USB -> green, charging < 80% -> yellow, charging >= 80% -> green,
//   on battery: >= 80% white, < 80% orange, < 40% red, < 20% blinking red
static lv_color_t battery_color(const BatteryInfo &bat, bool *blink) {
  *blink = false;
  int pct = bat.percent;
  if (bat.vbus_in && (pct >= 100 || !bat.charging)) {
    return lv_color_hex(BAT_GREEN);
  }
  if (bat.charging) {
    return lv_color_hex(pct < 80 ? BAT_YELLOW : BAT_GREEN);
  }
  if (pct >= 80) return g_ui.text;
  if (pct >= 40) return lv_color_hex(WATCH_ACCENT);
  *blink = (pct < 20);
  return lv_color_hex(BAT_RED);
}

static void update_status(void) {
  // Connected = white, connecting = orange, offline/failed = red: always visible, never faded out
  NetState state = net_state();
  lv_color_t wifi_color = state == NET_CONNECTED    ? g_ui.text
                          : state == NET_CONNECTING ? lv_color_hex(WATCH_ACCENT)
                                                    : g_ui.danger;
  lv_obj_set_style_text_color(s_wifi, wifi_color, 0);

  BatteryInfo bat = board_battery();
  if (bat.present && bat.percent >= 0) {
    lv_label_set_text_fmt(s_battery, "%d%% %s", bat.percent, battery_symbol(bat.percent, bat.charging));
    lv_obj_set_style_text_color(s_battery, battery_color(bat, &s_battery_blink), 0);
  } else {
    lv_label_set_text(s_battery, bat.vbus_in ? LV_SYMBOL_USB : "");
    lv_obj_set_style_text_color(s_battery, g_ui.text, 0);
    s_battery_blink = false;
  }
}

static void watchface_tick(lv_timer_t *t) {
  (void)t;
  s_tick++;
  update_text(false);
  if (s_tick % 4 == 0) {  // PMU/Wi-Fi status every 2 s
    update_status();
  }
  lv_obj_set_style_text_opa(s_battery, (s_battery_blink && (s_tick & 1)) ? LV_OPA_10 : LV_OPA_COVER, 0);
}

/* ---------------- Layouts ---------------- */

static lv_obj_t *add_container(lv_obj_t *parent, lv_flex_align_t cross) {
  lv_obj_t *box = lv_obj_create(parent);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, cross, cross);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);  // taps fall through to the face
  return box;
}

static void build_phrase_layout(void) {
  lv_obj_t *text = add_container(s_content, LV_FLEX_ALIGN_START);
  lv_obj_set_width(text, lv_pct(100));
  lv_obj_set_style_pad_hor(text, WATCH_MARGIN, 0);
  lv_obj_set_style_pad_row(text, 2, 0);
  lv_obj_align(text, LV_ALIGN_LEFT_MID, 0, 16);

  s_before_time = ui_label(text, "", &lv_font_montserrat_40, g_ui.text);
  s_time = ui_label(text, "", &lv_font_montserrat_48, lv_color_hex(WATCH_ACCENT));
  s_before_day = ui_label(text, "", &lv_font_montserrat_40, g_ui.text);
  s_day = ui_label(text, "", &lv_font_montserrat_40, g_ui.text);
  s_date = ui_label(text, "", &lv_font_montserrat_28, g_ui.muted);
}

static void build_split_layout(void) {
  lv_obj_t *its = ui_label(s_content, "IT'S", &lv_font_montserrat_40, g_ui.text);
  lv_obj_align(its, LV_ALIGN_TOP_LEFT, WATCH_MARGIN, 70);

  lv_obj_t *clock = add_container(s_content, LV_FLEX_ALIGN_END);
  lv_obj_align(clock, LV_ALIGN_TOP_RIGHT, -WATCH_MARGIN, 62);
  s_date = ui_label(clock, "", &lv_font_montserrat_22, g_ui.text);
  s_time = ui_label(clock, "", &lv_font_montserrat_48, g_ui.text);

  lv_obj_t *tag = add_container(s_content, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_row(tag, 6, 0);
  lv_obj_align(tag, LV_ALIGN_BOTTOM_RIGHT, -WATCH_MARGIN, -34);
  ui_label(tag, "AND", &lv_font_montserrat_36, g_ui.text);

  lv_obj_t *day_row = lv_obj_create(tag);
  lv_obj_remove_style_all(day_row);
  lv_obj_set_size(day_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(day_row, LV_FLEX_FLOW_ROW);
  lv_obj_clear_flag(day_row, LV_OBJ_FLAG_CLICKABLE);
  s_day_first = ui_label(day_row, "", &lv_font_montserrat_36, lv_color_hex(WATCH_STRIKE));
  lv_obj_set_style_text_decor(s_day_first, LV_TEXT_DECOR_STRIKETHROUGH, 0);
  s_day = ui_label(day_row, "", &lv_font_montserrat_36, lv_color_hex(WATCH_ACCENT));

  ui_label(tag, "STILL SUCKS", &lv_font_montserrat_36, g_ui.text);

  // Badge bottom-left (brand accent instead of a team logo)
  lv_obj_t *badge = lv_obj_create(s_content);
  lv_obj_remove_style_all(badge);
  lv_obj_set_size(badge, 50, 50);
  lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(badge, lv_color_hex(WATCH_ACCENT), 0);
  lv_obj_set_style_bg_grad_color(badge, lv_color_hex(WATCH_STRIKE), 0);
  lv_obj_set_style_bg_grad_dir(badge, LV_GRAD_DIR_VER, 0);
  lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(badge, LV_ALIGN_BOTTOM_LEFT, WATCH_MARGIN, -40);
  lv_obj_t *bang = ui_label(badge, "!!", &lv_font_montserrat_24, lv_color_white());
  lv_obj_center(bang);
}

static void build_content(void) {
  lv_obj_clean(s_content);
  s_before_time = s_time = s_before_day = s_day_first = s_day = s_date = nullptr;
  if (current_style() == STYLE_SPLIT) {
    build_split_layout();
  } else {
    build_phrase_layout();
  }
  update_text(true);
}

static void on_face_clicked(lv_event_t *e) {
  (void)e;
  g_settings.watch_style = (current_style() + 1) % STYLE_COUNT;
  settings_save();
  build_content();  // only children of s_content are rebuilt; the clicked face stays alive
}

void watchface_create(lv_obj_t *parent) {
  // Whole page is the tap target; swipes still bubble up to the launcher
  lv_obj_t *face = lv_obj_create(parent);
  lv_obj_remove_style_all(face);
  lv_obj_set_size(face, lv_pct(100), lv_pct(100));
  lv_obj_clear_flag(face, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(face, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(face, on_face_clicked, LV_EVENT_CLICKED, nullptr);

  // Status icons at the top right, inset from the rounded corner so they stay visible
  lv_obj_t *status = ui_row(face);
  lv_obj_set_flex_align(status, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(status, 14, 0);
  lv_obj_set_style_pad_right(status, WATCH_STATUS_RIGHT, 0);
  lv_obj_set_style_pad_top(status, 16, 0);
  lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_clear_flag(status, LV_OBJ_FLAG_CLICKABLE);
  s_wifi = ui_label(status, LV_SYMBOL_WIFI, &lv_font_montserrat_22, g_ui.text);
  s_battery = ui_label(status, "", &lv_font_montserrat_22, g_ui.text);

  s_content = lv_obj_create(face);
  lv_obj_remove_style_all(s_content);
  lv_obj_set_size(s_content, lv_pct(100), lv_pct(100));
  lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(s_content, LV_OBJ_FLAG_CLICKABLE);

  s_last_minute = -1;
  build_content();
  update_status();
  s_timer = lv_timer_create(watchface_tick, WATCH_TICK_MS, nullptr);
}

void watchface_destroy(void) {
  if (s_timer != nullptr) {
    lv_timer_delete(s_timer);
    s_timer = nullptr;
  }
  s_content = s_before_time = s_time = s_before_day = s_day_first = s_day = s_date = nullptr;
  s_wifi = s_battery = nullptr;
}
