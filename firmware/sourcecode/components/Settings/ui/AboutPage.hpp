#pragma once

#include "esp_brookesia.hpp"
#include "lvgl.h"

namespace esp_brookesia::apps {
    class Settings;
}

namespace esp_brookesia::apps {

class AboutPage : public systems::phone::App {
public:
    static AboutPage *requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);

    AboutPage(bool use_status_bar, bool use_navigation_bar);
    virtual ~AboutPage();

    bool run() override;
    bool back() override;
    bool close() override;

private:
    static AboutPage *_instance;
    lv_obj_t *page_root;
    lv_obj_t *label;
    lv_obj_t *list1;
    lv_style_t style_list;
    lv_style_t style_list_btn;
    lv_style_t style_list_text;
    lv_style_t style_list_btn_pressed;
};

} // namespace esp_brookesia::apps
