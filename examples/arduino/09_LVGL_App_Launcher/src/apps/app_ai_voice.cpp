#include "../core/app_switch.h"
#include "../components/ui_theme.h"
#include "../apps/apps.h"
#include "ai_voice_icon.h"

// AI Voice runs as a separate firmware (xiaozhi-esp32) in the voice0/voice1 app slots.
// This app shows its status and hands the device over to it; pressing PWR inside AI Voice
// switches back to the Launcher (holding BOOT there opens its LAN update page).

// Same orange as the watch face time, fading to red
#define AI_VOICE_COLOR_TOP 0xFF9F45
#define AI_VOICE_COLOR_BOTTOM 0xFF3B30
#define AI_VOICE_ORB 168

static lv_obj_t *s_overlay = nullptr;
static lv_obj_t *s_overlay_label = nullptr;
static lv_timer_t *s_switch_timer = nullptr;

static void halo_anim_cb(void *var, int32_t v) {
  lv_obj_t *halo = (lv_obj_t *)var;
  lv_obj_set_size(halo, AI_VOICE_ORB + v, AI_VOICE_ORB + v);
  lv_obj_set_style_border_opa(halo, (lv_opa_t)(LV_OPA_60 - v * 2), 0);
}

static lv_obj_t *create_orb(lv_obj_t *parent) {
  // Fixed-size stage so the breathing halo does not move the layout below it
  lv_obj_t *stage = lv_obj_create(parent);
  lv_obj_remove_style_all(stage);
  lv_obj_set_size(stage, AI_VOICE_ORB + 40, AI_VOICE_ORB + 40);
  lv_obj_clear_flag(stage, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *halo = lv_obj_create(stage);
  lv_obj_remove_style_all(halo);
  lv_obj_set_style_radius(halo, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(halo, 3, 0);
  lv_obj_set_style_border_color(halo, lv_color_hex(AI_VOICE_COLOR_BOTTOM), 0);
  lv_obj_center(halo);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, halo);
  lv_anim_set_exec_cb(&a, halo_anim_cb);
  lv_anim_set_values(&a, 0, 30);
  lv_anim_set_duration(&a, 1400);
  lv_anim_set_playback_duration(&a, 1400);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);  // deleted automatically together with the halo object

  lv_obj_t *orb = lv_obj_create(stage);
  lv_obj_remove_style_all(orb);
  lv_obj_set_size(orb, AI_VOICE_ORB, AI_VOICE_ORB);
  lv_obj_set_style_radius(orb, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(orb, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(orb, lv_color_hex(AI_VOICE_COLOR_TOP), 0);
  lv_obj_set_style_bg_grad_color(orb, lv_color_hex(AI_VOICE_COLOR_BOTTOM), 0);
  lv_obj_set_style_bg_grad_dir(orb, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_border_width(orb, 4, 0);
  lv_obj_set_style_border_color(orb, lv_color_white(), 0);
  lv_obj_set_style_border_opa(orb, LV_OPA_20, 0);
  lv_obj_center(orb);

  lv_obj_t *logo = lv_image_create(orb);
  lv_image_set_src(logo, &ai_voice_icon_96);
  lv_obj_center(logo);
  return stage;
}

static void add_info_row(lv_obj_t *card, const char *symbol, const char *title, const char *value, lv_color_t color) {
  lv_obj_t *row = ui_row(card);
  lv_obj_t *left = ui_label(row, "", &lv_font_montserrat_18, g_ui.muted);
  lv_label_set_text_fmt(left, "%s  %s", symbol, title);
  ui_label(row, value, &lv_font_montserrat_18, color);
}

static void switch_timer_cb(lv_timer_t *t) {
  (void)t;
  s_switch_timer = nullptr;  // one-shot timer, LVGL deletes it after this call
  if (app_switch_select_ai_voice()) {
    esp_restart();
  }
  lv_label_set_text(s_overlay_label, LV_SYMBOL_WARNING "\nAI Voice image is invalid.\nFlash ai-voice-app.bin again.");
}

static void on_overlay_clicked(lv_event_t *e) {
  (void)e;
  if (s_switch_timer == nullptr) {  // only dismissable after a failed switch
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
  }
}

static void on_open_clicked(lv_event_t *e) {
  (void)e;
  lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(s_overlay_label, LV_SYMBOL_REFRESH "\nStarting AI Voice...");
  // Let LVGL draw the overlay first; verifying the 3 MB image takes a moment
  s_switch_timer = lv_timer_create(switch_timer_cb, 150, nullptr);
  lv_timer_set_repeat_count(s_switch_timer, 1);
}

static void ai_voice_create(lv_obj_t *body, bool restore) {
  (void)restore;
  lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  esp_app_desc_t desc;
  const esp_partition_t *slot = app_switch_find_ai_voice(&desc);

  create_orb(body);
  ui_label(body, "AI Voice", &lv_font_montserrat_32, g_ui.text);
  ui_label(body, "Talk to your watch  -  powered by xiaozhi", &lv_font_montserrat_16, g_ui.muted);

  lv_obj_t *card = ui_card(body);
  if (slot != nullptr) {
    char version[48];
    snprintf(version, sizeof(version), "v%s", desc.version);
    add_info_row(card, LV_SYMBOL_OK, "Firmware", version, g_ui.text);
    add_info_row(card, LV_SYMBOL_DRIVE, "Slot", slot->label, g_ui.text);
  } else {
    add_info_row(card, LV_SYMBOL_WARNING, "Firmware", "Not installed", g_ui.danger);
  }
  add_info_row(card, LV_SYMBOL_LEFT, "Return", "Press PWR", g_ui.text);

  lv_obj_t *open = ui_button(body, LV_SYMBOL_PLAY "  Open AI Voice", on_open_clicked, nullptr);
  lv_obj_set_size(open, lv_pct(100), 60);
  lv_obj_set_style_radius(open, 30, 0);
  lv_obj_set_style_bg_color(open, lv_color_hex(AI_VOICE_COLOR_TOP), 0);
  lv_obj_set_style_bg_grad_color(open, lv_color_hex(AI_VOICE_COLOR_BOTTOM), 0);
  lv_obj_set_style_bg_grad_dir(open, LV_GRAD_DIR_HOR, 0);
  lv_obj_set_style_shadow_width(open, 0, 0);
  if (slot == nullptr) {
    lv_obj_add_state(open, LV_STATE_DISABLED);
  }

  lv_obj_t *note = ui_label(body, "AI Voice keeps its own Wi-Fi setup and updates.", &lv_font_montserrat_14, g_ui.muted);
  lv_obj_set_width(note, lv_pct(100));
  lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);

  // Full-screen overlay shown while switching firmware
  s_overlay = lv_obj_create(lv_obj_get_screen(body));
  lv_obj_remove_style_all(s_overlay);
  lv_obj_set_size(s_overlay, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_overlay, LV_OPA_80, 0);
  lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_overlay, on_overlay_clicked, LV_EVENT_CLICKED, nullptr);
  s_overlay_label = ui_label(s_overlay, "", &lv_font_montserrat_24, lv_color_white());
  lv_obj_set_style_text_align(s_overlay_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(s_overlay_label);
}

static void ai_voice_close(void) {
  if (s_switch_timer != nullptr) {
    lv_timer_delete(s_switch_timer);
    s_switch_timer = nullptr;
  }
  s_overlay = nullptr;
  s_overlay_label = nullptr;
}

const LauncherApp APP_AI_VOICE = {
  "AI Voice",
  LV_SYMBOL_AUDIO,
  AI_VOICE_COLOR_TOP,
  AI_VOICE_COLOR_BOTTOM,
  ai_voice_create,
  ai_voice_close,
  &ai_voice_icon_64,
};
