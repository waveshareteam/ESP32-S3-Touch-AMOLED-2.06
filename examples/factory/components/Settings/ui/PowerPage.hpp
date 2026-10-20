/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_brookesia.hpp"
#include "lvgl.h"

namespace esp_brookesia::apps {

class PowerPage : public systems::phone::App {
public:
    static PowerPage *requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);

    PowerPage(bool use_status_bar, bool use_navigation_bar);
    ~PowerPage() override;

    bool run() override;
    bool back() override;
    bool close() override;

private:
    static PowerPage *_instance;

    static void refreshTimerCallback(lv_timer_t *timer);
    void refreshValues();

    lv_obj_t *page_root = nullptr;
    lv_obj_t *list = nullptr;
    lv_obj_t *percent_value = nullptr;
    lv_obj_t *charge_value = nullptr;
    lv_obj_t *present_value = nullptr;
    lv_obj_t *wifi_value = nullptr;
    lv_obj_t *rssi_value = nullptr;
    lv_timer_t *refresh_timer = nullptr;

    lv_style_t style_list = {};
    lv_style_t style_row = {};
    lv_style_t style_section = {};
    lv_style_t style_pressed = {};
};

} // namespace esp_brookesia::apps
