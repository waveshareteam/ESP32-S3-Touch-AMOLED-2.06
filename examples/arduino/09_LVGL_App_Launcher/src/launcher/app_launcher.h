#pragma once

#include <lvgl.h>

// Lightweight launcher modelled on ESP-Brookesia's phone AppLauncher:
//  - page 0 is the watch face, the following pages hold app icons
//  - horizontal swipes page through the table, dots show the active page
//  - tapping an icon creates the app screen; closing the app deletes it (frees LVGL memory)

struct LauncherApp {
  const char *name;
  const char *symbol;  // LV_SYMBOL_* shown on the icon
  uint32_t color;      // icon background color (gradient top)
  uint32_t color_end;  // gradient bottom, same as color for a flat icon
  // Build the app UI inside body. restore=true when the screen is rebuilt (e.g. theme change)
  void (*on_create)(lv_obj_t *body, bool restore);
  // Release timers/resources before the app screen is deleted
  void (*on_close)(void);
  // Optional image shown instead of symbol (white-on-transparent, <= 64 px); nullptr = use symbol
  const lv_image_dsc_t *image;
};

bool launcher_begin(void);
int launcher_install(const LauncherApp *app);  // returns app id, -1 on failure
bool launcher_start_app(int id);
void launcher_close_app(void);
bool launcher_scroll_to_page(int index);
// Close any open app and show the watch face page. Returns false if it was already showing.
bool launcher_go_home(void);

// Recreate every screen with the current theme (keeps the page / open app)
void launcher_rebuild(void);
