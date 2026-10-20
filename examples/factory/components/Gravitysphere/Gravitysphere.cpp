/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Gravitysphere.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

#include "bsp/esp-bsp.h"
#include "esp_brookesia.hpp"
#include "esp_err.h"
#include "esp_lib_utils.h"

#ifdef ESP_UTILS_LOG_TAG
#undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:Gravitysphere"

LV_IMG_DECLARE(img_app_qmi8658ball);

namespace esp_brookesia::apps {

namespace {
constexpr char APP_NAME[] = "Gravitysphere";
}

Gravitysphere *Gravitysphere::_instance = nullptr;

Gravitysphere *Gravitysphere::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (_instance == nullptr) {
        _instance = new Gravitysphere(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

Gravitysphere::Gravitysphere(bool use_status_bar, bool use_navigation_bar)
    : App(APP_NAME, &img_app_qmi8658ball, true, use_status_bar, use_navigation_bar)
{
}

Gravitysphere::~Gravitysphere()
{
    stopWorker();
}

bool Gravitysphere::init()
{
    ESP_UTILS_LOGI("Initializing the QMI8658 motion sensor");

    const i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    ESP_UTILS_CHECK_NULL_RETURN(bus, false, "Get board I2C bus failed");

    esp_err_t result = qmi8658_init(&_imu, bus, QMI8658_ADDRESS_HIGH);
    ESP_UTILS_CHECK_ERROR_RETURN(result, false, "Initialize QMI8658 failed");

    result = qmi8658_set_accel_range(&_imu, QMI8658_ACCEL_RANGE_8G);
    ESP_UTILS_CHECK_ERROR_RETURN(result, false, "Set QMI8658 accelerometer range failed");
    result = qmi8658_set_accel_odr(&_imu, QMI8658_ACCEL_ODR_500HZ);
    ESP_UTILS_CHECK_ERROR_RETURN(result, false, "Set QMI8658 accelerometer rate failed");
    // qmi8658 v2.0.0 exposes unit selection as a void setter.
    qmi8658_set_accel_unit_mps2(&_imu, true);
    result = qmi8658_write_register(&_imu, QMI8658_CTRL5, 0x03);
    ESP_UTILS_CHECK_ERROR_RETURN(result, false, "Configure QMI8658 filtering failed");

    const gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << CALIBRATION_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    result = gpio_config(&button_config);
    ESP_UTILS_CHECK_ERROR_RETURN(result, false, "Configure calibration button failed");

    _imu_initialized = true;
    return true;
}

bool Gravitysphere::deinit()
{
    stopWorker();
    releaseUi();
    _imu_initialized = false;
    return true;
}

bool Gravitysphere::run()
{
    ESP_UTILS_CHECK_FALSE_RETURN(_imu_initialized, false, "QMI8658 is not initialized");
    ESP_UTILS_CHECK_FALSE_RETURN(_worker_task.load() == nullptr, false, "Worker is already running");

    ESP_UTILS_CHECK_FALSE_RETURN(refreshVisualGeometry(false), false, "Invalid visual area");

    const int center_x = _movement_center_x.load();
    const int center_y = _movement_center_y.load();
    _ball_x.store(center_x);
    _ball_y.store(center_y);

    createUi();

    _calibration_progress.store(0);
    _calibration_state.store(CalibrationState::RUNNING);
    _recalibration_requested.store(true);
    _paused.store(false);
    _running.store(true);
    _button_was_pressed = false;

    TaskHandle_t task = nullptr;
    const BaseType_t created = xTaskCreatePinnedToCore(
        workerTask, "gravitysphere", 6144, this, 4, &task, 1
    );
    if (created != pdPASS) {
        _running.store(false);
        ESP_UTILS_LOGE("Create Gravitysphere worker failed");
        return false;
    }
    _worker_task.store(task);
    return true;
}

bool Gravitysphere::back()
{
    return notifyCoreClosed();
}

bool Gravitysphere::close()
{
    stopWorker();
    releaseUi();
    return true;
}

bool Gravitysphere::pause()
{
    _paused.store(true);
    if (_ui_timer != nullptr) {
        lv_timer_pause(_ui_timer);
    }
    return true;
}

bool Gravitysphere::resume()
{
    _paused.store(false);
    if (_ui_timer != nullptr) {
        lv_timer_reset(_ui_timer);
        lv_timer_resume(_ui_timer);
    }
    const TaskHandle_t task = _worker_task.load();
    if (task != nullptr) {
        xTaskNotifyGive(task);
    }
    return true;
}

void Gravitysphere::createUi()
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0B1020), LV_PART_MAIN);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen, screenEventCallback, LV_EVENT_CLICKED, this);

    _movement_boundary = lv_obj_create(screen);
    lv_obj_remove_flag(_movement_boundary, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(_movement_boundary, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(_movement_boundary, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_movement_boundary, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(_movement_boundary, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(_movement_boundary, lv_color_hex(0x52627F), LV_PART_MAIN);
    lv_obj_set_style_border_opa(_movement_boundary, LV_OPA_30, LV_PART_MAIN);

    _hint_label = lv_label_create(screen);
    lv_label_set_text(_hint_label, "Tap screen or press BOOT\nto recalibrate");
    lv_obj_set_style_text_color(_hint_label, lv_color_hex(0xA9B7D0), LV_PART_MAIN);
    lv_obj_set_style_text_align(_hint_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_add_flag(_hint_label, LV_OBJ_FLAG_HIDDEN);

    _calibration_bar = lv_bar_create(screen);
    lv_bar_set_range(_calibration_bar, 0, 100);
    lv_bar_set_value(_calibration_bar, 0, LV_ANIM_OFF);

    _calibration_label = lv_label_create(screen);
    lv_label_set_text(_calibration_label, "Keep the board level\nCalibrating 0%");
    lv_obj_set_style_text_color(_calibration_label, lv_color_hex(0xE7ECF5), LV_PART_MAIN);
    lv_obj_set_style_text_align(_calibration_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    _ball = lv_obj_create(screen);
    lv_obj_remove_flag(_ball, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(_ball, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(_ball, BALL_RADIUS * 2, BALL_RADIUS * 2);
    lv_obj_set_style_radius(_ball, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(_ball, lv_color_hex(0xFF4F70), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(_ball, lv_color_hex(0xFFB14E), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(_ball, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_border_width(_ball, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(_ball, lv_color_hex(0xFF4F70), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(_ball, 18, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(_ball, LV_OPA_50, LV_PART_MAIN);
    lv_obj_add_flag(_ball, LV_OBJ_FLAG_HIDDEN);

    layoutUi();
    _ui_timer = lv_timer_create(uiTimerCallback, SENSOR_PERIOD_MS, this);
}

bool Gravitysphere::refreshVisualGeometry(bool request_recalibration)
{
    const lv_area_t visual_area = getVisualArea();
    const int width = lv_area_get_width(&visual_area);
    const int height = lv_area_get_height(&visual_area);
    // Ball-center radius = visual radius - solid ball radius - glow margin.
    const int movement_radius = (std::min(width, height) / 2) - BALL_RADIUS - MOVEMENT_SAFE_MARGIN;
    if (width <= 0 || height <= 0 || movement_radius <= 0) {
        return false;
    }

    const bool changed = visual_area.x1 != _visual_area.x1 || visual_area.y1 != _visual_area.y1 ||
                         visual_area.x2 != _visual_area.x2 || visual_area.y2 != _visual_area.y2;
    if (!changed) {
        return true;
    }

    _visual_area = visual_area;
    _display_width = width;
    _display_height = height;
    _movement_center_x.store(width / 2);
    _movement_center_y.store(height / 2);
    _movement_radius.store(movement_radius);

    if (_ball != nullptr) {
        _ball_x.store(_movement_center_x.load());
        _ball_y.store(_movement_center_y.load());
        layoutUi();
    }

    if (request_recalibration && _running.load()) {
        _calibration_progress.store(0);
        _calibration_state.store(CalibrationState::RUNNING);
        _recalibration_requested.store(true);
        const TaskHandle_t task = _worker_task.load();
        if (task != nullptr) {
            xTaskNotifyGive(task);
        }
    }

    return true;
}

void Gravitysphere::layoutUi()
{
    const int center_x = _movement_center_x.load();
    const int center_y = _movement_center_y.load();
    const int movement_radius = _movement_radius.load();
    const int screen_radius = std::min(_display_width, _display_height) / 2;
    const int safe_half_extent = std::max(
        1,
        static_cast<int>((screen_radius - UI_SAFE_MARGIN) * 0.70710678F)
    );
    const int safe_width = std::max(1, (safe_half_extent * 2) - (UI_INNER_PADDING * 2));

    if (_movement_boundary != nullptr) {
        lv_obj_set_size(_movement_boundary, movement_radius * 2, movement_radius * 2);
        lv_obj_set_pos(
            _movement_boundary,
            center_x - movement_radius,
            center_y - movement_radius
        );
    }

    if (_hint_label != nullptr) {
        lv_obj_set_width(_hint_label, safe_width);
        lv_obj_set_pos(
            _hint_label,
            center_x - (safe_width / 2),
            center_y - safe_half_extent + UI_INNER_PADDING
        );
    }

    if (_calibration_bar != nullptr) {
        const int bar_width = std::max(1, std::min(280, safe_width - 24));
        lv_obj_set_size(_calibration_bar, bar_width, 18);
        lv_obj_set_pos(_calibration_bar, center_x - (bar_width / 2), center_y - 24);
    }

    if (_calibration_label != nullptr) {
        lv_obj_set_width(_calibration_label, safe_width);
        lv_obj_set_pos(_calibration_label, center_x - (safe_width / 2), center_y + 8);
    }

    if (_ball != nullptr) {
        lv_obj_set_pos(
            _ball,
            _ball_x.load() - BALL_RADIUS,
            _ball_y.load() - BALL_RADIUS
        );
    }
}

void Gravitysphere::screenEventCallback(lv_event_t *event)
{
    auto *app = static_cast<Gravitysphere *>(lv_event_get_user_data(event));
    if (app == nullptr) {
        return;
    }

    app->_recalibration_requested.store(true);
    const TaskHandle_t task = app->_worker_task.load();
    if (task != nullptr) {
        xTaskNotifyGive(task);
    }
}

void Gravitysphere::uiTimerCallback(lv_timer_t *timer)
{
    auto *app = static_cast<Gravitysphere *>(lv_timer_get_user_data(timer));
    if (app != nullptr) {
        app->updateUi();
    }
}

void Gravitysphere::updateUi()
{
    if (!refreshVisualGeometry(true)) {
        return;
    }

    if (_ball == nullptr || _calibration_label == nullptr || _calibration_bar == nullptr) {
        return;
    }

    const CalibrationState state = _calibration_state.load();
    const int progress = _calibration_progress.load();

    if (state != _displayed_state || progress != _displayed_progress) {
        switch (state) {
        case CalibrationState::RUNNING:
            lv_obj_add_flag(_ball, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(_hint_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(_calibration_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(_calibration_label, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(_calibration_bar, progress, LV_ANIM_OFF);
            lv_label_set_text_fmt(
                _calibration_label, "Keep the board level\nCalibrating %d%%", progress
            );
            break;
        case CalibrationState::DONE:
            lv_obj_add_flag(_calibration_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(_calibration_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(_hint_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(_ball, LV_OBJ_FLAG_HIDDEN);
            break;
        case CalibrationState::FAILED:
            lv_obj_add_flag(_ball, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(_calibration_label, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(_calibration_label, "Sensor read failed\nRetrying...");
            break;
        case CalibrationState::IDLE:
        default:
            break;
        }
        _displayed_state = state;
        _displayed_progress = progress;
    }

    if (state == CalibrationState::DONE) {
        // Brookesia already relocates the app screen to visual_area.x1/y1;
        // child coordinates are local to that resized screen.
        lv_obj_set_pos(
            _ball,
            _ball_x.load() - BALL_RADIUS,
            _ball_y.load() - BALL_RADIUS
        );
    }
}

void Gravitysphere::workerTask(void *arg)
{
    static_cast<Gravitysphere *>(arg)->workerLoop();
}

void Gravitysphere::workerLoop()
{
    int x = _movement_center_x.load();
    int y = _movement_center_y.load();

    while (_running.load()) {
        if (_paused.load()) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }

        const bool button_pressed = gpio_get_level(CALIBRATION_BUTTON_GPIO) == 0;
        if (button_pressed && !_button_was_pressed) {
            _recalibration_requested.store(true);
        }
        _button_was_pressed = button_pressed;

        if (_recalibration_requested.exchange(false) ||
                _calibration_state.load() != CalibrationState::DONE) {
            x = _movement_center_x.load();
            y = _movement_center_y.load();
            _ball_x.store(x);
            _ball_y.store(y);

            if (!performLevelCalibration()) {
                if (!_running.load()) {
                    break;
                }
                _calibration_state.store(CalibrationState::FAILED);
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
                continue;
            }
        }

        bool data_ready = false;
        qmi8658_data_t data = {};
        const esp_err_t ready_result = qmi8658_is_data_ready(&_imu, &data_ready);
        if (ready_result == ESP_OK && data_ready && qmi8658_read_sensor_data(&_imu, &data) == ESP_OK) {
            data.accelX -= _accel_bias_x;
            data.accelY -= _accel_bias_y;
            if (std::fabs(data.accelX) < CALIBRATION_DEADZONE) {
                data.accelX = 0.0F;
            }
            if (std::fabs(data.accelY) < CALIBRATION_DEADZONE) {
                data.accelY = 0.0F;
            }

            x -= static_cast<int>(data.accelY * ACCEL_SCALE_FACTOR);
            y += static_cast<int>(data.accelX * ACCEL_SCALE_FACTOR);
            constrainPosition(x, y);
            _ball_x.store(x);
            _ball_y.store(y);
        }

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SENSOR_PERIOD_MS));
    }

    _worker_task.store(nullptr);
    vTaskDelete(nullptr);
}

bool Gravitysphere::performLevelCalibration()
{
    _calibration_state.store(CalibrationState::RUNNING);
    bool have_samples = false;
    float last_bias_x = 0.0F;
    float last_bias_y = 0.0F;

    for (int attempt = 0; attempt < CALIBRATION_MAX_RETRIES && _running.load(); ++attempt) {
        float sum_x = 0.0F;
        float sum_y = 0.0F;
        float min_x = INFINITY;
        float max_x = -INFINITY;
        float min_y = INFINITY;
        float max_y = -INFINITY;
        int valid_samples = 0;

        for (int sample = 0; sample < CALIBRATION_SAMPLES && _running.load(); ++sample) {
            while (_paused.load() && _running.load()) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            }
            if (!_running.load()) {
                return false;
            }

            qmi8658_data_t data = {};
            if (qmi8658_read_sensor_data(&_imu, &data) == ESP_OK) {
                sum_x += data.accelX;
                sum_y += data.accelY;
                min_x = std::min(min_x, data.accelX);
                max_x = std::max(max_x, data.accelX);
                min_y = std::min(min_y, data.accelY);
                max_y = std::max(max_y, data.accelY);
                ++valid_samples;
            }

            _calibration_progress.store(((sample + 1) * 100) / CALIBRATION_SAMPLES);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        }

        if (valid_samples == 0) {
            ESP_UTILS_LOGW("Calibration attempt %d read no QMI8658 samples", attempt + 1);
            continue;
        }

        have_samples = true;
        last_bias_x = sum_x / valid_samples;
        last_bias_y = sum_y / valid_samples;
        const float range_x = max_x - min_x;
        const float range_y = max_y - min_y;

        if (range_x <= 0.1F && range_y <= 0.1F) {
            _accel_bias_x = last_bias_x;
            _accel_bias_y = last_bias_y;
            _calibration_state.store(CalibrationState::DONE);
            ESP_UTILS_LOGI(
                "QMI8658 calibrated: X %.4f m/s^2, Y %.4f m/s^2",
                _accel_bias_x,
                _accel_bias_y
            );
            return true;
        }

        ESP_UTILS_LOGW(
            "Calibration attempt %d was unstable (X %.3f, Y %.3f)",
            attempt + 1,
            range_x,
            range_y
        );
        _calibration_progress.store(0);
    }

    if (have_samples && _running.load()) {
        // Match the board's Immersive_block example: remain usable with the
        // latest real sample set when a perfectly still calibration is unavailable.
        _accel_bias_x = last_bias_x;
        _accel_bias_y = last_bias_y;
        _calibration_state.store(CalibrationState::DONE);
        ESP_UTILS_LOGW("Using the latest QMI8658 calibration sample set");
        return true;
    }

    return false;
}

void Gravitysphere::constrainPosition(int &x, int &y) const
{
    const int center_x = _movement_center_x.load();
    const int center_y = _movement_center_y.load();
    const int radius = _movement_radius.load();
    const int64_t delta_x = static_cast<int64_t>(x) - center_x;
    const int64_t delta_y = static_cast<int64_t>(y) - center_y;
    const int64_t distance_squared = (delta_x * delta_x) + (delta_y * delta_y);
    const int64_t radius_squared = static_cast<int64_t>(radius) * radius;

    if (radius <= 0) {
        x = center_x;
        y = center_y;
        return;
    }
    if (distance_squared <= radius_squared) {
        return;
    }

    const double scale = static_cast<double>(radius) /
                         std::sqrt(static_cast<double>(distance_squared));
    // Integer conversion truncates each component toward the center, keeping
    // the projected point on or just inside the circular boundary.
    x = center_x + static_cast<int>(static_cast<double>(delta_x) * scale);
    y = center_y + static_cast<int>(static_cast<double>(delta_y) * scale);
}

void Gravitysphere::releaseUi()
{
    if (_ui_timer != nullptr) {
        lv_timer_delete(_ui_timer);
        _ui_timer = nullptr;
    }

    // The Brookesia application screen owns and deletes these LVGL objects.
    _movement_boundary = nullptr;
    _ball = nullptr;
    _hint_label = nullptr;
    _calibration_label = nullptr;
    _calibration_bar = nullptr;
    _displayed_state = CalibrationState::IDLE;
    _displayed_progress = -1;
}

void Gravitysphere::stopWorker()
{
    _running.store(false);
    TaskHandle_t task = _worker_task.load();
    if (task == nullptr) {
        return;
    }

    xTaskNotifyGive(task);
    // The QMI8658 driver bounds each I2C transfer to 1000 ms.  A motion loop
    // can have a data-ready read followed by a sensor-data read in flight, so
    // allow both calls to time out and let the worker release the bus itself.
    for (int i = 0; i < 300 && _worker_task.load() != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    task = _worker_task.exchange(nullptr);
    if (task != nullptr) {
        ESP_UTILS_LOGE("Gravitysphere worker exceeded bounded I2C shutdown time; deleting it");
        vTaskDelete(task);
    }
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, Gravitysphere, APP_NAME, []() {
    return std::shared_ptr<Gravitysphere>(
        Gravitysphere::requestInstance(), [](Gravitysphere *) {}
    );
})

} // namespace esp_brookesia::apps
