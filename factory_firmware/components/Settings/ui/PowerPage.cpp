/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "PowerPage.hpp"

#include "../Settings.hpp"
#include "SettingsUI.hpp"
#include "system_status.hpp"

#ifdef ESP_UTILS_LOG_TAG
#undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:PowerPage"
#include "esp_lib_utils.h"

LV_IMG_DECLARE(battery);

namespace esp_brookesia::apps {

namespace {

lv_obj_t *getValueLabel(lv_obj_t *row)
{
    if (row == nullptr) {
        return nullptr;
    }
    const uint32_t child_count = lv_obj_get_child_count(row);
    return child_count > 0 ? lv_obj_get_child(row, static_cast<int32_t>(child_count - 1)) : nullptr;
}

const char *chargeStateText(const brookesia::system_status::Snapshot &snapshot)
{
    if (!snapshot.battery_present) {
        return "No battery";
    }

    switch (snapshot.battery_power_state) {
    case brookesia::system_status::BatteryPowerState::CHARGING:
        switch (snapshot.charger_status) {
        case 1:
            return "Pre-charging";
        case 2:
            return "Charging (CC)";
        case 3:
            return "Charging (CV)";
        default:
            return "Charging";
        }
    case brookesia::system_status::BatteryPowerState::DISCHARGING:
        return "Discharging";
    case brookesia::system_status::BatteryPowerState::STANDBY:
        return snapshot.charger_status == 4 ? "Charged" : "Standby";
    case brookesia::system_status::BatteryPowerState::UNKNOWN:
    default:
        return snapshot.charger_status == 4 ? "Charged" : "Unknown";
    }
}

} // namespace

PowerPage *PowerPage::_instance = nullptr;

PowerPage *PowerPage::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (_instance == nullptr) {
        _instance = new PowerPage(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

PowerPage::PowerPage(bool use_status_bar, bool use_navigation_bar)
    : App("Power", nullptr, true, use_status_bar, use_navigation_bar)
{
}

PowerPage::~PowerPage()
{
}

bool PowerPage::run()
{
    ESP_UTILS_LOGD("PowerPage Run");
    if (page_root != nullptr) {
        close();
    }
    lv_obj_clean(lv_screen_active());

    page_root = settings_ui::create_page(lv_screen_active());
    settings_ui::create_header(page_root, "Power & Network", [](lv_event_t *event) {
        (void)event;
        lv_async_call([](void *param) {
            (void)param;
            Settings::requestInstance()->showRootPage();
        }, nullptr);
    });

    settings_ui::init_list_styles(style_list, style_row, style_section, style_pressed);
    list = settings_ui::create_content_list(page_root);
    lv_obj_add_style(list, &style_list, LV_PART_MAIN);

    settings_ui::add_section(list, "Battery", style_section);
    percent_value = getValueLabel(
        settings_ui::add_info_row(list, &battery, "Level", "--", style_row)
    );
    charge_value = getValueLabel(
        settings_ui::add_info_row(list, nullptr, "State", "Measuring...", style_row)
    );
    present_value = getValueLabel(
        settings_ui::add_info_row(list, nullptr, "Present", "Unknown", style_row)
    );

    settings_ui::add_section(list, "Network", style_section);
    wifi_value = getValueLabel(
        settings_ui::add_info_row(list, nullptr, "Wi-Fi", "Disconnected", style_row)
    );
    rssi_value = getValueLabel(
        settings_ui::add_info_row(list, nullptr, "Signal", "--", style_row)
    );

    refreshValues();
    refresh_timer = lv_timer_create(refreshTimerCallback, 1000, this);
    return refresh_timer != nullptr;
}

bool PowerPage::back()
{
    ESP_UTILS_LOGD("PowerPage Back");
    Settings::requestInstance()->showRootPage();
    return true;
}

bool PowerPage::close()
{
    ESP_UTILS_LOGD("PowerPage Close");
    if (refresh_timer != nullptr) {
        lv_timer_delete(refresh_timer);
        refresh_timer = nullptr;
    }

    if (page_root != nullptr) {
        lv_obj_delete(page_root);
        page_root = nullptr;
        list = nullptr;
        percent_value = nullptr;
        charge_value = nullptr;
        present_value = nullptr;
        wifi_value = nullptr;
        rssi_value = nullptr;
        settings_ui::reset_list_styles(style_list, style_row, style_section, style_pressed);
    }
    return true;
}

void PowerPage::refreshTimerCallback(lv_timer_t *timer)
{
    auto *page = static_cast<PowerPage *>(lv_timer_get_user_data(timer));
    if (page != nullptr) {
        page->refreshValues();
    }
}

void PowerPage::refreshValues()
{
    if (percent_value == nullptr || charge_value == nullptr || present_value == nullptr ||
            wifi_value == nullptr || rssi_value == nullptr) {
        return;
    }

    brookesia::system_status::Snapshot snapshot = {};
    if (!brookesia::system_status::get_snapshot(snapshot)) {
        lv_label_set_text(percent_value, "--");
        lv_label_set_text(charge_value, "Unavailable");
        lv_label_set_text(present_value, "Unknown");
        lv_label_set_text(wifi_value, "Unavailable");
        lv_label_set_text(rssi_value, "--");
        return;
    }

    if (!snapshot.battery_valid) {
        lv_label_set_text(percent_value, "--");
        lv_label_set_text(charge_value, "Unknown");
        lv_label_set_text(present_value, "Unknown");
    } else if (!snapshot.battery_present) {
        lv_label_set_text(percent_value, "--");
        lv_label_set_text(charge_value, "No battery");
        lv_label_set_text(present_value, "No");
    } else {
        lv_label_set_text_fmt(percent_value, "%d%%", snapshot.battery_percent);
        lv_label_set_text(charge_value, chargeStateText(snapshot));
        lv_label_set_text(present_value, "Yes");
    }

    if (!snapshot.wifi_enabled) {
        lv_label_set_text(wifi_value, "Off");
        lv_label_set_text(rssi_value, "--");
    } else if (snapshot.wifi_connected) {
        lv_label_set_text(wifi_value, "Connected");
        if (snapshot.wifi_rssi > -127) {
            lv_label_set_text_fmt(rssi_value, "%d dBm", snapshot.wifi_rssi);
        } else {
            lv_label_set_text(rssi_value, "Acquiring...");
        }
    } else {
        lv_label_set_text(wifi_value, "Disconnected");
        lv_label_set_text(rssi_value, "--");
    }
}

} // namespace esp_brookesia::apps
