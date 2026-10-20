#pragma once

#include "esp_brookesia.hpp"
#include "lvgl.h"

namespace esp_brookesia::apps {
    class Settings;
}
namespace esp_brookesia::apps {

class SoundPage : public systems::phone::App {
public:
    static SoundPage *requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);

    SoundPage(bool use_status_bar, bool use_navigation_bar);
    virtual ~SoundPage();

    bool run() override;
    bool back() override;
    bool close() override;

private:
    static void event_handler_cb(lv_event_t *e);
    static SoundPage *_instance;
    lv_obj_t *page_root;
    lv_obj_t *label;
    lv_obj_t *list1;
    lv_style_t style_list;
    lv_style_t style_list_btn;
    lv_style_t style_list_text;
    lv_style_t style_list_btn_pressed;
};

} // namespace esp_brookesia::apps
