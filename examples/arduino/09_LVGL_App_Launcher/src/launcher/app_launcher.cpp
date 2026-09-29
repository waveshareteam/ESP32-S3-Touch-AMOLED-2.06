#include "../components/ui_theme.h"
#include "../launcher/watchface.h"
#include "../launcher/app_launcher.h"

#define LAUNCHER_MAX_APPS 8
#define LAUNCHER_MAX_PAGES 6
#define LAUNCHER_INDICATOR_H 48
#define LAUNCHER_ICON_CELL 170   // icon + label cell (square)
#define LAUNCHER_ICON_SIZE 112
#define LAUNCHER_HEADER_H 64

static const LauncherApp *s_apps[LAUNCHER_MAX_APPS];
static int s_app_count = 0;

// Launcher screen objects (equivalent of main_obj / table_obj / indicator_obj / MixObject)
static lv_obj_t *s_screen = nullptr;
static lv_obj_t *s_table = nullptr;
static lv_obj_t *s_indicator = nullptr;
static lv_obj_t *s_pages[LAUNCHER_MAX_PAGES];
static lv_obj_t *s_spots[LAUNCHER_MAX_PAGES];
static int s_page_count = 0;
static int s_current_page = 0;
static bool s_scrolling = false;

// Running app
static int s_active_app = -1;
static lv_obj_t *s_app_screen = nullptr;

static void build_launcher_screen(void);
static void build_app_screen(int id, bool restore, lv_screen_load_anim_t anim);

/* ---------------- Page helpers ---------------- */

static void update_active_spot(void) {
  for (int i = 0; i < s_page_count; i++) {
    bool active = (i == s_current_page);
    lv_obj_set_size(s_spots[i], active ? 36 : 12, 12);
    lv_obj_set_style_bg_color(s_spots[i], active ? g_ui.text : g_ui.muted, 0);
    lv_obj_set_style_bg_opa(s_spots[i], active ? LV_OPA_COVER : LV_OPA_50, 0);
  }
}

bool launcher_scroll_to_page(int index) {
  if (s_table == nullptr || index < 0 || index >= s_page_count) {
    return false;
  }
  if (index == s_current_page) {
    return true;
  }
  // The table is never user-scrollable: only gestures move it (same trick as Brookesia).
  // SCROLLABLE is re-cleared in on_table_scroll_end() once the animation finishes.
  lv_obj_add_flag(s_table, LV_OBJ_FLAG_SCROLLABLE);
  s_scrolling = true;
  lv_obj_scroll_to_view(s_pages[index], LV_ANIM_ON);
  s_current_page = index;
  update_active_spot();
  return true;
}

static void on_table_scroll_end(lv_event_t *e) {
  (void)e;
  lv_obj_clear_flag(s_table, LV_OBJ_FLAG_SCROLLABLE);
  s_scrolling = false;
}

static void on_launcher_gesture(lv_event_t *e) {
  (void)e;
  if (lv_screen_active() != s_screen) {
    return;
  }
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
  if (dir == LV_DIR_LEFT) {
    launcher_scroll_to_page(s_current_page + 1);
  } else if (dir == LV_DIR_RIGHT) {
    launcher_scroll_to_page(s_current_page - 1);
  }
}

static void on_icon_clicked(lv_event_t *e) {
  if (s_scrolling) {
    return;  // avoid launching an app from a page that is still sliding in
  }
  int id = (int)(intptr_t)lv_event_get_user_data(e);
  launcher_start_app(id);
}

