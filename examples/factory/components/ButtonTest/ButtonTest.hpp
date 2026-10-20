/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <atomic>
#include <cstdint>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class ButtonTest final : public systems::phone::App {
public:
    static ButtonTest *requestInstance();
    ~ButtonTest() override;

    using systems::phone::App::endRecordResource;
    using systems::phone::App::startRecordResource;

protected:
    ButtonTest();

    bool init() override;
    bool deinit() override;
    bool run() override;
    bool back() override;
    bool close() override;
    bool pause() override;
    bool resume() override;

private:
    struct ButtonWidgets {
        lv_obj_t *card = nullptr;
        lv_obj_t *state_panel = nullptr;
        lv_obj_t *state_label = nullptr;
        lv_obj_t *count_label = nullptr;
        lv_obj_t *pass_label = nullptr;
        int displayed_level = -2;
        uint32_t displayed_count = UINT32_MAX;
    };

    static ButtonTest *_instance;

    static void workerTask(void *arg);
    static void uiTimerCallback(lv_timer_t *timer);
    static void resetEventCallback(lv_event_t *event);

    bool configureInput(gpio_num_t gpio, bool active_high);
    bool createUi();
    ButtonWidgets createButtonCard(
        lv_obj_t *parent,
        const char *name,
        const char *subtitle
    );
    void layoutUi();
    void updateUi();
    void updateButtonCard(
        ButtonWidgets &widgets,
        int level,
        bool active_high,
        uint32_t count
    );
    void releaseUi();
    void workerLoop();
    void stopWorker();

    std::atomic<TaskHandle_t> _worker_task = nullptr;
    std::atomic<bool> _running = false;
    std::atomic<bool> _paused = false;
    std::atomic<int> _boot_level = -1;
    std::atomic<int> _pwr_level = -1;
    std::atomic<uint32_t> _boot_count = 0;
    std::atomic<uint32_t> _pwr_count = 0;
    std::atomic<bool> _reset_requested = false;
    std::atomic<bool> _reset_sequence = false;

    lv_area_t _visual_area = {};
    int _visual_width = 0;
    int _visual_height = 0;
    lv_obj_t *_screen = nullptr;
    lv_obj_t *_title_label = nullptr;
    lv_obj_t *_subtitle_label = nullptr;
    ButtonWidgets _boot_widgets = {};
    ButtonWidgets _pwr_widgets = {};
    lv_obj_t *_result_panel = nullptr;
    lv_obj_t *_result_label = nullptr;
    lv_obj_t *_result_hint = nullptr;
    lv_obj_t *_progress_bar = nullptr;
    lv_obj_t *_reset_button = nullptr;
    lv_obj_t *_warning_label = nullptr;
    lv_timer_t *_ui_timer = nullptr;
    int _displayed_result = -2;

    static constexpr gpio_num_t BOOT_GPIO = GPIO_NUM_0;
    // Schematic KEYS: PWRON -> T1 inverter -> SYS_OUT -> GPIO10.
    // The external pull-up makes SYS_OUT high while PWR is held down.
    static constexpr gpio_num_t PWR_GPIO = GPIO_NUM_10;
    static constexpr int SAMPLE_PERIOD_MS = 20;
    static constexpr int UI_PERIOD_MS = 50;
    static constexpr int INPUT_RETRY_PERIOD_MS = 1000;
};

} // namespace esp_brookesia::apps
