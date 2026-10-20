/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 * SPDX-License-Identifier: Apache-2.0
 */
#include "VibrationTest.hpp"

#include <algorithm>
#include <memory>

#include "esp_brookesia.hpp"
#include "esp_lib_utils.h"

#ifdef ESP_UTILS_LOG_TAG
#undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:VibrationTest"

LV_IMG_DECLARE(img_app_vibration);

namespace esp_brookesia::apps {
namespace {

constexpr char APP_NAME[] = "Vibration";
constexpr uint32_t COLOR_BACKGROUND = 0x0C111B;
constexpr uint32_t COLOR_TEXT = 0xF4F7FF;
constexpr uint32_t COLOR_MUTED = 0xAAB8D1;
constexpr uint32_t COLOR_ACCENT = 0x62C8FF;
constexpr uint32_t COLOR_ACTIVE = 0x66E0B5;
constexpr uint32_t COLOR_ERROR = 0xFB7185;

lv_obj_t *createLabel(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    if (label != nullptr) {
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    }
    return label;
}

} // namespace

VibrationTest *VibrationTest::_instance = nullptr;

VibrationTest *VibrationTest::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new VibrationTest();
    }
    return _instance;
}

VibrationTest::VibrationTest()
    : App(APP_NAME, &img_app_vibration, true, false, false)
{
}

VibrationTest::~VibrationTest()
{
    stopWorker();
    if (_worker_task.load() == nullptr && _motor_mutex != nullptr) {
        vSemaphoreDelete(_motor_mutex);
        _motor_mutex = nullptr;
    }
}

bool VibrationTest::init()
{
    // Defer GPIO setup until the app opens.
    if (_motor_mutex == nullptr) {
        _motor_mutex = xSemaphoreCreateMutex();
    }
    return _motor_mutex != nullptr;
}

bool VibrationTest::deinit()
{
    stopWorker();
    releaseUi();
    return true;
}

bool VibrationTest::run()
{
    ESP_UTILS_CHECK_FALSE_RETURN(_worker_task.load() == nullptr, false, "Worker is already running");
    ESP_UTILS_CHECK_NULL_RETURN(_motor_mutex, false, "Motor mutex is unavailable");
    const lv_area_t area = getVisualArea();
    const int width = lv_area_get_width(&area);
    const int height = lv_area_get_height(&area);
    ESP_UTILS_CHECK_FALSE_RETURN(width >= 360 && height >= 480, false, "Visual area is too small");

    forceMotorOff();
    _pulse_count.store(0);
    _touch_held = false;
    if (!createUi(width, height)) {
        releaseUi();
        return false;
    }
    updateUi();
    _paused.store(false);
    _running.store(true);
    TaskHandle_t task = nullptr;
    if (xTaskCreatePinnedToCore(workerTask, "vibration_test", 3072, this, 4, &task, 1) != pdPASS) {
        _running.store(false);
        forceMotorOff();
        releaseUi();
        return false;
    }
    _worker_task.store(task);
    return true;
}

bool VibrationTest::back()
{
    return notifyCoreClosed();
}

bool VibrationTest::close()
{
    stopWorker();
    releaseUi();
    return true;
}

bool VibrationTest::pause()
{
    _paused.store(true);
    forceMotorOff();
    _touch_held = false;
    if (_motor_button != nullptr) {
        lv_obj_remove_state(_motor_button, LV_STATE_PRESSED);
    }
    if (_ui_timer != nullptr) {
        lv_timer_pause(_ui_timer);
    }
    return true;
}

bool VibrationTest::resume()
{
    _paused.store(false);
    updateUi();
    if (_ui_timer != nullptr) {
        lv_timer_reset(_ui_timer);
        lv_timer_resume(_ui_timer);
    }
    return true;
}

