#pragma once

#include <lvgl.h>

struct UiPalette {
  lv_color_t bg;
  lv_color_t card;
  lv_color_t text;
  lv_color_t muted;
  lv_color_t accent;
  lv_color_t danger;
};

extern UiPalette g_ui;

// Switches palette + LVGL default theme. Objects created afterwards use the new theme;
// existing screens must be rebuilt by their owner.
void ui_theme_apply(bool dark);

lv_obj_t *ui_screen_create(void);
lv_obj_t *ui_card(lv_obj_t *parent);
lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color);
lv_obj_t *ui_row(lv_obj_t *parent);
lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user_data);

// Stop swipe gestures on this widget from bubbling to the screen (sliders, keyboards...)
void ui_no_gesture(lv_obj_t *obj);
