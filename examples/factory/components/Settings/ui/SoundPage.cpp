#include "SoundPage.hpp"
#include "SettingsUI.hpp"
#include "esp_log.h"
#include "../Settings.hpp"

#define ESP_UTILS_LOG_TAG "BS:SoundPage"
#include "esp_lib_utils.h"

namespace esp_brookesia::apps
{
    SoundPage *SoundPage::_instance = nullptr;

    SoundPage *SoundPage::requestInstance(bool use_status_bar, bool use_navigation_bar)
    {
        if (_instance == nullptr)
        {
            _instance = new SoundPage(use_status_bar, use_navigation_bar);
        }
        return _instance;
    }

    SoundPage::SoundPage(bool use_status_bar, bool use_navigation_bar)
        : App("Sound", nullptr, true, use_status_bar, use_navigation_bar),
          page_root(nullptr), label(nullptr), list1(nullptr)
    {
    }

    SoundPage::~SoundPage()
    {
    }

    bool SoundPage::run()
    {
        ESP_UTILS_LOGD("SoundPage Run");
        lv_obj_clean(lv_scr_act());

        page_root = settings_ui::create_page(lv_scr_act());
        settings_ui::create_header(page_root, "Sound", [](lv_event_t *e) {
            (void)e;
            lv_async_call([](void *param) {
                (void)param;
                Settings::requestInstance()->showRootPage();
            }, nullptr);
        });

        settings_ui::init_list_styles(style_list, style_list_btn, style_list_text, style_list_btn_pressed);
        list1 = settings_ui::create_content_list(page_root);
        lv_obj_add_style(list1, &style_list, LV_PART_MAIN);

        settings_ui::add_section(list1, "Output", style_list_text);

        lv_obj_t *slider_container = lv_obj_create(list1);
        lv_obj_add_style(slider_container, &style_list_btn, LV_PART_MAIN);
        lv_obj_set_size(slider_container, lv_pct(100), settings_ui::CONTROL_PANEL_HEIGHT);
        lv_obj_set_style_pad_all(slider_container, 20, LV_PART_MAIN);
        lv_obj_clear_flag(slider_container, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(slider_container, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = lv_label_create(slider_container);
        lv_label_set_text(title, "Media volume");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN);
        lv_obj_set_style_text_color(title, lv_color_hex(settings_ui::COLOR_PRIMARY_TEXT), LV_PART_MAIN);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

        uint32_t value = bsp_extra_codec_volume_get();
        if (value > 95) {
            value = 95;
        }

        label = lv_label_create(slider_container);
        lv_label_set_text_fmt(label, "%lu%%", static_cast<unsigned long>(value));
        lv_obj_set_style_text_font(label, &lv_font_montserrat_22, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_hex(settings_ui::COLOR_ACCENT), LV_PART_MAIN);
        lv_obj_align(label, LV_ALIGN_TOP_RIGHT, 0, 1);

        lv_obj_t *volume_slider = lv_slider_create(slider_container);
        lv_obj_set_width(volume_slider, lv_pct(100));
        lv_obj_set_height(volume_slider, 14);
        lv_obj_align(volume_slider, LV_ALIGN_BOTTOM_MID, 0, -5);
        lv_slider_set_range(volume_slider, 0, 95);
        lv_slider_set_value(volume_slider, value, LV_ANIM_OFF);

        lv_obj_set_style_bg_color(volume_slider, lv_color_hex(0x505050), LV_PART_MAIN);
        lv_obj_set_style_bg_color(volume_slider, lv_color_hex(settings_ui::COLOR_ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(volume_slider, lv_color_white(), LV_PART_KNOB);
        lv_obj_set_style_pad_all(volume_slider, 7, LV_PART_KNOB);
        lv_obj_set_style_radius(volume_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        lv_obj_add_event_cb(volume_slider, event_handler_cb, LV_EVENT_VALUE_CHANGED, this);
        return true;
    }

    void SoundPage::event_handler_cb(lv_event_t *e)
    {
        SoundPage *instance = static_cast<SoundPage *>(lv_event_get_user_data(e));
        lv_obj_t *slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
        if (instance == nullptr || slider == nullptr || !lv_obj_has_class(slider, &lv_slider_class)) {
            return;
        }

        const uint32_t value = lv_slider_get_value(slider);
        if (instance->label != nullptr) {
            lv_label_set_text_fmt(instance->label, "%lu%%", static_cast<unsigned long>(value));
        }
        bsp_extra_codec_volume_set(value, nullptr);
    }

    bool SoundPage::back()
    {
        ESP_UTILS_LOGD("SoundPage Back");
        Settings::requestInstance()->showRootPage();
        return true;
    }

    bool SoundPage::close()
    {
        ESP_UTILS_LOGD("SoundPage Close");
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
