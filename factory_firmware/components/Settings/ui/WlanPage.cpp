#include "WlanPage.hpp"
#include "../Settings.hpp"
#include "SettingsUI.hpp"
#include "system_status.hpp"

#define ESP_UTILS_LOG_TAG "BS:WlanPage"
#include "esp_lib_utils.h"
#include "nvs.h"

#include <stdlib.h>

LV_IMG_DECLARE(wifi_1);
LV_IMG_DECLARE(wifi_2);
LV_IMG_DECLARE(wifi_3);
LV_IMG_DECLARE(wifi_4);

namespace esp_brookesia::apps
{
    static void copy_bounded_ssid(char *destination, size_t destination_size,
                                  const uint8_t *source, size_t source_size)
    {
        if (!destination || destination_size == 0) {
            return;
        }
        const size_t source_length = source ?
                                     strnlen(reinterpret_cast<const char *>(source), source_size) : 0;
        const size_t copy_length = source_length < destination_size - 1 ?
                                   source_length : destination_size - 1;
        if (copy_length > 0) {
            memcpy(destination, source, copy_length);
        }
        destination[copy_length] = '\0';
    }

    WlanPage *WlanPage::_instance = nullptr;

    lv_obj_t *WlanPage::ta = nullptr;
    lv_obj_t *WlanPage::kb = nullptr;

    WlanPage *WlanPage::requestInstance(bool use_status_bar, bool use_navigation_bar)
    {
        if (_instance == nullptr)
        {
            _instance = new WlanPage(use_status_bar, use_navigation_bar);
        }
        return _instance;
    }

    WlanPage::WlanPage(bool use_status_bar, bool use_navigation_bar)
        : App("WLAN", nullptr, true, use_status_bar, use_navigation_bar),
          page_root(nullptr), label(nullptr), list1(nullptr),
          connected_text(nullptr), conn_btn(nullptr), available_text(nullptr)
    {
        status_btn = nullptr;
        wlan_switch = nullptr;
        wifi_icon = nullptr;
        spinner = nullptr;
        password_title = nullptr;
        wifi_TaskHandle = nullptr;
        wifi_index = 0;
    }

    WlanPage::~WlanPage() {}

