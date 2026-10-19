/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "Settings.hpp"
#include "ui/SettingsUI.hpp"
#include "lvgl.h"
#include "esp_brookesia.hpp"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#ifdef ESP_UTILS_LOG_TAG
#undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:Settings"
#include "esp_lib_utils.h"

// Settings page icons.
LV_IMG_DECLARE(WIFI);
LV_IMG_DECLARE(sound);
LV_IMG_DECLARE(info);
LV_IMG_DECLARE(BLE);
LV_IMG_DECLARE(backlights);
LV_IMG_DECLARE(battery);
LV_IMG_DECLARE(arrow);
LV_IMG_DECLARE(img_app_settings);

namespace esp_brookesia::apps
{
    static bool s_time_sync_started = false;
    static bool s_time_synced = false;

    static void time_sync_notification_cb(struct timeval *tv)
    {
        (void)tv;
        setenv("TZ", "CST-8", 1);
        tzset();
        s_time_synced = true;
        ESP_UTILS_LOGI("SNTP time synced, timezone CST-8 applied");
    }

    static void start_time_sync_once()
    {
        if (s_time_sync_started || esp_sntp_enabled()) {
            return;
        }

        // Start SNTP only after the network stack has obtained an address.
        // This avoids retries while Wi-Fi is still connecting.
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
        esp_sntp_init();
        s_time_sync_started = true;
        ESP_UTILS_LOGI("SNTP time sync started");
    }

