/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "systems/phone/esp_brookesia_phone_app.hpp"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "./ui/AboutPage.hpp"
#include "./ui/DisplayPage.hpp"
#include "./ui/PowerPage.hpp"
#include "./ui/SoundPage.hpp"
#include "./ui/StoragePage.hpp"
#include "./ui/WlanPage.hpp"

namespace esp_brookesia::apps
{

    /**
     * @brief Settings application for displaying a list of options
     *
     */
    class Settings : public systems::phone::App
    {
    public:
        static Settings *requestInstance(bool use_status_bar = true, bool use_navigation_bar = false);
        ~Settings();
        bool run(void) override;
        void showRootPage();
        // Prepare the network before installation, without holding the GUI lock.
        esp_err_t initWifi();

    protected:
        Settings(bool use_status_bar, bool use_navigation_bar);

        bool back(void) override;
        bool close(void) override;
        bool init(void) override;
        bool deinit(void) override;
        bool pause(void) override;
        bool resume(void) override;

    private:
        enum class ActivePage {
            None,
            Wlan,
            Sound,
            Display,
            Power,
            Storage,
            About,
        };

        static void event_handler_cb(lv_event_t *e);
        static void open_page_async_cb(void *user_data);
        void add_button_with_arrow(const void *icon, const char *text);
        void create_settings_ui();
        void destroy_settings_ui();
        void event_handler(lv_event_t *e);
        void open_page(const char *page_name);
        void close_active_page();

    private:
        static Settings *_instance;
        lv_obj_t *page_root;
        lv_obj_t *list1;
        lv_style_t style_list;
        lv_style_t style_list_btn;
        lv_style_t style_list_text;
        lv_style_t style_list_btn_pressed;
        ActivePage active_page;
    };

} // namespace esp_brookesia::apps