    bool WlanPage::run()
    {
        ESP_UTILS_LOGI("WlanPage Run");
        // Keep background producers gated until every new page object and
        // style has been constructed.
        page_active = false;
        const uint32_t generation = page_generation.fetch_add(1) + 1;
        portENTER_CRITICAL(&sta_snapshot_mux);
        sta_config_snapshot = {};
        sta_config_snapshot_valid = false;
        sta_config_snapshot_generation = generation;
        portEXIT_CRITICAL(&sta_snapshot_mux);

        brookesia::system_status::Snapshot system_snapshot = {};
        if (brookesia::system_status::get_snapshot(system_snapshot)) {
            wifi_enabled = system_snapshot.wifi_enabled;
            _nvs_param_map[NVS_KEY_WIFI_ENABLE] = system_snapshot.wifi_enabled;
        } else {
            _nvs_param_map[NVS_KEY_WIFI_ENABLE] = false;
            loadNvsParam();
            wifi_enabled = _nvs_param_map[NVS_KEY_WIFI_ENABLE];
        }

        start_wifi_events();

        CreateWifiUI();

        // Reflect the resident service state without making a potentially
        // blocking Wi-Fi driver call from the LVGL worker task.
        if (wifi_enabled) {
            Wifi_state = system_snapshot.wifi_connected ? CONNECTED : WIFIOPEN;
            lv_obj_add_state(wlan_switch, LV_STATE_CHECKED);
            lv_obj_add_state(wlan_switch, LV_STATE_DISABLED);
            lv_obj_remove_flag(spinner, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(list1, LV_OBJ_FLAG_HIDDEN);
        } else {
            Wifi_state = WIFICLOSE;
            lv_obj_remove_state(wlan_switch, LV_STATE_CHECKED);
            lv_obj_remove_state(wlan_switch, LV_STATE_DISABLED);
            lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(list1, LV_OBJ_FLAG_HIDDEN);
        }

        // The background task owns blocking scan/connect calls and posts UI work
        // back to LVGL with lv_async_call.
        if (wifi_TaskHandle == nullptr) {
            wifi_task_running = true;
            BaseType_t task_ret = xTaskCreate(wifi_task, "wifi_task", 6 * 1024, this, 9, &wifi_TaskHandle);
            if (task_ret != pdPASS) {
                wifi_task_running = false;
                wifi_TaskHandle = nullptr;
                ESP_UTILS_LOGE("Create Wi-Fi task failed");
                return false;
            }
        }

        // A worker can survive page close, so explicitly wake it when the page
        // is reopened and a fresh scan is required.
        scan_requested = wifi_enabled.load();
        page_active = true;
        xTaskNotifyGive(wifi_TaskHandle);

        return true;
    }

    bool WlanPage::back()
    {
        ESP_UTILS_LOGI("WlanPage Back");

        Settings::requestInstance()->showRootPage();
        return true;
    }

    bool WlanPage::close()
    {
        ESP_UTILS_LOGI("WlanPage Close");
        page_active = false;
        scan_requested = false;
        page_generation.fetch_add(1);

        // Never wait for the scan task while the LVGL worker owns the GUI lock.
        // The WlanPage singleton outlives its page objects; the resident worker
        // checks page_active before posting any UI callback and is reused on the
        // next visit.
        if (wifi_TaskHandle) {
            xTaskNotifyGive(wifi_TaskHandle);
        }

        if(wifi_events_registered)
            stop_wifi_events();
        if (page_root != nullptr)
        {
            lv_obj_del(page_root);
            page_root = nullptr;
            label = nullptr;
            list1 = nullptr;
            connected_text = nullptr;
            conn_btn = nullptr;
            available_text = nullptr;
            status_btn = nullptr;
            wlan_switch = nullptr;
            wifi_icon = nullptr;
            spinner = nullptr;
            password_title = nullptr;
            ta = nullptr;
            kb = nullptr;
            wifi_btns.clear();
            settings_ui::reset_list_styles(style_list, style_list_btn, style_list_text, style_list_btn_pressed);
        }

        return true;
    }


    void WlanPage::CreateWifiUI()
    {
        lv_obj_clean(lv_scr_act());

        page_root = settings_ui::create_page(lv_scr_act());
        status_btn = settings_ui::create_header(page_root, "WLAN", [](lv_event_t *e) {
            (void)e;
            WlanPage *page = WlanPage::requestInstance();
            queue_lvgl_async_from_gui([](void *param) {
                (void)param;
                Settings::requestInstance()->showRootPage();
            }, page);
        });

        settings_ui::init_list_styles(style_list, style_list_btn, style_list_text, style_list_btn_pressed);

        const bool round_page = settings_ui::is_round_page(page_root);
        if (round_page) {
            lv_style_set_height(&style_list_btn, 72);
            lv_style_set_pad_hor(&style_list_btn, 14);
            lv_style_set_pad_ver(&style_list_btn, 8);
            lv_style_set_text_font(&style_list_btn, &lv_font_montserrat_22);
            lv_style_set_text_font(&style_list_text, &lv_font_montserrat_18);
            lv_style_set_pad_top(&style_list_text, 10);
        }

        list1 = settings_ui::create_content_list(page_root);
        lv_obj_add_style(list1, &style_list, LV_PART_MAIN);

        settings_ui::add_section(list1, "Network", style_list_text);

        lv_obj_t *wlan_btn = lv_list_add_button(list1, nullptr, "Wi-Fi");
        lv_obj_add_style(wlan_btn, &style_list_btn, LV_PART_MAIN);
        settings_ui::use_ellipsis_for_button_label(wlan_btn);

        wlan_switch = lv_switch_create(wlan_btn);
        lv_obj_set_size(wlan_switch, round_page ? 54 : 64, round_page ? 30 : 36);
        lv_obj_remove_state(wlan_switch, LV_STATE_CHECKED);

        lv_obj_add_event_cb(wlan_switch, [](lv_event_t *e)
        {
            lv_obj_t *sw = static_cast<lv_obj_t *>(lv_event_get_target(e));
            bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
            auto page = WlanPage::requestInstance();

            if (checked) page->toggleWifiUI(WIFIOPEN);
            else page->toggleWifiUI(WIFICLOSE);
        },
        LV_EVENT_VALUE_CHANGED, nullptr);

        lv_obj_add_state(wlan_btn, LV_STATE_DISABLED);
        // lv_obj_add_state(conn_btn, LV_STATE_DISABLED);

        lv_obj_update_layout(page_root);
        const lv_coord_t horizontal_margin = settings_ui::get_horizontal_margin(page_root);
        const lv_coord_t header_bottom = settings_ui::get_header_top(page_root) +
                                         settings_ui::PAGE_HEADER_HEIGHT;
        const lv_coord_t keyboard_bottom = round_page
                                           ? settings_ui::get_bottom_margin(page_root) : 16;
        lv_coord_t content_width = lv_obj_get_width(page_root) - horizontal_margin * 2;
        lv_coord_t keyboard_height = lv_obj_get_height(page_root) * (round_page ? 33 : 48) / 100;
        if (content_width < 1) {
            content_width = 1;
        }
        if (keyboard_height < 1) {
            keyboard_height = 1;
        }

        spinner = lv_spinner_create(page_root);
        lv_obj_set_size(spinner, round_page ? 56 : 72, round_page ? 56 : 72);
        lv_obj_align(spinner, LV_ALIGN_CENTER, 0, round_page ? 8 : 36);
        lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_arc_width(spinner, 4, LV_PART_MAIN);
        lv_obj_set_style_arc_width(spinner, 4, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(spinner, lv_color_hex(settings_ui::COLOR_ACCENT), LV_PART_INDICATOR);

        password_title = lv_label_create(page_root);
        lv_label_set_text(password_title, "Connect to network");
        lv_label_set_long_mode(password_title, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(password_title, content_width);
        lv_obj_set_style_text_font(password_title,
                                   round_page ? &lv_font_montserrat_20 : &lv_font_montserrat_24,
                                   LV_PART_MAIN);
        lv_obj_set_style_text_color(password_title, lv_color_hex(settings_ui::COLOR_PRIMARY_TEXT), LV_PART_MAIN);
        lv_obj_align(password_title, LV_ALIGN_TOP_MID, 0, header_bottom + (round_page ? 12 : 20));
        lv_obj_add_flag(password_title, LV_OBJ_FLAG_HIDDEN);

        ta = lv_textarea_create(page_root);
        lv_obj_add_flag(ta, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(ta, content_width, round_page ? 52 : 64);
        lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, header_bottom + (round_page ? 44 : 64));

        lv_textarea_set_password_mode(ta, true);
        lv_textarea_set_max_length(ta, sizeof(wifi_pwd) - 1);
        lv_textarea_set_password_show_time(ta, 1500);
        lv_textarea_set_placeholder_text(ta, "Enter password...");
        lv_obj_set_style_bg_color(ta, lv_color_hex(settings_ui::COLOR_SURFACE), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(ta, lv_color_hex(settings_ui::COLOR_BORDER), LV_PART_MAIN);
        lv_obj_set_style_border_width(ta, 2, LV_PART_MAIN);
        lv_obj_set_style_radius(ta, 6, LV_PART_MAIN);
        lv_obj_set_style_text_color(ta, lv_color_hex(settings_ui::COLOR_PRIMARY_TEXT), LV_PART_MAIN);
        lv_obj_set_style_text_color(ta, lv_color_hex(settings_ui::COLOR_SECONDARY_TEXT), LV_PART_TEXTAREA_PLACEHOLDER);
        lv_obj_set_style_text_font(ta,
                                   round_page ? &lv_font_montserrat_18 : &lv_font_montserrat_20,
                                   LV_PART_MAIN);
        lv_obj_set_style_text_font(ta,
                                   round_page ? &lv_font_montserrat_18 : &lv_font_montserrat_20,
                                   LV_PART_TEXTAREA_PLACEHOLDER);
        lv_obj_set_style_bg_color(ta, lv_color_white(), LV_PART_CURSOR);
        lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_PART_CURSOR);
        lv_obj_add_event_cb(ta, ta_event_cb, LV_EVENT_ALL, this);

        kb = lv_keyboard_create(page_root);
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(kb, content_width, keyboard_height);
        lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -keyboard_bottom);
        lv_obj_set_style_bg_color(kb, lv_color_hex(0x181818), LV_PART_MAIN);
        lv_obj_set_style_border_color(kb, lv_color_hex(settings_ui::COLOR_BORDER), LV_PART_MAIN);
        lv_obj_set_style_border_width(kb, 2, LV_PART_MAIN);
        lv_obj_set_style_radius(kb, 6, LV_PART_MAIN);
        lv_obj_set_style_bg_color(kb, lv_color_hex(0x333333), LV_PART_ITEMS);
        lv_obj_set_style_text_color(kb, lv_color_hex(settings_ui::COLOR_PRIMARY_TEXT), LV_PART_ITEMS);
        lv_obj_set_style_text_font(kb,
                                   round_page ? &lv_font_montserrat_16 : &lv_font_montserrat_20,
                                   LV_PART_ITEMS);
        lv_obj_set_style_border_color(kb, lv_color_hex(0x555555), LV_PART_ITEMS);
        lv_obj_set_style_border_width(kb, 1, LV_PART_ITEMS);
        lv_obj_set_style_radius(kb, 4, LV_PART_ITEMS);
        lv_keyboard_set_textarea(kb, ta);
        lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_ALL, this);
    }

    bool WlanPage::OpenWifi()
    {
        ESP_UTILS_LOGI("WlanPage OpenWifi");

        if (!wifi_events_registered) start_wifi_events();

        // Driver start/stop and NVS commits can wait on the resident monitor.
        // Queue them to wifi_task so this LVGL event callback always returns.
        wifi_enabled = true;
        _nvs_param_map[NVS_KEY_WIFI_ENABLE] = true;

        if (wlan_switch) {
            lv_obj_add_state(wlan_switch, LV_STATE_CHECKED);
            lv_obj_add_state(wlan_switch, LV_STATE_DISABLED);
        }
        if (spinner) {
            lv_obj_remove_flag(spinner, LV_OBJ_FLAG_HIDDEN);
        }
        if (list1) {
            lv_obj_add_flag(list1, LV_OBJ_FLAG_HIDDEN);
        }

        post_worker_command(WorkerCommand::ENABLE);
        return true;
    }

    bool WlanPage::CloseWifi()
    {
        ESP_UTILS_LOGI("WlanPage CloseWifi");
        Wifi_state = WIFICLOSE;
        wifi_enabled = false;
        _nvs_param_map[NVS_KEY_WIFI_ENABLE] = false;

        if (wlan_switch) {
            lv_obj_remove_state(wlan_switch, LV_STATE_CHECKED);
            lv_obj_remove_state(wlan_switch, LV_STATE_DISABLED);
        }
        if (spinner) {
            lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
        }
        if (list1) {
            lv_obj_remove_flag(list1, LV_OBJ_FLAG_HIDDEN);
        }

        if (connected_text)
            lv_obj_add_flag(connected_text, LV_OBJ_FLAG_HIDDEN);
        if (conn_btn)
            lv_obj_add_flag(conn_btn, LV_OBJ_FLAG_HIDDEN);
        if (available_text)
            lv_obj_add_flag(available_text, LV_OBJ_FLAG_HIDDEN);
        for (auto btn : wifi_btns)
        {
            if (btn)
                lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
        }

        post_worker_command(WorkerCommand::DISABLE);
        return true;
    }

    void WlanPage::toggleWifiUI(WifiState visible)
    {
        if (visible == WIFIOPEN)
        {
            _nvs_param_map[NVS_KEY_WIFI_ENABLE] = true;
            printf("WIFIOPEN \r\n");
            OpenWifi();
        }
        else if(visible == WIFICLOSE) {
            printf("WIFICLOSE \r\n");
            _nvs_param_map[NVS_KEY_WIFI_ENABLE] = false;
            CloseWifi();
        }
    }
    // Runs on the LVGL thread after Wi-Fi state changes.
    void WlanPage::wifi_state_cb(void *param)
    {
        WlanPage *self = static_cast<WlanPage *>(param);
        if (!self || !self->page_active || !self->list1) {
            return;
        }
        if (self->kb) {
            lv_obj_add_flag(self->kb, LV_OBJ_FLAG_HIDDEN);
        }
        if (self->ta) {
            lv_obj_add_flag(self->ta, LV_OBJ_FLAG_HIDDEN);
        }
        if (self->password_title) {
            lv_obj_add_flag(self->password_title, LV_OBJ_FLAG_HIDDEN);
        }
        if (self->spinner) {
            lv_obj_add_flag(self->spinner, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_remove_flag(self->list1, LV_OBJ_FLAG_HIDDEN);
        if (self->wlan_switch) {
            lv_obj_remove_state(self->wlan_switch, LV_STATE_DISABLED);
            if (self->Wifi_state == WIFICLOSE) {
                lv_obj_remove_state(self->wlan_switch, LV_STATE_CHECKED);
            } else {
                lv_obj_add_state(self->wlan_switch, LV_STATE_CHECKED);
            }
        }
        if (self->status_btn) {
            lv_obj_remove_state(self->status_btn, LV_STATE_DISABLED);
        }


        if (!create_session_timer(self->wifi_sta_cb, 100, self)) {
            ESP_UTILS_LOGW("Create Wi-Fi state refresh timer failed");
        }
    }

    // Handles AP list button events and owns the per-button user_data lifetime.
    void WlanPage::wifi_btn_cb(lv_event_t * e)
    {
        lv_event_code_t event_code = lv_event_get_code(e);
        btn_user_data_t *ud = (btn_user_data_t *)lv_event_get_user_data(e);
        if (!ud) {
            return;
        }
        if (event_code == LV_EVENT_DELETE) {
            free(ud);
            return;
        }
        WlanPage *page = static_cast<WlanPage *>(ud->self);
        if (!page || !page->page_active) {
            return;
        }
        // When a network in the list is pressed
        if(event_code == LV_EVENT_SHORT_CLICKED) {
            page->wifi_index = ud->index;

            ESP_UTILS_LOGI("wifi_index %d", page->wifi_index);

            copy_bounded_ssid(page->wifi_ssid, sizeof(page->wifi_ssid),
                              page->ap_info[page->wifi_index].ssid,
                              sizeof(page->ap_info[page->wifi_index].ssid));
            ESP_UTILS_LOGI("Selected SSID: %s", page->wifi_ssid);

            if (page->ap_info[page->wifi_index].authmode == WIFI_AUTH_OPEN) {
                page->wifi_pwd[0] = '\0';
                page->Wifi_state = CONNECTING;
                lv_obj_add_flag(page->kb, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(page->ta, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(page->spinner, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_state(page->wlan_switch, LV_STATE_DISABLED);
                lv_obj_add_flag(page->list1, LV_OBJ_FLAG_HIDDEN);
                return;
            }

            if (page->password_title) {
                lv_label_set_text_fmt(page->password_title, "Connect to %s", page->wifi_ssid);
                lv_obj_remove_flag(page->password_title, LV_OBJ_FLAG_HIDDEN);
            }
            lv_textarea_set_text(ta, "");
            lv_obj_add_flag(page->list1, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ta, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_state(ta, LV_STATE_FOCUSED);
            lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
            lv_keyboard_set_textarea(kb, ta);

        }
    }

    // Rebuilds the connected/available AP list after a background scan completes.
    void WlanPage::wifi_scan_cb(lv_timer_t * timer) {
        WlanPage *self = nullptr;
        uint32_t generation = 0;
        if (!consume_timer_context(timer, &self, &generation) ||
                !self->page_active || self->page_generation.load() != generation ||
                !self->list1) {
            return;
        }

        if (self->connected_text) {
            lv_obj_del(self->connected_text);
            self->connected_text = nullptr;
        }
        if (self->conn_btn) {
            lv_obj_del(self->conn_btn);
            self->conn_btn = nullptr;
            self->wifi_icon = nullptr;
        }
        if (self->available_text) {
            lv_obj_del(self->available_text);
            self->available_text = nullptr;
        }
        for (auto btn : self->wifi_btns) {
            if (btn) {
                lv_obj_del(btn);
            }
        }
        self->wifi_btns.clear();

        // Connected WLAN
        self->connected_text = lv_list_add_text(self->list1, "Connected WLAN");
        lv_obj_add_style(self->connected_text, &self->style_list_text, LV_PART_MAIN);
        lv_obj_add_flag(self->connected_text, LV_OBJ_FLAG_HIDDEN);

        self->conn_btn = lv_list_add_button(self->list1, nullptr, "SSID");
        lv_obj_add_style(self->conn_btn, &self->style_list_btn, LV_PART_MAIN);
        settings_ui::use_ellipsis_for_button_label(self->conn_btn);
        lv_obj_add_flag(self->conn_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(self->conn_btn, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_add_event_cb(self->conn_btn, [](lv_event_t *e)
        {
            lv_event_code_t event_code = lv_event_get_code(e);
            auto page = WlanPage::requestInstance();

            // When a network in the list is pressed
            if(event_code == LV_EVENT_SHORT_CLICKED) {
                ESP_LOGI("WIFI", "LV_EVENT_SHORT_CLICKED");
                page->wifi_enabled = true;
                page->_nvs_param_map[NVS_KEY_WIFI_ENABLE] = true;
                page->Wifi_state = WIFISCAN;
                page->connection_num = 0;
                if (page->connected_text) {
                    lv_obj_add_flag(page->connected_text, LV_OBJ_FLAG_HIDDEN);
                }
                if (page->conn_btn) {
                    lv_obj_add_flag(page->conn_btn, LV_OBJ_FLAG_HIDDEN);
                }
                if (page->spinner) {
                    lv_obj_remove_flag(page->spinner, LV_OBJ_FLAG_HIDDEN);
                }
                if (page->list1) {
                    lv_obj_add_flag(page->list1, LV_OBJ_FLAG_HIDDEN);
                }
                page->post_worker_command(WorkerCommand::FORGET);
            }
        },
        LV_EVENT_SHORT_CLICKED, nullptr);

        self->wifi_icon = lv_label_create(self->conn_btn);
        lv_label_set_text(self->wifi_icon, LV_SYMBOL_CLOSE);
        lv_obj_set_style_text_font(self->wifi_icon, &lv_font_montserrat_26, LV_PART_MAIN);
        // Available WLAN
        self->available_text = lv_list_add_text(self->list1, "Available WLAN");
        lv_obj_add_style(self->available_text, &self->style_list_text, LV_PART_MAIN);

        uint16_t display_count = self->scanned_ap_count;
        if (display_count > DEFAULT_SCAN_LIST_SIZE) {
            display_count = DEFAULT_SCAN_LIST_SIZE;
        }

        for (int i = 0; i < display_count; i++)
        {
            if (self->ap_info[i].ssid[0] == '\0') {
                continue;
            }
            lv_obj_t *wifi_btn = lv_list_add_button(self->list1, nullptr, (const char *)self->ap_info[i].ssid);
            lv_obj_add_style(wifi_btn, &self->style_list_btn, LV_PART_MAIN);
            lv_obj_add_style(wifi_btn, &self->style_list_btn_pressed, LV_STATE_PRESSED);
            settings_ui::use_ellipsis_for_button_label(wifi_btn);

            lv_obj_t *icon = lv_image_create(wifi_btn);
            if (self->ap_info[i].rssi > -25)
                lv_img_set_src(icon, &wifi_4);
            else if ((self->ap_info[i].rssi < -25) && (self->ap_info[i].rssi > -50))  // Medium signal
                // Add button with medium signal icon
               lv_img_set_src(icon, &wifi_3);
            else if ((self->ap_info[i].rssi < -50) && (self->ap_info[i].rssi > -75))  // Medium signal
                // Add button with weak signal icon
                lv_img_set_src(icon, &wifi_2);
            else lv_img_set_src(icon, &wifi_1);

            btn_user_data_t *ud = (btn_user_data_t *)malloc(sizeof(btn_user_data_t));
            if (!ud) {
                ESP_UTILS_LOGW("Allocate Wi-Fi button user data failed");
                lv_obj_del(wifi_btn);
                continue;
            }

            ud->index = i;
            ud->self = self;
            lv_obj_add_event_cb(wifi_btn,wifi_btn_cb, LV_EVENT_ALL, ud);

            self->wifi_btns.push_back(wifi_btn);
        }

        lv_obj_remove_state(self->wlan_switch, LV_STATE_DISABLED);
        lv_obj_remove_state(self->status_btn, LV_STATE_DISABLED);
        lv_obj_add_flag(self->spinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(self->list1, LV_OBJ_FLAG_HIDDEN);

        wifi_config_t wifi_config = {};
        bool config_valid = false;
        portENTER_CRITICAL(&self->sta_snapshot_mux);
        if (self->sta_config_snapshot_valid &&
                self->sta_config_snapshot_generation == generation) {
            wifi_config = self->sta_config_snapshot;
            config_valid = true;
        }
        portEXIT_CRITICAL(&self->sta_snapshot_mux);
        if (!config_valid) {
            ESP_UTILS_LOGW("No STA configuration snapshot for this page session");
            return;
        }

        ESP_UTILS_LOGI("SSID stored in NVS: %s", wifi_config.sta.ssid);
        const bool has_stored_ssid = wifi_config.sta.ssid[0] != '\0';
        if (self->Wifi_state == CONNECTED && has_stored_ssid) {
            ESP_LOGI("WIFI", "Connected to SSID: %s", wifi_config.sta.ssid);
            copy_bounded_ssid(self->wifi_ssid, sizeof(self->wifi_ssid),
                              wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid));
            lv_obj_remove_flag(self->connected_text, LV_OBJ_FLAG_HIDDEN);
            lv_list_set_button_text(self->list1, self->conn_btn, (const char*)wifi_config.sta.ssid);
            lv_obj_remove_flag(self->conn_btn, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(self->wifi_icon, LV_SYMBOL_OK);
        } else {
            ESP_LOGI("WIFI", "Not connected");
            if (has_stored_ssid && self->_nvs_param_map[NVS_KEY_WIFI_ENABLE]) {
                printf("Wi-Fi ssid exists\n");
                copy_bounded_ssid(self->wifi_ssid, sizeof(self->wifi_ssid),
                                  wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid));

                lv_obj_remove_flag(self->spinner, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_state(self->wlan_switch, LV_STATE_DISABLED);
                lv_obj_add_flag(self->list1, LV_OBJ_FLAG_HIDDEN);

                self->post_worker_command(WorkerCommand::RECONNECT);
            } else {
                printf("Wi-Fi ssid empty\n");
            }
        }
    }

    // One-shot UI refresh after a connect attempt completes or times out.
    void WlanPage::wifi_sta_cb(lv_timer_t * timer) {
        WlanPage *self = nullptr;
        uint32_t generation = 0;
        if (!consume_timer_context(timer, &self, &generation) ||
                !self->page_active || self->page_generation.load() != generation ||
                !self->list1 || !self->connected_text || !self->conn_btn ||
                !self->wifi_icon) {
            return;
        }
        if (self->wifi_ssid[0] == '\0') {
            lv_obj_add_flag(self->connected_text, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(self->conn_btn, LV_OBJ_FLAG_HIDDEN);
            return;
        }

        lv_obj_remove_flag(self->connected_text, LV_OBJ_FLAG_HIDDEN);
        lv_list_set_button_text(self->list1, self->conn_btn, self->wifi_ssid);
        if (self->Wifi_state == CONNECTED)
        {
            lv_label_set_text(self->wifi_icon, LV_SYMBOL_OK);
            lv_obj_remove_flag(self->conn_btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(self->conn_btn, LV_OBJ_FLAG_HIDDEN);

        }
        else
        {
            lv_label_set_text(self->wifi_icon, LV_SYMBOL_CLOSE);
            lv_obj_add_flag(self->conn_btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(self->conn_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Keyboard READY/CANCEL hides the input and lets wifi_task perform the blocking connect.
    void WlanPage::kb_event_cb(lv_event_t *e)
    {
        WlanPage *self = (WlanPage *)lv_event_get_user_data(e);
        lv_event_code_t code = lv_event_get_code(e);

        if (code == LV_EVENT_CANCEL) {
            lv_obj_add_flag(self->kb, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(self->ta, LV_OBJ_FLAG_HIDDEN);
            if (self->password_title) {
                lv_obj_add_flag(self->password_title, LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_remove_flag(self->list1, LV_OBJ_FLAG_HIDDEN);
            lv_textarea_set_text(self->ta, "");
            return;
        }

        if (code == LV_EVENT_READY) {

            strlcpy(self->wifi_pwd, lv_textarea_get_text(ta), sizeof(self->wifi_pwd));
            ESP_UTILS_LOGI("Wi-Fi password length: %u", static_cast<unsigned>(strlen(self->wifi_pwd)));
            if (strlen(self->wifi_pwd) >= 8)
            {
                self->Wifi_state = CONNECTING;
            }
            else
            {
                self->Wifi_state = DISCONNECT;
                queue_lvgl_async_from_gui(wifi_state_cb, self);
            }
            lv_obj_add_flag(self->kb, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(self->ta, LV_OBJ_FLAG_HIDDEN);
            if (self->password_title) {
                lv_obj_add_flag(self->password_title, LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_remove_flag(self->spinner, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_state(self->wlan_switch, LV_STATE_DISABLED);
            lv_obj_add_flag(self->list1, LV_OBJ_FLAG_HIDDEN);

        }
    }

    // Textarea focus controls keyboard visibility.
    void WlanPage::ta_event_cb(lv_event_t * e)
    {
        WlanPage *self = (WlanPage *)lv_event_get_user_data(e);
        lv_obj_t * ta = (lv_obj_t*)lv_event_get_target(e);

        if (lv_event_get_code(e) == LV_EVENT_FOCUSED) {
            lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
            lv_keyboard_set_textarea(kb, ta);
        }
        else if (lv_event_get_code(e) == LV_EVENT_DEFOCUSED) {
            lv_obj_remove_flag(self->list1, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ta, LV_OBJ_FLAG_HIDDEN);
            if (self->password_title) {
                lv_obj_add_flag(self->password_title, LV_OBJ_FLAG_HIDDEN);
            }
            lv_keyboard_set_textarea(kb, NULL);
        }
    }

    // ESP event loop callback. UI work is deferred with lv_async_call because the
    // event loop does not run on the LVGL task.
    bool WlanPage::queue_lvgl_async(lv_async_cb_t callback, void *param,
                                    uint32_t generation)
    {
        auto *self = static_cast<WlanPage *>(param);
        if (self == nullptr || callback == nullptr || !self->page_active) {
            return false;
        }

        const uint32_t target_generation = generation != 0 ? generation :
                                           self->page_generation.load();
        if (self->page_generation.load() != target_generation) {
            return false;
        }

        auto *context = static_cast<async_context_t *>(malloc(sizeof(async_context_t)));
        if (!context) {
            ESP_UTILS_LOGW("Allocate Wi-Fi async context failed");
            return false;
        }
        context->self = self;
        context->generation = target_generation;
        context->callback = callback;

        // Producers never wait indefinitely for a busy GUI. page_active is
        // checked again after the bounded lock to cover concurrent page close.
        auto &lock = gui::LvLock::getInstance();
        if (!lock.lock(50)) {
            free(context);
            return false;
        }

        lv_result_t result = LV_RESULT_INVALID;
        if (self->page_active && self->page_generation.load() == target_generation) {
            result = lv_async_call(guarded_async_cb, context);
        }
        lock.unlock();

        if (result != LV_RESULT_OK) {
            free(context);
        }
        if (result != LV_RESULT_OK && self->page_active) {
            ESP_UTILS_LOGW("Queue LVGL async callback failed");
            return false;
        }
        return result == LV_RESULT_OK;
    }

    bool WlanPage::queue_lvgl_async_from_gui(lv_async_cb_t callback, WlanPage *self)
    {
        if (!self || !callback || !self->page_active) {
            return false;
        }
        auto *context = static_cast<async_context_t *>(malloc(sizeof(async_context_t)));
        if (!context) {
            return false;
        }
        context->self = self;
        context->generation = self->page_generation.load();
        context->callback = callback;
        const lv_result_t result = lv_async_call(guarded_async_cb, context);
        if (result != LV_RESULT_OK) {
            free(context);
        }
        return result == LV_RESULT_OK;
    }

    void WlanPage::guarded_async_cb(void *param)
    {
        auto *context = static_cast<async_context_t *>(param);
        if (!context) {
            return;
        }
        WlanPage *self = context->self;
        const uint32_t generation = context->generation;
        const lv_async_cb_t callback = context->callback;
        free(context);

        if (self && callback && self->page_active &&
                self->page_generation.load() == generation) {
            callback(self);
        }
    }

    lv_timer_t *WlanPage::create_session_timer(lv_timer_cb_t callback, uint32_t period_ms,
                                                WlanPage *self)
    {
        if (!self || !callback || !self->page_active) {
            return nullptr;
        }
        auto *context = static_cast<timer_context_t *>(malloc(sizeof(timer_context_t)));
        if (!context) {
            return nullptr;
        }
        context->self = self;
        context->generation = self->page_generation.load();
        lv_timer_t *timer = lv_timer_create(callback, period_ms, context);
        if (!timer) {
            free(context);
            return nullptr;
        }
        lv_timer_set_repeat_count(timer, 1);
        return timer;
    }

    bool WlanPage::consume_timer_context(lv_timer_t *timer, WlanPage **self,
                                         uint32_t *generation)
    {
        if (!timer || !self || !generation) {
            return false;
        }
        auto *context = static_cast<timer_context_t *>(lv_timer_get_user_data(timer));
        if (!context) {
            return false;
        }
        *self = context->self;
        *generation = context->generation;
        lv_timer_set_user_data(timer, nullptr);
        free(context);
        return *self != nullptr;
    }

    void WlanPage::post_worker_command(WorkerCommand command)
    {
        pending_command.store(command);
        if (wifi_TaskHandle) {
            xTaskNotifyGive(wifi_TaskHandle);
        }
    }

    void WlanPage::wifi_event_handler(void *arg, esp_event_base_t event_base,\
                                int32_t event_id, void *event_data)
    {
        WlanPage *self = static_cast<WlanPage *>(arg);
        if (!self) {
            return;
        }
        if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
            // esp_wifi_connect();
            ESP_UTILS_LOGI("WIFI_EVENT_STA_START");
        } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
            if (self->Wifi_state == WIFICLOSE || !self->wifi_enabled) {
                self->connection_num = 0;
                return;
            }
            if (self->connection_num < 3)
            {
                ESP_UTILS_LOGI("Resident Wi-Fi service is reconnecting");
            }
            else
            {
                self->connection_num = 0;
                ESP_UTILS_LOGI("connection_fail");
                self->Wifi_state = DISCONNECT;
                queue_lvgl_async(wifi_state_cb, self);
            }
            self->connection_num++;


        } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
            // ESP_UTILS_LOGI("IP_EVENT_STA_GOT_IP");
            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            ESP_UTILS_LOGI("got ip:" IPSTR, IP2STR(&event->ip_info.ip));
            self->Wifi_state = CONNECTED;
            self->connection_num = 0;

            queue_lvgl_async(wifi_state_cb, self);
        }

    }

    void WlanPage::start_wifi_events() {
        if (wifi_events_registered) {
            return;
        }
        // Register the Wi-Fi event handler
        esp_err_t ret = esp_event_handler_register(WIFI_EVENT,
                                                   ESP_EVENT_ANY_ID,
                                                   &wifi_event_handler,
                                                   this);
        if (ret != ESP_OK) {
            ESP_UTILS_LOGW("Register Wi-Fi event handler failed: %s", esp_err_to_name(ret));
            return;
        }

        ret = esp_event_handler_register(IP_EVENT,
                                         IP_EVENT_STA_GOT_IP,
                                         &wifi_event_handler,
                                         this);
        if (ret != ESP_OK) {
            ESP_UTILS_LOGW("Register IP event handler failed: %s", esp_err_to_name(ret));
            esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
            return;
        }

        wifi_events_registered = true;
        ESP_UTILS_LOGI("Wi-Fi event handler registered.\n");
    }

    void WlanPage::stop_wifi_events() {
        esp_err_t ret = esp_event_handler_unregister(WIFI_EVENT,
                                                    ESP_EVENT_ANY_ID,
                                                    &wifi_event_handler);
        if (ret != ESP_OK)
            ESP_UTILS_LOGI("Wi-Fi event handler unregistered Faile.\n");


        ret = esp_event_handler_unregister(IP_EVENT,
                                                    IP_EVENT_STA_GOT_IP,
                                                    &wifi_event_handler);
        if (ret != ESP_OK)
            ESP_UTILS_LOGI("IP event handler unregistered Faile.\n");

        wifi_events_registered = false;
        ESP_UTILS_LOGI("Wi-Fi event handler unregistered.\n");
    }

    // Background Wi-Fi worker. It serializes scan and connect operations so the UI
    // thread is never blocked by esp_wifi_scan_start(..., true).
    void WlanPage::wifi_task(void *arg)
    {
        WlanPage *self = static_cast<WlanPage*>(arg);
        if (!self) return;

        while (self->wifi_task_running)
        {
            const WorkerCommand command = self->pending_command.exchange(WorkerCommand::NONE);
            if (command == WorkerCommand::ENABLE) {
                const esp_err_t ret = brookesia::system_status::set_wifi_enabled(true);
                if (ret != ESP_OK) {
                    ESP_UTILS_LOGW("Enable resident Wi-Fi service failed: %s", esp_err_to_name(ret));
                    self->wifi_enabled = false;
                    self->Wifi_state = WIFICLOSE;
                    queue_lvgl_async(wifi_state_cb, self);
                } else if (self->wifi_enabled.load()) {
                    self->Wifi_state = WIFIOPEN;
                    self->scan_requested = true;
                }
            } else if (command == WorkerCommand::DISABLE) {
                const esp_err_t ret = brookesia::system_status::set_wifi_enabled(false);
                if (ret != ESP_OK) {
                    ESP_UTILS_LOGW("Disable resident Wi-Fi service failed: %s", esp_err_to_name(ret));
                }
                self->Wifi_state = WIFICLOSE;
            } else if (command == WorkerCommand::RECONNECT) {
                if (!self->wifi_enabled.load()) {
                    continue;
                }
                const esp_err_t ret = brookesia::system_status::request_wifi_connect();
                if (ret != ESP_OK) {
                    ESP_UTILS_LOGW("Request resident Wi-Fi connect failed: %s", esp_err_to_name(ret));
                    if (self->wifi_enabled.load()) {
                        self->Wifi_state = DISCONNECT;
                        queue_lvgl_async(wifi_state_cb, self);
                    }
                }
            } else if (command == WorkerCommand::FORGET) {
                esp_err_t ret = brookesia::system_status::set_wifi_enabled(false);
                if (ret == ESP_OK) {
                    ret = esp_wifi_restore();
                }
                if (ret == ESP_OK && self->wifi_enabled.load()) {
                    ret = brookesia::system_status::set_wifi_enabled(true);
                }
                if (ret != ESP_OK) {
                    ESP_UTILS_LOGW("Forget saved Wi-Fi network failed: %s", esp_err_to_name(ret));
                    self->wifi_enabled = false;
                    self->Wifi_state = WIFICLOSE;
                    queue_lvgl_async(wifi_state_cb, self);
                } else if (self->wifi_enabled.load()) {
                    self->Wifi_state = WIFIOPEN;
                    self->scan_requested = true;
                }
            }

            // ESP_UTILS_LOGI("Failed to get STA list");
            if (self->page_active && self->wifi_enabled &&
                    (self->scan_requested.exchange(false) || self->Wifi_state == WIFIOPEN))
            {
                const uint32_t scan_generation = self->page_generation.load();
                // A GOT_IP event must not cancel the page's initial refresh.
                if (self->Wifi_state != CONNECTED) {
                    self->Wifi_state = WIFISCAN;
                }

                uint16_t number = DEFAULT_SCAN_LIST_SIZE;  // Maximum number of APs to scan for
                uint16_t ap_count = 0;  // Variable to hold the number of found APs

                // Start the Wi-Fi scan
                esp_err_t ret = esp_wifi_scan_start(NULL, true);
                if (ret != ESP_OK) {
                    ESP_UTILS_LOGW("esp_wifi_scan_start failed: %s", esp_err_to_name(ret));
                }

                // A close/reopen during a scan belongs to a new UI session.
                if (!self->page_active || self->page_generation.load() != scan_generation) {
                    continue;
                }

                // Clear the ap_info array to hold fresh scan results
                memset(self->ap_info, 0, sizeof(self->ap_info));

                ESP_UTILS_LOGI("Max AP number ap_info can hold = %u", number);
                if (ret == ESP_OK) {
                    ret = esp_wifi_scan_get_ap_num(&ap_count);
                    if (ret != ESP_OK) {
                        ESP_UTILS_LOGW("esp_wifi_scan_get_ap_num failed: %s", esp_err_to_name(ret));
                        ap_count = 0;
                    }
                    ret = esp_wifi_scan_get_ap_records(&number, self->ap_info);
                    if (ret != ESP_OK) {
                        ESP_UTILS_LOGW("esp_wifi_scan_get_ap_records failed: %s", esp_err_to_name(ret));
                        number = 0;
                    }
                } else {
                    // Still publish the saved/connected network and enable the
                    // controls when scanning is unavailable (e.g. reconnecting).
                    number = 0;
                }
                self->scanned_ap_count = number;
                ESP_UTILS_LOGI("Total APs scanned = %u, actual AP number ap_info holds = %u", ap_count, number);

                // Driver access stays in the resident worker. Publish a bounded
                // copy so the LVGL timer only reads application-owned memory.
                wifi_config_t sta_config = {};
                const esp_err_t config_result = esp_wifi_get_config(WIFI_IF_STA, &sta_config);
                portENTER_CRITICAL(&self->sta_snapshot_mux);
                self->sta_config_snapshot_generation = scan_generation;
                self->sta_config_snapshot_valid = config_result == ESP_OK;
                if (config_result == ESP_OK) {
                    self->sta_config_snapshot = sta_config;
                } else {
                    self->sta_config_snapshot = {};
                }
                portEXIT_CRITICAL(&self->sta_snapshot_mux);
                if (config_result != ESP_OK) {
                    ESP_UTILS_LOGW("esp_wifi_get_config failed: %s",
                                   esp_err_to_name(config_result));
                }

                // Reopening WLAN does not generate another GOT_IP event.
                // Restore the existing connection from the resident service.
                brookesia::system_status::Snapshot snapshot = {};
                if (self->wifi_enabled &&
                        brookesia::system_status::get_snapshot(snapshot) &&
                        snapshot.wifi_connected) {
                    self->Wifi_state = CONNECTED;
                }

                if (self->page_active &&
                        self->page_generation.load() == scan_generation &&
                        self->wifi_enabled && self->Wifi_state != WIFICLOSE) {
                    const bool refresh_queued = queue_lvgl_async([](void *param) {
                        auto page = static_cast<WlanPage *>(param);
                        if (!page || !page->page_active || !page->list1) {
                            return;
                        }
                        if (!create_session_timer(page->wifi_scan_cb, 20, page)) {
                            ESP_UTILS_LOGW("Create Wi-Fi scan refresh timer failed");
                            page->scan_requested = true;
                        }
                    }, self, scan_generation);
                    if (!refresh_queued && self->page_active &&
                            self->page_generation.load() == scan_generation) {
                        self->scan_requested = true;
                    }
                }
            }

            if (self->Wifi_state == CONNECTING && self->wifi_ssid[0] != '\0')
            {
                ESP_UTILS_LOGI("ssid:%s", self->wifi_ssid);
                ESP_UTILS_LOGI("password length:%u", static_cast<unsigned>(strlen(self->wifi_pwd)));

                wifi_config_t wifi_config = {};
                const size_t ssid_length = strnlen(self->wifi_ssid, sizeof(self->wifi_ssid));
                memcpy(wifi_config.sta.ssid, self->wifi_ssid,
                       ssid_length < sizeof(wifi_config.sta.ssid) ?
                       ssid_length : sizeof(wifi_config.sta.ssid));
                strlcpy((char *)wifi_config.sta.password, self->wifi_pwd, sizeof(wifi_config.sta.password));
                wifi_config.sta.threshold.authmode = self->ap_info[self->wifi_index].authmode;

                // The resident service serializes configuration and reconnects
                // even after this page is closed.
                esp_err_t ret = brookesia::system_status::connect_wifi(wifi_config);
                if (ret != ESP_OK) {
                    ESP_UTILS_LOGW("Connect WiFi failed: %s", esp_err_to_name(ret));
                    if (self->wifi_enabled.load()) {
                        self->Wifi_state = DISCONNECT;
                        queue_lvgl_async(wifi_state_cb, self);
                    }
                }

                // GOT_IP can arrive before connect_wifi() returns. Preserve it.
                WifiState expected = CONNECTING;
                self->Wifi_state.compare_exchange_strong(
                    expected, self->wifi_enabled.load() ? DISCONNECT : WIFICLOSE
                );
            }

            // Sleep indefinitely when the page is closed. run() and command
            // producers notify this persistent worker when more work arrives.
            const TickType_t wait_ticks = self->page_active
                                          ? pdMS_TO_TICKS(100) : portMAX_DELAY;
            ulTaskNotifyTake(pdTRUE, wait_ticks);
        }

        self->wifi_TaskHandle = nullptr;
        vTaskDelete(NULL);
    }

    // Load or create persisted page settings.
    bool WlanPage::loadNvsParam(void)
    {
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open(NVS_STORAGE_NAMESPACE, NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGE("NVS", "Error (%s) opening NVS handle!", esp_err_to_name(err));
            return false;
        }

        for (auto& key_value : _nvs_param_map) {
            int32_t val = key_value.second;
            err = nvs_get_i32(nvs_handle, key_value.first.c_str(), &val);
            if (err == ESP_OK) {
                key_value.second = val;
                ESP_LOGI("NVS", "Load %s: %ld", key_value.first.c_str(), key_value.second);
            } else if (err == ESP_ERR_NVS_NOT_FOUND) {
                err = nvs_set_i32(nvs_handle, key_value.first.c_str(), val);
                if (err != ESP_OK) {
                    ESP_LOGE("NVS", "Error (%s) setting default for %s", esp_err_to_name(err), key_value.first.c_str());
                    nvs_close(nvs_handle);
                    return false;
                }
                ESP_LOGW("NVS", "Key %s not found, set default: %ld", key_value.first.c_str(), val);
            } else {
                ESP_LOGE("NVS", "Error (%s) reading key %s", esp_err_to_name(err), key_value.first.c_str());
                nvs_close(nvs_handle);
                return false;
            }
        }

        err = nvs_commit(nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGE("NVS", "Error (%s) committing NVS changes", esp_err_to_name(err));
            nvs_close(nvs_handle);
            return false;
        }

        nvs_close(nvs_handle);
        return true;
    }

    bool WlanPage::setNvsParam(std::string key, int value)
    {
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open(NVS_STORAGE_NAMESPACE, NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGE("NVS", "Error (%s) opening NVS handle!", esp_err_to_name(err));
            return false;
        }

        int32_t v = value;
        err = nvs_set_i32(nvs_handle, key.c_str(), v);
        if (err != ESP_OK) {
            ESP_LOGE("NVS", "Error (%s) setting %s", esp_err_to_name(err), key.c_str());
            nvs_close(nvs_handle);
            return false;
        }

        err = nvs_commit(nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGE("NVS", "Error (%s) committing NVS changes", esp_err_to_name(err));
            nvs_close(nvs_handle);
            return false;
        }

        ESP_LOGI("NVS", "Set %s = %d", key.c_str(), value);
        nvs_close(nvs_handle);
        return true;
    }
} // namespace esp_brookesia::apps
