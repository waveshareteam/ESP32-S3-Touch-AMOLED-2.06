/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <atomic>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "qmi8658.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class Gravitysphere : public systems::phone::App {
public:
    static Gravitysphere *requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);
    ~Gravitysphere() override;

    using systems::phone::App::endRecordResource;
    using systems::phone::App::startRecordResource;

protected:
    Gravitysphere(bool use_status_bar, bool use_navigation_bar);

    bool init() override;
    bool deinit() override;
    bool run() override;
    bool back() override;
    bool close() override;
    bool pause() override;
    bool resume() override;

private:
    enum class CalibrationState : uint8_t {
        IDLE,
        RUNNING,
        DONE,
        FAILED,
    };

    static Gravitysphere *_instance;

    static void workerTask(void *arg);
    static void uiTimerCallback(lv_timer_t *timer);
    static void screenEventCallback(lv_event_t *event);

    void workerLoop();
    bool performLevelCalibration();
    bool refreshVisualGeometry(bool request_recalibration);
    void createUi();
    void layoutUi();
    void updateUi();
    void releaseUi();
    void stopWorker();
    void constrainPosition(int &x, int &y) const;

    qmi8658_dev_t _imu = {};
    bool _imu_initialized = false;

    std::atomic<TaskHandle_t> _worker_task = nullptr;
    std::atomic<bool> _running = false;
    std::atomic<bool> _paused = false;
    std::atomic<bool> _recalibration_requested = false;
    std::atomic<CalibrationState> _calibration_state = CalibrationState::IDLE;
    std::atomic<int> _calibration_progress = 0;
    std::atomic<int> _ball_x = 0;
    std::atomic<int> _ball_y = 0;

    float _accel_bias_x = 0.0F;
    float _accel_bias_y = 0.0F;
    lv_area_t _visual_area = {};
    int _display_width = 0;
    int _display_height = 0;
    std::atomic<int> _movement_center_x = 0;
    std::atomic<int> _movement_center_y = 0;
    std::atomic<int> _movement_radius = 0;
    bool _button_was_pressed = false;

    lv_obj_t *_movement_boundary = nullptr;
    lv_obj_t *_ball = nullptr;
    lv_obj_t *_hint_label = nullptr;
    lv_obj_t *_calibration_label = nullptr;
    lv_obj_t *_calibration_bar = nullptr;
    lv_timer_t *_ui_timer = nullptr;
    CalibrationState _displayed_state = CalibrationState::IDLE;
    int _displayed_progress = -1;

    static constexpr gpio_num_t CALIBRATION_BUTTON_GPIO = GPIO_NUM_0;
    static constexpr int BALL_RADIUS = 22;
    // Match the ball's shadow width so its glow is not clipped at the round edge.
    static constexpr int MOVEMENT_SAFE_MARGIN = 18;
    static constexpr int UI_SAFE_MARGIN = 14;
    static constexpr int UI_INNER_PADDING = 8;
    static constexpr int ACCEL_SCALE_FACTOR = 8;
    static constexpr int SENSOR_PERIOD_MS = 30;
    static constexpr int CALIBRATION_SAMPLES = 200;
    static constexpr int CALIBRATION_MAX_RETRIES = 5;
    static constexpr float CALIBRATION_DEADZONE = 0.05F;
};

} // namespace esp_brookesia::apps