    static void wifi_time_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
    {
        (void)arg;
        (void)event_data;

        if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
            start_time_sync_once();
        }
    }

    Settings *Settings::_instance = nullptr;

    Settings *Settings::requestInstance(bool use_status_bar, bool use_navigation_bar)
    {
        if (_instance == nullptr)
        {
            _instance = new Settings(use_status_bar, use_navigation_bar);
        }
        return _instance;
    }

    Settings::Settings(bool use_status_bar, bool use_navigation_bar)
        : App("Settings", &img_app_settings, true, use_status_bar, use_navigation_bar),
          page_root(nullptr), list1(nullptr), active_page(ActivePage::None)
    {
    }

    Settings::~Settings()
    {
    }

    void Settings::create_settings_ui()
    {
        destroy_settings_ui();
        lv_obj_clean(lv_scr_act());

        page_root = settings_ui::create_page(lv_scr_act());
        settings_ui::create_header(page_root, "Settings");
        settings_ui::init_list_styles(style_list, style_list_btn, style_list_text, style_list_btn_pressed);

        list1 = settings_ui::create_content_list(page_root);
        lv_obj_add_style(list1, &style_list, LV_PART_MAIN);

        settings_ui::add_section(list1, "Wireless", style_list_text);
        add_button_with_arrow(&WIFI, "WLAN");

        settings_ui::add_section(list1, "Media", style_list_text);
        add_button_with_arrow(&sound, "Sound");
        add_button_with_arrow(&backlights, "Display");

        settings_ui::add_section(list1, "System", style_list_text);
        add_button_with_arrow(&battery, "Power");
        add_button_with_arrow(LV_SYMBOL_SD_CARD, "Storage");

        settings_ui::add_section(list1, "More", style_list_text);
        add_button_with_arrow(&info, "About");
    }

    void Settings::destroy_settings_ui()
    {
        if (page_root != nullptr)
        {
            lv_obj_del(page_root);
            page_root = nullptr;
            list1 = nullptr;
            settings_ui::reset_list_styles(style_list, style_list_btn, style_list_text, style_list_btn_pressed);
        }
    }

    void Settings::add_button_with_arrow(const void *icon, const char *text)
    {
        lv_obj_t *btn = lv_list_add_button(list1, icon, text);
        // Keep navigation independent of the label and the child hit by touch.
        lv_obj_set_user_data(btn, const_cast<char *>(text));
        lv_obj_add_style(btn, &style_list_btn, LV_PART_MAIN);
        lv_obj_add_style(btn, &style_list_btn_pressed, LV_STATE_PRESSED);
        settings_ui::use_ellipsis_for_button_label(btn);
        lv_obj_add_event_cb(btn, event_handler_cb, LV_EVENT_CLICKED, this);

        lv_obj_t *arrow_img = lv_image_create(btn);
        lv_image_set_src(arrow_img, &arrow);
        for (uint32_t i = 0; i < lv_obj_get_child_count(btn); ++i) {
            lv_obj_remove_flag(lv_obj_get_child(btn, i), LV_OBJ_FLAG_CLICKABLE);
        }
    }

    void Settings::event_handler_cb(lv_event_t *e)
    {
        Settings *instance = static_cast<Settings *>(lv_event_get_user_data(e));
        if (instance != nullptr)
        {
            instance->event_handler(e);
        }
    }

    void Settings::open_page_async_cb(void *user_data)
    {
        const char *page_name = static_cast<const char *>(user_data);
        Settings *instance = Settings::requestInstance();
        if (instance != nullptr && page_name != nullptr &&
                instance->page_root != nullptr && instance->active_page == ActivePage::None)
        {
            instance->open_page(page_name);
        }
    }

    void Settings::event_handler(lv_event_t *e)
    {
        if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
            return;
        }

        if (page_root == nullptr || active_page != ActivePage::None) {
            return;
        }
        lv_obj_t *obj = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
        const char *txt = static_cast<const char *>(lv_obj_get_user_data(obj));
        if (txt == nullptr) {
            return;
        }
        ESP_UTILS_LOGI("Clicked: %s", txt);

        if (strcmp(txt, "WLAN") == 0)
        {
            lv_async_call(open_page_async_cb, (void *)"WLAN");
        }
        else if (strcmp(txt, "Sound") == 0)
        {
            lv_async_call(open_page_async_cb, (void *)"Sound");
        }
        else if (strcmp(txt, "Display") == 0)
        {
            lv_async_call(open_page_async_cb, (void *)"Display");
        }
        else if (strcmp(txt, "Power") == 0)
        {
            lv_async_call(open_page_async_cb, (void *)"Power");
        }
        else if (strcmp(txt, "Storage") == 0)
        {
            lv_async_call(open_page_async_cb, (void *)"Storage");
        }
        else if (strcmp(txt, "About") == 0)
        {
            lv_async_call(open_page_async_cb, (void *)"About");
        }
    }

    void Settings::open_page(const char *page_name)
    {
        destroy_settings_ui();

        if (strcmp(page_name, "WLAN") == 0)
        {
            active_page = ActivePage::Wlan;
            if (!WlanPage::requestInstance(false, false)->run()) {
                ESP_UTILS_LOGE("Open WLAN page failed");
                showRootPage();
            }
        }
        else if (strcmp(page_name, "Sound") == 0)
        {
            active_page = ActivePage::Sound;
            SoundPage::requestInstance(false, false)->run();
        }
        else if (strcmp(page_name, "Display") == 0)
        {
            active_page = ActivePage::Display;
            DisplayPage::requestInstance(false, false)->run();
        }
        else if (strcmp(page_name, "Power") == 0)
        {
            active_page = ActivePage::Power;
            PowerPage::requestInstance(false, false)->run();
        }
        else if (strcmp(page_name, "Storage") == 0)
        {
            active_page = ActivePage::Storage;
            StoragePage::requestInstance(false, false)->run();
        }
        else if (strcmp(page_name, "About") == 0)
        {
            active_page = ActivePage::About;
            AboutPage::requestInstance(false, false)->run();
        }
    }

    void Settings::close_active_page()
    {
        switch (active_page)
        {
        case ActivePage::Wlan:
            WlanPage::requestInstance(false, false)->close();
            break;
        case ActivePage::Sound:
            SoundPage::requestInstance(false, false)->close();
            break;
        case ActivePage::Display:
            DisplayPage::requestInstance(false, false)->close();
            break;
        case ActivePage::Power:
            PowerPage::requestInstance(false, false)->close();
            break;
        case ActivePage::Storage:
            StoragePage::requestInstance(false, false)->close();
            break;
        case ActivePage::About:
            AboutPage::requestInstance(false, false)->close();
            break;
        case ActivePage::None:
        default:
            break;
        }
        active_page = ActivePage::None;
    }

    void Settings::showRootPage()
    {
        close_active_page();
        create_settings_ui();
    }

    bool Settings::run(void)
    {
        ESP_UTILS_LOGD("Run");
        active_page = ActivePage::None;
        if (page_root == nullptr)
        {
            create_settings_ui();
        }
        return true;
    }

    bool Settings::back(void)
    {
        ESP_UTILS_LOGD("Back");
        if (active_page != ActivePage::None)
        {
            showRootPage();
            return true;
        }
        return notifyCoreClosed();
    }

    bool Settings::close(void)
    {
        ESP_UTILS_LOGD("Close");
        close_active_page();
        destroy_settings_ui();
        return true;
    }

    bool Settings::init()
    {
        ESP_UTILS_LOGD("Init");
        return initWifi() == ESP_OK;
    }

    bool Settings::deinit()
    {
        ESP_UTILS_LOGD("Deinit");
        return true;
    }

    bool Settings::pause()
    {
        ESP_UTILS_LOGD("Pause");
        return true;
    }

    bool Settings::resume()
    {
        ESP_UTILS_LOGD("Resume");
        return true;
    }

    esp_err_t Settings::initWifi()
    {
        static bool netif_initialized = false;
        static bool event_loop_initialized = false;
        static bool wifi_initialized = false;
        static bool time_event_handler_registered = false;
        static esp_netif_t *sta_netif = nullptr;

        if (!netif_initialized) {
            esp_err_t ret = esp_netif_init();
            if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
                ESP_UTILS_LOGE("esp_netif_init failed: %s", esp_err_to_name(ret));
                return ret;
            }
            netif_initialized = true;
        }

        if (!event_loop_initialized) {
            esp_err_t ret = esp_event_loop_create_default();
            if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
                ESP_UTILS_LOGE("esp_event_loop_create_default failed: %s", esp_err_to_name(ret));
                return ret;
            }
            event_loop_initialized = true;
        }

        if (!sta_netif) {
            sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (!sta_netif) {
                sta_netif = esp_netif_create_default_wifi_sta();
            }
            if (!sta_netif) {
                ESP_UTILS_LOGE("esp_netif_create_default_wifi_sta failed");
                return ESP_FAIL;
            }
        }

        if (!wifi_initialized) {
            wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
            esp_err_t ret = esp_wifi_init(&cfg);
            if (ret != ESP_OK && ret != ESP_ERR_WIFI_INIT_STATE) {
                ESP_UTILS_LOGE("esp_wifi_init failed: %s", esp_err_to_name(ret));
                return ret;
            }
            wifi_initialized = true;
        }

        if (!time_event_handler_registered) {
            esp_err_t ret = esp_event_handler_register(
                IP_EVENT,
                IP_EVENT_STA_GOT_IP,
                &wifi_time_event_handler,
                nullptr);
            if (ret != ESP_OK) {
                ESP_UTILS_LOGE("Register time sync event handler failed: %s", esp_err_to_name(ret));
                return ret;
            }
            time_event_handler_registered = true;
        }

        esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_STA);
        if (ret != ESP_OK) {
            ESP_UTILS_LOGW("esp_wifi_set_mode returned: %s", esp_err_to_name(ret));
        }

        return ESP_OK;
    }
} // namespace esp_brookesia::apps
