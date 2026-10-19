/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <cstdio>
#include <ctime>
#include <new>
#include <vector>

#include "bsp/esp-bsp.h"
#include "esp_brookesia.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "Drawpanel.hpp"
#include "Gallery.hpp"
#include "MusicPlayer.hpp"
#include "Recorder.hpp"
#include "Settings.hpp"
#include "SpecAnalyzer.hpp"
#include "VideoPlayer.hpp"
#include "XiaozhiApp.hpp"
#include "bsp_board_extra.h"
#include "chat_history.h"
#include "esp_brookesia_app_calculator.hpp"
#include "storage_service.h"
#include "system_status.hpp"

#ifdef ESP_UTILS_LOG_TAG
#undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BrookesiaFirmware"
#include "esp_lib_utils.h"
#include "./dark/stylesheet.hpp"

using namespace esp_brookesia;
using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems::phone;

namespace {

#define LVGL_PORT_INIT_CONFIG()  \
    {                            \
        .task_priority = 4,      \
        .task_stack = 20 * 1024, \
        .task_affinity = -1,     \
        .task_max_sleep_ms = 100,\
        .timer_period_ms = 5,    \
    }

lv_display_t *startup_display = nullptr;
lv_obj_t *startup_panel = nullptr;
lv_obj_t *startup_stage_label = nullptr;
lv_obj_t *startup_progress_bar = nullptr;
int startup_progress = 0;

void update_startup_screen(const char *stage, int progress)
{
    LvLockGuard gui_guard;
    if (startup_panel == nullptr) {
        return;
    }
    startup_progress = progress;
    lv_label_set_text(startup_stage_label, stage);
    lv_bar_set_value(startup_progress_bar, progress, LV_ANIM_OFF);
    // Present each stage immediately; the LVGL task animates it between updates.
    lv_refr_now(startup_display);
}

bool show_startup_screen(lv_display_t *display)
{
    LvLockGuard gui_guard;
    startup_display = display;
    // The top layer survives Brookesia's main-screen and system-layer replacement.
    startup_panel = lv_obj_create(lv_display_get_layer_top(display));
    if (startup_panel == nullptr) {
        return false;
    }
    lv_obj_remove_style_all(startup_panel);
    lv_obj_set_size(startup_panel, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_pos(startup_panel, 0, 0);
    lv_obj_set_style_bg_color(startup_panel, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(startup_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(startup_panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(startup_panel, LV_OBJ_FLAG_CLICKABLE);

    auto create_label = [](const char *text, const lv_font_t *font, uint32_t color, int y) {
        lv_obj_t *label = lv_label_create(startup_panel);
        if (label != nullptr) {
            lv_label_set_text(label, text);
            lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
            lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_set_width(label, BSP_LCD_H_RES - 80);
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
            lv_obj_align(label, LV_ALIGN_CENTER, 0, y);
        }
        return label;
    };

    lv_obj_t *brand = create_label("WAVESHARE", &lv_font_montserrat_24, 0xF5F5F5, -142);
    lv_obj_t *spinner = lv_spinner_create(startup_panel);
    if (brand == nullptr || spinner == nullptr) {
        return false;
    }
    lv_obj_set_size(spinner, 76, 76);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -52);
    lv_spinner_set_anim_params(spinner, 1200, 200);
    lv_obj_set_style_arc_width(spinner, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x333C4A), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x38BDF8), LV_PART_INDICATOR);

    lv_obj_t *title = create_label("Loading...", &lv_font_montserrat_24, 0xF5F5F5, 23);
    startup_progress_bar = lv_bar_create(startup_panel);
    if (title == nullptr || startup_progress_bar == nullptr) {
        return false;
    }
    lv_obj_set_size(startup_progress_bar, BSP_LCD_H_RES - 130, 8);
    lv_obj_align(startup_progress_bar, LV_ALIGN_CENTER, 0, 76);
    lv_bar_set_range(startup_progress_bar, 0, 100);
    lv_obj_set_style_bg_color(startup_progress_bar, lv_color_hex(0x333C4A), LV_PART_MAIN);
    lv_obj_set_style_bg_color(startup_progress_bar, lv_color_hex(0x38BDF8), LV_PART_INDICATOR);
    lv_obj_set_style_radius(startup_progress_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(startup_progress_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    startup_stage_label = create_label("Starting up...", &lv_font_montserrat_16, 0xAAB4C4, 111);
    lv_obj_t *hint = create_label("Please wait", &lv_font_montserrat_14, 0x7A8596, 147);
    if (startup_stage_label == nullptr || hint == nullptr) {
        return false;
    }
    update_startup_screen("Starting up...", 5);
    // The QSPI command waits for the first loading frame's pixel transfer to finish.
    return bsp_display_backlight_on() == ESP_OK;
}

void hide_startup_screen(Phone *phone)
{
    LvLockGuard gui_guard;
    if (startup_panel == nullptr) {
        return;
    }
    lv_obj_delete(startup_panel); // Also deletes the spinner's animations.
    startup_panel = nullptr;
    startup_stage_label = nullptr;
    startup_progress_bar = nullptr;
    // Reveal the system bars in the same locked frame as the home screen.
    lv_obj_remove_flag(phone->getDisplay().getSystemScreenObject(), LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(lv_display_get_screen_active(startup_display));
    lv_refr_now(startup_display);
    startup_display = nullptr;
}

esp_err_t init_nvs()
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        result = nvs_flash_erase();
        if (result == ESP_OK) {
            result = nvs_flash_init();
        }
    }
    return result;
}

void update_status_bar_clock(lv_timer_t *timer)
{
    auto *phone = static_cast<Phone *>(lv_timer_get_user_data(timer));
    if (phone == nullptr) {
        return;
    }

    time_t now = 0;
    struct tm time_info = {};
    time(&now);
    localtime_r(&now, &time_info);

    auto *status_bar = phone->getDisplay().getStatusBar();
    if (status_bar != nullptr) {
        status_bar->setClock(time_info.tm_hour, time_info.tm_min);
    }
}

bool install_app(Phone *phone, systems::base::App *app, const char *name)
{
    if (phone == nullptr || app == nullptr) {
        ESP_UTILS_LOGE("Create %s failed", name);
        return false;
    }
    char stage[96];
    std::snprintf(stage, sizeof(stage), "Loading %s...", name);
    update_startup_screen(stage, startup_progress);
    const int64_t started_us = esp_timer_get_time();
    int app_id;
    {
        // Protect one installation only, so the animation can run between apps.
        LvLockGuard gui_guard;
        app_id = phone->getManager().installApp(app);
    }
    if (app_id < systems::base::App::APP_ID_MIN) {
        ESP_UTILS_LOGE("Install %s failed", name);
        return false;
    }
    ESP_UTILS_LOGI("Installed %s (id: %d, %lld ms)", name, app_id,
                   static_cast<long long>((esp_timer_get_time() - started_us) / 1000));
    if (startup_panel != nullptr) {
        const int progress = startup_progress <= 93 ? startup_progress + 2 : 95;
        update_startup_screen(stage, progress);
    }
    // Yield with the GUI lock released, including on single-core builds.
    vTaskDelay(1);
    return true;
}

} // namespace

extern "C" void app_main(void)
{
    ESP_UTILS_LOGI("Starting ESP32-S3-Touch-AMOLED-2.06 default firmware");

    bsp_display_cfg_t display_config = {
        .lvgl_port_cfg = LVGL_PORT_INIT_CONFIG(),
        // A 20-line RGB565 DMA buffer needs only 16,400 bytes of SRAM.
        // Normal UI allocations continue to prefer PSRAM via sdkconfig.
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
        },
    };
    lv_display_t *display = bsp_display_start_with_config(&display_config);
    ESP_UTILS_CHECK_NULL_EXIT(display, "Start display failed");

    LvLock::registerCallbacks([](int timeout_ms) {
        // esp_lvgl_port uses zero for an indefinite wait. Preserve Brookesia's
        // negative default and give explicit zero-time requests one tick.
        if (timeout_ms < 0) {
            timeout_ms = 0;
        } else if (timeout_ms == 0) {
            timeout_ms = 1;
        }
        return bsp_display_lock(static_cast<uint32_t>(timeout_ms));
    }, []() {
        bsp_display_unlock();
        return true;
    });

    ESP_UTILS_CHECK_FALSE_EXIT(show_startup_screen(display), "Show startup screen failed");

    update_startup_screen("Preparing settings...", 10);
    ESP_UTILS_CHECK_ERROR_EXIT(init_nvs(), "Initialize NVS failed");
    update_startup_screen("Preparing storage...", 20);
    ESP_UTILS_CHECK_ERROR_EXIT(
        storage_service_init(), "Initialize SD storage service failed"
    );
    update_startup_screen("Loading saved data...", 30);
    const esp_err_t chat_history_result = chat_history_init();
    if (chat_history_result != ESP_OK) {
        ESP_UTILS_LOGW(
            "AIChats history unavailable: %s", esp_err_to_name(chat_history_result)
        );
    }

    update_startup_screen("Loading resources...", 40);
    const esp_err_t spiffs_result = bsp_spiffs_mount();
    if (spiffs_result != ESP_OK && spiffs_result != ESP_ERR_INVALID_STATE) {
        ESP_UTILS_LOGW(
            "Internal storage unavailable: %s", esp_err_to_name(spiffs_result)
        );
    }

    update_startup_screen("Preparing sound...", 50);
    const esp_err_t audio_result = bsp_extra_codec_init();
    if (audio_result != ESP_OK) {
        ESP_UTILS_LOGW(
            "Audio initialization skipped: %s", esp_err_to_name(audio_result)
        );
    }

    // Wi-Fi initialization and the large SPIFFS font read must not run under
    // the installation lock: otherwise they freeze the loading spinner.
    update_startup_screen("Preparing Wi-Fi...", 54);
    auto *settings = apps::Settings::requestInstance();
    ESP_UTILS_CHECK_NULL_EXIT(settings, "Create Settings failed");
    ESP_UTILS_CHECK_ERROR_EXIT(settings->initWifi(), "Initialize Wi-Fi failed");

    update_startup_screen("Loading fonts...", 57);
    auto *xiaozhi = apps::XiaozhiApp::requestInstance();
    ESP_UTILS_CHECK_NULL_EXIT(xiaozhi, "Create AIChats failed");
    ESP_UTILS_CHECK_FALSE_EXIT(xiaozhi->prepareUiResources(), "Prepare AIChats fonts failed");

    update_startup_screen("Preparing home screen...", 60);
    Phone *phone = new (std::nothrow) Phone();
    ESP_UTILS_CHECK_NULL_EXIT(phone, "Create phone failed");
    StatusBar *status_bar = nullptr;

    {
        LvLockGuard gui_guard;

        if ((BSP_LCD_H_RES == 410) && (BSP_LCD_V_RES == 502)) {
            Stylesheet *stylesheet =
                new (std::nothrow) Stylesheet(STYLESHEET_410_502_DARK);
            ESP_UTILS_CHECK_NULL_EXIT(stylesheet, "Create stylesheet failed");
            ESP_UTILS_CHECK_FALSE_EXIT(
                phone->addStylesheet(stylesheet), "Add stylesheet failed"
            );
            ESP_UTILS_CHECK_FALSE_EXIT(
                phone->activateStylesheet(stylesheet), "Activate stylesheet failed"
            );
            delete stylesheet;
        }

        ESP_UTILS_CHECK_FALSE_EXIT(phone->begin(), "Begin phone failed");
        // The system layer is above the loading page's top layer. Keep its
        // bars hidden until startup finishes, before releasing the GUI lock.
        lv_obj_add_flag(phone->getDisplay().getSystemScreenObject(), LV_OBJ_FLAG_HIDDEN);
    }

    update_startup_screen("Loading apps...", 65);
    // Board I/O, Vibration, Gravitysphere, RTC and SquareLine register
    // themselves with Brookesia and are installed through this registry.
    std::vector<systems::base::Manager::RegistryAppInfo> registry_apps;
    {
        LvLockGuard gui_guard;
        ESP_UTILS_CHECK_FALSE_EXIT(
            phone->initAppFromRegistry(registry_apps),
            "Initialize application registry failed"
        );
    }
    for (auto &[name, app] : registry_apps) {
        ESP_UTILS_CHECK_FALSE_EXIT(
            install_app(phone, app.get(), name.c_str()),
            "Install registry application failed"
        );
    }

    ESP_UTILS_CHECK_FALSE_EXIT(
        install_app(phone, apps::Calculator::requestInstance(), "Calculator"),
        "Install Calculator failed"
    );
    ESP_UTILS_CHECK_FALSE_EXIT(
        install_app(phone, apps::Drawpanel::requestInstance(), "DrawPanel"),
        "Install DrawPanel failed"
    );
    ESP_UTILS_CHECK_FALSE_EXIT(
        install_app(phone, apps::SpecAnalyzer::requestInstance(), "SpecAnalyzer"),
        "Install SpecAnalyzer failed"
    );
    ESP_UTILS_CHECK_FALSE_EXIT(
        install_app(phone, apps::MusicPlayer::requestInstance(), "MusicPlayer"),
        "Install MusicPlayer failed"
    );
    ESP_UTILS_CHECK_FALSE_EXIT(
        install_app(phone, apps::Gallery::requestInstance(), "Gallery"),
        "Install Gallery failed"
    );
    ESP_UTILS_CHECK_FALSE_EXIT(
        install_app(phone, apps::VideoPlayer::requestInstance(), "VideoPlayer"),
        "Install VideoPlayer failed"
    );
    ESP_UTILS_CHECK_FALSE_EXIT(
        install_app(phone, apps::Recorder::requestInstance(), "Recorder"),
        "Install Recorder failed"
    );
    ESP_UTILS_CHECK_FALSE_EXIT(
        install_app(phone, settings, "Settings"),
        "Install Settings failed"
    );
    ESP_UTILS_CHECK_FALSE_EXIT(
        install_app(phone, xiaozhi, "AIChats"),
        "Install AIChats failed"
    );

    {
        LvLockGuard gui_guard;
        if (lv_timer_create(update_status_bar_clock, 1000, phone) == nullptr) {
            ESP_UTILS_LOGW("Create status-bar clock timer failed");
        }
        status_bar = phone->getDisplay().getStatusBar();
    }

    update_startup_screen("Finishing up...", 98);
    const esp_err_t status_result =
        brookesia::system_status::start(status_bar, 1000);
    if (status_result != ESP_OK) {
        ESP_UTILS_LOGW(
            "Battery/Wi-Fi status monitor unavailable: %s",
            esp_err_to_name(status_result)
        );
    }

    update_startup_screen("Ready", 100);
    hide_startup_screen(phone);
    ESP_UTILS_LOGI("Brookesia firmware is ready");
}
