/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <atomic>
#include <cstdint>

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class VibrationTest final : public systems::phone::App {
public:
    static VibrationTest *requestInstance();
    ~VibrationTest() override;

protected:
    VibrationTest();
    bool init() override;
    bool deinit() override;
    bool run() override;
    bool back() override;
    bool close() override;
    bool pause() override;
    bool resume() override;

private:
    static VibrationTest *_instance;
    static void workerTask(void *arg);
    static void uiTimerCallback(lv_timer_t *timer);
    static void motorEventCallback(lv_event_t *event);

    bool createUi(int width, int height);
    void updateUi();
    void releaseUi();
    bool configureMotorOutput(); // Caller holds _motor_mutex.
    void requestMotor(bool pressed);
    void updateMotorOutput(TickType_t now);
    void forceMotorOff();
    void workerLoop();
    void stopWorker();

    SemaphoreHandle_t _motor_mutex = nullptr;
    bool _motor_configured = false; // Protected by _motor_mutex.
    bool _motor_requested = false;
    TickType_t _motor_deadline = 0;
    std::atomic<TaskHandle_t> _worker_task = nullptr;
    std::atomic<bool> _running = false;
    std::atomic<bool> _paused = false;
    std::atomic<bool> _motor_ready = false;
    std::atomic<bool> _motor_active = false;
    std::atomic<esp_err_t> _motor_error = ESP_OK;
    std::atomic<uint32_t> _pulse_count = 0;

    // Only the GUI thread accesses widget references and cached UI state.
    bool _touch_held = false;
    int _displayed_state = -1;
    uint32_t _displayed_count = UINT32_MAX;
    lv_obj_t *_screen = nullptr;
    lv_obj_t *_motor_button = nullptr;
    lv_obj_t *_button_label = nullptr;
    lv_obj_t *_state_label = nullptr;
    lv_obj_t *_hint_label = nullptr;
    lv_obj_t *_count_label = nullptr;
    lv_timer_t *_ui_timer = nullptr;

    static constexpr gpio_num_t MOTOR_GPIO = GPIO_NUM_18;
    static constexpr int MOTOR_MAX_ON_MS = 250;
    static constexpr int WORKER_PERIOD_MS = 5;
    static constexpr int UI_PERIOD_MS = 50;
    static constexpr int RETRY_PERIOD_MS = 1000;
};

} // namespace esp_brookesia::apps
