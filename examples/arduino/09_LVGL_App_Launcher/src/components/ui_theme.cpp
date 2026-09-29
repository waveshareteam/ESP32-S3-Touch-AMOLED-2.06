#include "../components/ui_theme.h"

UiPalette g_ui;

void ui_theme_apply(bool dark) {
  if (dark) {
    g_ui.bg = lv_color_hex(0x000000);  // true black saves power on AMOLED
    g_ui.card = lv_color_hex(0x1C1C1E);
    g_ui.text = lv_color_hex(0xFFFFFF);
    g_ui.muted = lv_color_hex(0x8E8E93);
  } else {
    g_ui.bg = lv_color_hex(0xF2F2F7);
    g_ui.card = lv_color_hex(0xFFFFFF);
    g_ui.text = lv_color_hex(0x000000);
    g_ui.muted = lv_color_hex(0x6C6C70);
  }
  g_ui.accent = lv_color_hex(0x0A84FF);
  g_ui.danger = lv_color_hex(0xFF453A);

  lv_display_t *disp = lv_display_get_default();
  lv_theme_t *theme = lv_theme_default_init(disp, g_ui.accent, lv_palette_main(LV_PALETTE_GREEN), dark,
                                            &lv_font_montserrat_20);
  lv_display_set_theme(disp, theme);
}

lv_obj_t *ui_screen_create(void) {
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, g_ui.bg, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  return scr;
}

lv_obj_t *ui_card(lv_obj_t *parent) {
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_set_width(card, lv_pct(100));
  lv_obj_set_height(card, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(card, g_ui.card, 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_radius(card, 18, 0);
  lv_obj_set_style_pad_all(card, 16, 0);
  lv_obj_set_style_pad_row(card, 12, 0);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  return card;
}

lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color) {
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, color, 0);
  return label;
}

lv_obj_t *ui_row(lv_obj_t *parent) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  return row;
}

lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user_data) {
  lv_obj_t *btn = lv_button_create(parent);
  lv_obj_set_height(btn, 52);
  lv_obj_set_style_radius(btn, 26, 0);
  lv_obj_t *label = lv_label_create(btn);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  if (cb != nullptr) {
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
  }
  return btn;
}

void ui_no_gesture(lv_obj_t *obj) {
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
}