bool VibrationTest::createUi(int width, int height)
{
    _screen = lv_screen_active();
    ESP_UTILS_CHECK_NULL_RETURN(_screen, false, "Get app screen failed");
    lv_obj_remove_flag(_screen, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(_screen, lv_color_hex(COLOR_BACKGROUND), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_screen, LV_OPA_COVER, LV_PART_MAIN);

    const int top = std::max(3, (height - 474) / 2);
    lv_obj_t *title = createLabel(_screen, "Vibration test", &lv_font_montserrat_28, COLOR_TEXT);
    lv_obj_t *subtitle = createLabel(_screen, "Feel the motor respond to your touch", &lv_font_montserrat_14, COLOR_MUTED);
    ESP_UTILS_CHECK_FALSE_RETURN(title && subtitle, false, "Create title failed");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, top + 8);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, top + 48);

    _motor_button = lv_button_create(_screen);
    ESP_UTILS_CHECK_NULL_RETURN(_motor_button, false, "Create motor button failed");
    lv_obj_set_size(_motor_button, 184, 184);
    lv_obj_align(_motor_button, LV_ALIGN_TOP_MID, 0, top + 98);
    lv_obj_set_style_radius(_motor_button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(_motor_button, lv_color_hex(0x172B3D), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_motor_button, lv_color_hex(0x1E4B5E), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(_motor_button, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(_motor_button, lv_color_hex(0x305773), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(_motor_button, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(_motor_button, motorEventCallback, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(_motor_button, motorEventCallback, LV_EVENT_RELEASED, this);
    lv_obj_add_event_cb(_motor_button, motorEventCallback, LV_EVENT_PRESS_LOST, this);

    lv_obj_t *image = lv_image_create(_motor_button);
    ESP_UTILS_CHECK_NULL_RETURN(image, false, "Create motor image failed");
    lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(image, &img_app_vibration);
    lv_obj_align(image, LV_ALIGN_CENTER, 0, -18);
    _button_label = createLabel(_motor_button, "HOLD", &lv_font_montserrat_18, COLOR_ACCENT);
    _state_label = createLabel(_screen, "", &lv_font_montserrat_22, COLOR_TEXT);
    _hint_label = createLabel(_screen, "", &lv_font_montserrat_14, COLOR_MUTED);
    _count_label = createLabel(_screen, "", &lv_font_montserrat_16, COLOR_MUTED);
    lv_obj_t *footer = createLabel(_screen, "Each press is limited to 250 ms.\nRelease, then press again to repeat.", &lv_font_montserrat_14, COLOR_MUTED);
    ESP_UTILS_CHECK_FALSE_RETURN(
        _button_label && _state_label && _hint_label && _count_label && footer,
        false, "Create motor labels failed"
    );
    lv_obj_align(_button_label, LV_ALIGN_CENTER, 0, 56);
    lv_obj_align(_state_label, LV_ALIGN_TOP_MID, 0, top + 304);
    lv_obj_set_width(_hint_label, width - 64);
    lv_obj_set_style_text_align(_hint_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(_hint_label, LV_ALIGN_TOP_MID, 0, top + 338);
    lv_obj_align(_count_label, LV_ALIGN_TOP_MID, 0, top + 394);
    lv_obj_set_width(footer, width - 64);
    lv_obj_set_style_text_align(footer, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(footer, LV_ALIGN_TOP_MID, 0, top + 434);
    _ui_timer = lv_timer_create(uiTimerCallback, UI_PERIOD_MS, this);
    return _ui_timer != nullptr;
}

void VibrationTest::updateUi()
{
    const bool ready = _motor_ready.load();
    const bool active = _motor_active.load();
    const bool error = _motor_error.load() != ESP_OK;
    const int state = error ? 4 : !ready ? 0 : active ? 2 : _touch_held ? 3 : 1;
    if (state != _displayed_state) {
        _displayed_state = state;
        const char *states[] = {"Preparing motor...", "Ready", "Vibrating", "Pulse complete", "Motor unavailable"};
        const char *hints[] = {
            "Please wait while the motor is initialized.",
            "Press and hold the circle to test.",
            "Release your finger to stop early.",
            "Release your finger before the next test.",
            "GPIO18 could not be driven. Retrying...",
        };
        const uint32_t color = error ? COLOR_ERROR : active ? COLOR_ACTIVE : COLOR_ACCENT;
        lv_label_set_text(_state_label, states[state]);
        lv_label_set_text(_hint_label, hints[state]);
        lv_label_set_text(_button_label, active ? "VIBRATING" : _touch_held ? "RELEASE" : "HOLD");
        lv_obj_set_style_text_color(_state_label, lv_color_hex(color), LV_PART_MAIN);
        lv_obj_set_style_border_color(_motor_button, lv_color_hex(color), LV_PART_MAIN);
        if (ready && !error) {
            lv_obj_remove_state(_motor_button, LV_STATE_DISABLED);
        } else {
            _touch_held = false;
            lv_obj_add_state(_motor_button, LV_STATE_DISABLED);
        }
    }
    const uint32_t count = _pulse_count.load();
    if (count != _displayed_count) {
        _displayed_count = count;
        // Counts successful GPIO activations; the user verifies actual vibration.
        lv_label_set_text_fmt(_count_label, "Pulses sent: %lu", static_cast<unsigned long>(count));
    }
}

bool VibrationTest::configureMotorOutput()
{
    // Preload a low output latch before enabling the GPIO driver.
    esp_err_t result = gpio_set_level(MOTOR_GPIO, 0);
    if (result == ESP_OK) {
        const gpio_config_t config = {
            .pin_bit_mask = 1ULL << MOTOR_GPIO,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        result = gpio_config(&config);
        if (result == ESP_OK) {
            _motor_configured = true;
            result = gpio_set_level(MOTOR_GPIO, 0);
        }
    }
    _motor_error.store(result);
    _motor_ready.store(result == ESP_OK);
    if (result != ESP_OK) {
        ESP_UTILS_LOGE("Configure motor GPIO18 failed: %s", esp_err_to_name(result));
    }
    return result == ESP_OK;
}

void VibrationTest::requestMotor(bool pressed)
{
    if (xSemaphoreTake(_motor_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    _motor_requested = pressed && _running.load() && !_paused.load() && _motor_ready.load();
    if (_motor_requested) {
        _motor_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(MOTOR_MAX_ON_MS);
    }
    xSemaphoreGive(_motor_mutex);
    if (!pressed) {
        forceMotorOff();
    }
}

void VibrationTest::updateMotorOutput(TickType_t now)
{
    if (xSemaphoreTake(_motor_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (_motor_requested && static_cast<int32_t>(now - _motor_deadline) >= 0) {
        _motor_requested = false;
    }
    // Check every gate inside the same lock used by pause/stop and touch events.
    const bool should_run = _running.load() && !_paused.load() &&
                            _motor_ready.load() && _motor_requested;
    if (_motor_configured && (_motor_active.load() != should_run || _motor_error.load() != ESP_OK)) {
        const esp_err_t result = gpio_set_level(MOTOR_GPIO, should_run ? 1 : 0);
        _motor_error.store(result);
        _motor_ready.store(result == ESP_OK);
        if (result == ESP_OK) {
            if (should_run && !_motor_active.load() && _pulse_count.load() < 9999) {
                _pulse_count.fetch_add(1);
            }
            _motor_active.store(should_run);
        } else {
            _motor_requested = false;
            ESP_UTILS_LOGE("Drive motor GPIO18 failed: %s", esp_err_to_name(result));
        }
    }
    xSemaphoreGive(_motor_mutex);
}

void VibrationTest::forceMotorOff()
{
    if (_motor_mutex == nullptr || xSemaphoreTake(_motor_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    _motor_requested = false;
    if (_motor_configured) {
        const esp_err_t result = gpio_set_level(MOTOR_GPIO, 0);
        _motor_error.store(result);
        _motor_ready.store(result == ESP_OK);
        if (result == ESP_OK) {
            _motor_active.store(false);
        } else {
            ESP_UTILS_LOGE("Stop motor GPIO18 failed: %s", esp_err_to_name(result));
        }
    }
    xSemaphoreGive(_motor_mutex);
}

void VibrationTest::motorEventCallback(lv_event_t *event)
{
    auto *app = static_cast<VibrationTest *>(lv_event_get_user_data(event));
    if (app == nullptr) {
        return;
    }
    app->_touch_held = lv_event_get_code(event) == LV_EVENT_PRESSED;
    app->requestMotor(app->_touch_held);
    if (!app->_touch_held) {
        app->updateUi();
    }
}

void VibrationTest::uiTimerCallback(lv_timer_t *timer)
{
    auto *app = static_cast<VibrationTest *>(lv_timer_get_user_data(timer));
    if (app != nullptr) {
        app->updateUi();
    }
}

void VibrationTest::workerTask(void *arg)
{
    static_cast<VibrationTest *>(arg)->workerLoop();
}

void VibrationTest::workerLoop()
{
    TickType_t next_retry = xTaskGetTickCount();
    while (_running.load()) {
        const TickType_t now = xTaskGetTickCount();
        if (!_motor_ready.load() && !_paused.load() && static_cast<int32_t>(now - next_retry) >= 0) {
            if (xSemaphoreTake(_motor_mutex, portMAX_DELAY) == pdTRUE) {
                configureMotorOutput();
                xSemaphoreGive(_motor_mutex);
            }
            next_retry = now + pdMS_TO_TICKS(RETRY_PERIOD_MS);
        }
        updateMotorOutput(now);
        vTaskDelay(pdMS_TO_TICKS(WORKER_PERIOD_MS));
    }
    forceMotorOff();
    _worker_task.store(nullptr);
    vTaskDelete(nullptr);
}

void VibrationTest::stopWorker()
{
    _running.store(false);
    forceMotorOff();
    for (int i = 0; i < 25 && _worker_task.load() != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    forceMotorOff();
    if (_worker_task.load() != nullptr) {
        ESP_UTILS_LOGW("Vibration worker is still leaving");
    }
}

void VibrationTest::releaseUi()
{
    if (_ui_timer != nullptr) {
        lv_timer_delete(_ui_timer);
        _ui_timer = nullptr;
    }
    // Brookesia owns and deletes the app screen and all children.
    _screen = nullptr;
    _motor_button = nullptr;
    _button_label = nullptr;
    _state_label = nullptr;
    _hint_label = nullptr;
    _count_label = nullptr;
    _touch_held = false;
    _displayed_state = -1;
    _displayed_count = UINT32_MAX;
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, VibrationTest, APP_NAME, []() {
    return std::shared_ptr<VibrationTest>(VibrationTest::requestInstance(), [](VibrationTest *) {});
})

} // namespace esp_brookesia::apps