static lv_obj_t *create_page(void) {
  lv_obj_t *page = lv_obj_create(s_table);
  lv_obj_remove_style_all(page);
  lv_obj_set_size(page, lv_pct(100), lv_pct(100));
  lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(page, LV_OBJ_FLAG_EVENT_BUBBLE);

  lv_obj_t *spot = lv_obj_create(s_indicator);
  lv_obj_remove_style_all(spot);
  lv_obj_set_style_radius(spot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(spot, LV_OPA_COVER, 0);

  s_pages[s_page_count] = page;
  s_spots[s_page_count] = spot;
  s_page_count++;
  return page;
}

static void create_icon(lv_obj_t *page, int id) {
  const LauncherApp *app = s_apps[id];

  lv_obj_t *cell = lv_obj_create(page);
  lv_obj_remove_style_all(cell);
  lv_obj_set_size(cell, LAUNCHER_ICON_CELL, LAUNCHER_ICON_CELL);
  lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(cell, 10, 0);
  lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(cell, LV_OBJ_FLAG_EVENT_BUBBLE);

  lv_obj_t *icon = lv_button_create(cell);
  lv_obj_set_size(icon, LAUNCHER_ICON_SIZE, LAUNCHER_ICON_SIZE);
  lv_obj_set_style_radius(icon, 32, 0);
  lv_obj_set_style_bg_color(icon, lv_color_hex(app->color), 0);
  lv_obj_set_style_bg_grad_color(icon, lv_color_hex(app->color_end), 0);
  lv_obj_set_style_bg_grad_dir(icon, LV_GRAD_DIR_VER, 0);
  // Thin light rim instead of a drop shadow (shadows need large buffers from the 64 KB LVGL heap)
  lv_obj_set_style_border_width(icon, 2, 0);
  lv_obj_set_style_border_color(icon, lv_color_white(), 0);
  lv_obj_set_style_border_opa(icon, LV_OPA_20, 0);
  lv_obj_set_style_shadow_width(icon, 0, 0);
  // Pressed feedback: shrink like AppLauncherIcon (default_size -> press_size).
  // Size styles instead of transform_scale, which would need a large ARGB layer.
  lv_obj_set_style_width(icon, LAUNCHER_ICON_SIZE - 10, LV_STATE_PRESSED);
  lv_obj_set_style_height(icon, LAUNCHER_ICON_SIZE - 10, LV_STATE_PRESSED);
  lv_obj_add_event_cb(icon, on_icon_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)id);

  if (app->image != nullptr) {
    lv_obj_t *img = lv_image_create(icon);
    lv_image_set_src(img, app->image);
    lv_obj_center(img);
  } else {
    lv_obj_t *symbol = lv_label_create(icon);
    lv_label_set_text(symbol, app->symbol);
    lv_obj_set_style_text_font(symbol, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(symbol, lv_color_white(), 0);
    lv_obj_center(symbol);
  }

  ui_label(cell, app->name, &lv_font_montserrat_20, g_ui.text);
}

/* ---------------- Launcher screen ---------------- */

static void build_launcher_screen(void) {
  int32_t width = lv_display_get_horizontal_resolution(NULL);
  int32_t height = lv_display_get_vertical_resolution(NULL);
  int32_t table_h = height - LAUNCHER_INDICATOR_H;

  s_screen = ui_screen_create();
  lv_obj_add_event_cb(s_screen, on_launcher_gesture, LV_EVENT_GESTURE, nullptr);

  s_table = lv_obj_create(s_screen);
  lv_obj_remove_style_all(s_table);
  lv_obj_set_size(s_table, width, table_h);
  lv_obj_align(s_table, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_flex_flow(s_table, LV_FLEX_FLOW_ROW);
  lv_obj_set_scrollbar_mode(s_table, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_scroll_snap_x(s_table, LV_SCROLL_SNAP_CENTER);
  lv_obj_clear_flag(s_table, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(s_table, on_table_scroll_end, LV_EVENT_SCROLL_END, nullptr);

  s_indicator = lv_obj_create(s_screen);
  lv_obj_remove_style_all(s_indicator);
  lv_obj_set_size(s_indicator, width, LAUNCHER_INDICATOR_H);
  lv_obj_align(s_indicator, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_flex_flow(s_indicator, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(s_indicator, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(s_indicator, 10, 0);

  s_page_count = 0;

  // Page 0: watch face
  watchface_create(create_page());

  // Icon pages: same capacity math as AppLauncher::updateByNewData()
  int cols = width / LAUNCHER_ICON_CELL;
  int rows = table_h / LAUNCHER_ICON_CELL;
  if (cols < 1) cols = 1;
  if (rows < 1) rows = 1;
  int per_page = cols * rows;
  int pad_col = (width - cols * LAUNCHER_ICON_CELL) / (cols + 1);
  int pad_row = (table_h - rows * LAUNCHER_ICON_CELL) / (rows + 1);

  lv_obj_t *page = nullptr;
  for (int id = 0; id < s_app_count || page == nullptr; id++) {
    if (page == nullptr || (id % per_page) == 0) {
      if (s_page_count >= LAUNCHER_MAX_PAGES) {
        break;
      }
      page = create_page();
      lv_obj_set_flex_flow(page, LV_FLEX_FLOW_ROW_WRAP);
      lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
      lv_obj_set_style_pad_hor(page, pad_col, 0);
      lv_obj_set_style_pad_column(page, pad_col, 0);
      lv_obj_set_style_pad_ver(page, pad_row, 0);
      lv_obj_set_style_pad_row(page, pad_row, 0);
    }
    if (id < s_app_count) {
      create_icon(page, id);
    }
  }

  if (s_current_page >= s_page_count) {
    s_current_page = 0;
  }
  lv_obj_update_layout(s_screen);
  lv_obj_add_flag(s_table, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_scroll_to_view(s_pages[s_current_page], LV_ANIM_OFF);
  lv_obj_clear_flag(s_table, LV_OBJ_FLAG_SCROLLABLE);
  s_scrolling = false;
  update_active_spot();
}

bool launcher_begin(void) {
  build_launcher_screen();
  lv_screen_load(s_screen);
  return true;
}

int launcher_install(const LauncherApp *app) {
  if (app == nullptr || s_app_count >= LAUNCHER_MAX_APPS) {
    return -1;
  }
  s_apps[s_app_count] = app;
  s_app_count++;
  if (s_screen != nullptr) {
    launcher_rebuild();
  }
  return s_app_count - 1;
}

/* ---------------- App screens ---------------- */

static void on_back_clicked(lv_event_t *e) {
  (void)e;
  launcher_close_app();
}

static void on_app_gesture(lv_event_t *e) {
  (void)e;
  if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT) {
    launcher_close_app();
  }
}

static void build_app_screen(int id, bool restore, lv_screen_load_anim_t anim) {
  const LauncherApp *app = s_apps[id];
  int32_t height = lv_display_get_vertical_resolution(NULL);

  lv_obj_t *scr = ui_screen_create();
  lv_obj_add_event_cb(scr, on_app_gesture, LV_EVENT_GESTURE, nullptr);

  lv_obj_t *header = lv_obj_create(scr);
  lv_obj_remove_style_all(header);
  lv_obj_set_size(header, lv_pct(100), LAUNCHER_HEADER_H);
  lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_pad_hor(header, 16, 0);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(header, 12, 0);
  lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *back = lv_button_create(header);
  lv_obj_set_size(back, 48, 48);
  lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(back, g_ui.card, 0);
  lv_obj_set_style_shadow_width(back, 0, 0);
  lv_obj_add_event_cb(back, on_back_clicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *back_icon = ui_label(back, LV_SYMBOL_LEFT, &lv_font_montserrat_20, g_ui.text);
  lv_obj_center(back_icon);

  ui_label(header, app->name, &lv_font_montserrat_26, g_ui.text);

  lv_obj_t *body = lv_obj_create(scr);
  lv_obj_remove_style_all(body);
  lv_obj_set_size(body, lv_pct(100), height - LAUNCHER_HEADER_H);
  lv_obj_align(body, LV_ALIGN_TOP_MID, 0, LAUNCHER_HEADER_H);
  lv_obj_set_style_pad_hor(body, 16, 0);
  lv_obj_set_style_pad_bottom(body, 24, 0);
  lv_obj_set_style_pad_row(body, 12, 0);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(body, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t *old = s_app_screen;
  s_app_screen = scr;
  s_active_app = id;

  if (app->on_create) {
    app->on_create(body, restore);
  }

  if (anim == LV_SCR_LOAD_ANIM_NONE) {
    lv_screen_load(scr);
  } else {
    lv_screen_load_anim(scr, anim, 200, 0, false);
  }
  if (old != nullptr) {
    lv_obj_delete_async(old);
  }
}

bool launcher_start_app(int id) {
  if (id < 0 || id >= s_app_count || s_active_app >= 0) {
    return false;
  }
  build_app_screen(id, false, LV_SCR_LOAD_ANIM_MOVE_LEFT);
  return true;
}

void launcher_close_app(void) {
  if (s_active_app < 0 || s_app_screen == nullptr) {
    return;
  }
  const LauncherApp *app = s_apps[s_active_app];
  if (app->on_close) {
    app->on_close();
  }
  s_active_app = -1;
  s_app_screen = nullptr;
  // auto_del=true: the app screen is deleted once the animation finishes
  lv_screen_load_anim(s_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, true);
}

bool launcher_go_home(void) {
  bool changed = false;
  if (s_active_app >= 0) {
    launcher_close_app();
    changed = true;
  }
  if (s_current_page != 0) {
    launcher_scroll_to_page(0);
    changed = true;
  }
  return changed;
}

void launcher_rebuild(void) {
  lv_obj_t *old_launcher = s_screen;
  bool launcher_visible = (lv_screen_active() == old_launcher);

  watchface_destroy();
  build_launcher_screen();

  if (s_active_app >= 0) {
    // Rebuild the open app so it picks up the new theme as well
    const LauncherApp *app = s_apps[s_active_app];
    if (app->on_close) {
      app->on_close();
    }
    build_app_screen(s_active_app, true, LV_SCR_LOAD_ANIM_NONE);
  } else if (launcher_visible) {
    lv_screen_load(s_screen);
  }
  if (old_launcher != nullptr) {
    lv_obj_delete_async(old_launcher);
  }
}
