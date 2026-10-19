#include "AboutPage.hpp"
#include "SettingsUI.hpp"
#include "esp_log.h"
#include "../Settings.hpp"

#define ESP_UTILS_LOG_TAG "BS:AboutPage"
#include "esp_lib_utils.h"

LV_IMG_DECLARE(logo);

namespace esp_brookesia::apps
{
    AboutPage *AboutPage::_instance = nullptr;

    AboutPage *AboutPage::requestInstance(bool use_status_bar, bool use_navigation_bar)
    {
        if (_instance == nullptr)
        {
            _instance = new AboutPage(use_status_bar, use_navigation_bar);
        }
        return _instance;
    }

    AboutPage::AboutPage(bool use_status_bar, bool use_navigation_bar)
        : App("About", nullptr, true, use_status_bar, use_navigation_bar),
          page_root(nullptr), label(nullptr), list1(nullptr)
    {
    }

    AboutPage::~AboutPage()
    {
    }

    bool AboutPage::run()
    {
        ESP_UTILS_LOGD("AboutPage Run");
        lv_obj_clean(lv_scr_act());

        page_root = settings_ui::create_page(lv_scr_act());
        settings_ui::create_header(page_root, "About", [](lv_event_t *e) {
            (void)e;
            lv_async_call([](void *param) {
                (void)param;
                Settings::requestInstance()->showRootPage();
            }, nullptr);
        });

        settings_ui::init_list_styles(style_list, style_list_btn, style_list_text, style_list_btn_pressed);
        list1 = settings_ui::create_content_list(page_root);
        lv_obj_add_style(list1, &style_list, LV_PART_MAIN);

        settings_ui::add_section(list1, "Device", style_list_text);
        settings_ui::add_info_row(list1, &logo, "Manufacturer", "Waveshare", style_list_btn);
        settings_ui::add_info_row(
            list1, nullptr, "Product", "ESP32-S3-Touch-AMOLED-2.06", style_list_btn);

        settings_ui::add_section(list1, "Software", style_list_text);
        settings_ui::add_info_row(list1, nullptr, "UI Framework", "ESP-Brookesia", style_list_btn);
        return true;
    }

    bool AboutPage::back()
    {
        ESP_UTILS_LOGD("AboutPage Back");
        Settings::requestInstance()->showRootPage();
        return true;
    }

    bool AboutPage::close()
    {
        ESP_UTILS_LOGD("AboutPage Close");
        if (page_root != nullptr)
        {
            lv_obj_del(page_root);
            page_root = nullptr;
            label = nullptr;
            list1 = nullptr;
            settings_ui::reset_list_styles(style_list, style_list_btn, style_list_text, style_list_btn_pressed);
        }
        return true;
    }

} // namespace esp_brookesia::apps
