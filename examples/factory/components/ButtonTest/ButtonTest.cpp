/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ButtonTest.hpp"

#include <algorithm>
#include <memory>

#include "esp_brookesia.hpp"
#include "esp_err.h"
#include "esp_lib_utils.h"

#ifdef ESP_UTILS_LOG_TAG
#undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:ButtonTest"

LV_IMG_DECLARE(img_app_button_test);

namespace esp_brookesia::apps {

namespace {

constexpr char APP_NAME[] = "Board I/O";

constexpr uint32_t COLOR_BACKGROUND_TOP = 0x0C111B;
constexpr uint32_t COLOR_CARD = 0x171F2D;
constexpr uint32_t COLOR_CARD_BORDER = 0x2C384B;
constexpr uint32_t COLOR_ACCENT = 0x62C8FF;
constexpr uint32_t COLOR_TEXT = 0xF4F7FF;
constexpr uint32_t COLOR_MUTED = 0xAAB8D1;
constexpr uint32_t COLOR_RELEASED = 0x34435D;
constexpr uint32_t COLOR_PRESSED = 0x11A98A;
constexpr uint32_t COLOR_ERROR = 0xD44C5C;
constexpr uint32_t COLOR_PASS = 0x66E0B5;
constexpr uint32_t COLOR_WAITING = 0x7E8CA6;
constexpr int REQUIRED_STABLE_SAMPLES = 3;

enum class TestPhase : uint8_t {
    WAIT_RELEASE,
    WAIT_PRESS,
    WAIT_FINAL_RELEASE,
};

struct ButtonSequence {
    TestPhase phase = TestPhase::WAIT_RELEASE;
    int candidate_level = -1;
    int level = -1;
    int stable_samples = 0;
};

void makePassive(lv_obj_t *object)
{
    if (object == nullptr) {
        return;
    }
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

bool updateButtonSequence(ButtonSequence &sequence, int raw_level, bool active_high)
{
    if (raw_level < 0) {
        sequence = {};
        return false;
    }

    const bool pressed = active_high ? raw_level == 1 : raw_level == 0;
    if (raw_level != sequence.candidate_level) {
        sequence.candidate_level = raw_level;
        sequence.stable_samples = 1;
    } else if (sequence.stable_samples < REQUIRED_STABLE_SAMPLES) {
        ++sequence.stable_samples;
    }

    if (sequence.stable_samples < REQUIRED_STABLE_SAMPLES) {
        return false;
    }

    sequence.level = raw_level;
    switch (sequence.phase) {
    case TestPhase::WAIT_RELEASE:
        if (!pressed) {
            sequence.phase = TestPhase::WAIT_PRESS;
        }
        break;
    case TestPhase::WAIT_PRESS:
        if (pressed) {
            sequence.phase = TestPhase::WAIT_FINAL_RELEASE;
        }
        break;
    case TestPhase::WAIT_FINAL_RELEASE:
        if (!pressed) {
            sequence.phase = TestPhase::WAIT_PRESS;
            return true;
        }
        break;
    }

    return false;
}

} // namespace

ButtonTest *ButtonTest::_instance = nullptr;

ButtonTest *ButtonTest::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new ButtonTest();
    }
    return _instance;
}

ButtonTest::ButtonTest()
    : App(APP_NAME, &img_app_button_test, true, false, false)
{
}

ButtonTest::~ButtonTest()
{
    stopWorker();
}

bool ButtonTest::init()
{
    // Hardware setup is deferred until the test opens.
    return true;
}

bool ButtonTest::deinit()
{
    stopWorker();
    releaseUi();
    return true;
}

bool ButtonTest::run()
{
    ESP_UTILS_CHECK_FALSE_RETURN(_worker_task.load() == nullptr, false, "Worker is already running");

    _visual_area = getVisualArea();
    _visual_width = lv_area_get_width(&_visual_area);
    _visual_height = lv_area_get_height(&_visual_area);
    ESP_UTILS_CHECK_FALSE_RETURN(
        _visual_width >= 360 && _visual_height >= 480,
        false,
        "Visual area is too small"
    );

    _boot_level.store(-1);
    _pwr_level.store(-1);
    _boot_count.store(0);
    _pwr_count.store(0);
    _reset_requested.store(false);
    _reset_sequence.store(false);
    if (!createUi()) {
        releaseUi();
        return false;
    }
    updateUi();

    _paused.store(false);
    _running.store(true);

    TaskHandle_t task = nullptr;
    const BaseType_t created = xTaskCreatePinnedToCore(
        workerTask, "button_test", 4096, this, 4, &task, 1
    );
    if (created != pdPASS) {
        _running.store(false);
        releaseUi();
        ESP_UTILS_LOGE("Create Button Test worker failed");
        return false;
    }
    _worker_task.store(task);
    return true;
}

bool ButtonTest::back()
{
    return notifyCoreClosed();
}

bool ButtonTest::close()
{
    stopWorker();
    releaseUi();
    return true;
}

bool ButtonTest::pause()
{
    _paused.store(true);
    _reset_sequence.store(true);
    if (_ui_timer != nullptr) {
        lv_timer_pause(_ui_timer);
    }
    return true;
}

bool ButtonTest::resume()
{
    _paused.store(false);
    if (_ui_timer != nullptr) {
        lv_timer_reset(_ui_timer);
        lv_timer_resume(_ui_timer);
    }
    return true;
}

bool ButtonTest::configureInput(gpio_num_t gpio, bool active_high)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        // BOOT is pulled up. PWR uses an external pull-up and an inverter;
        // disable internal pulls on SYS_OUT so the board circuit drives it.
        .pull_up_en = active_high ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const esp_err_t result = gpio_config(&config);
    if (result != ESP_OK) {
        ESP_UTILS_LOGE("Configure button GPIO%d failed: %s", static_cast<int>(gpio), esp_err_to_name(result));
    }
    return result == ESP_OK;
}

bool ButtonTest::createUi()
{
    _screen = lv_screen_active();
    ESP_UTILS_CHECK_NULL_RETURN(_screen, false, "Get active screen failed");
    makePassive(_screen);
    lv_obj_set_style_pad_all(_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(_screen, lv_color_hex(COLOR_BACKGROUND_TOP), LV_PART_MAIN);

    auto label = [](lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color) {
        lv_obj_t *object = lv_label_create(parent);
        if (object != nullptr) {
            makePassive(object);
            lv_label_set_text(object, text);
            lv_obj_set_style_text_font(object, font, LV_PART_MAIN);
            lv_obj_set_style_text_color(object, lv_color_hex(color), LV_PART_MAIN);
        }
        return object;
    };
    _title_label = label(_screen, "Button test", &lv_font_montserrat_28, COLOR_TEXT);
    _subtitle_label = label(_screen, "Press and release each physical button", &lv_font_montserrat_14, COLOR_MUTED);
    _boot_widgets = createButtonCard(_screen, "BOOT", "Boot button");
    _pwr_widgets = createButtonCard(_screen, "PWR", "Power button");

    _result_panel = lv_obj_create(_screen);
    ESP_UTILS_CHECK_NULL_RETURN(_result_panel, false, "Create result panel failed");
    makePassive(_result_panel);
    lv_obj_set_style_pad_all(_result_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(_result_panel, 18, LV_PART_MAIN);
    lv_obj_set_style_border_width(_result_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(_result_panel, lv_color_hex(COLOR_CARD), LV_PART_MAIN);
    _result_label = label(_result_panel, "", &lv_font_montserrat_18, COLOR_TEXT);
    _result_hint = label(_result_panel, "", &lv_font_montserrat_14, COLOR_MUTED);
    _progress_bar = lv_bar_create(_result_panel);
    ESP_UTILS_CHECK_NULL_RETURN(_progress_bar, false, "Create progress bar failed");
    lv_bar_set_range(_progress_bar, 0, 2);
    lv_obj_set_style_bg_color(_progress_bar, lv_color_hex(COLOR_CARD_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_progress_bar, lv_color_hex(COLOR_PASS), LV_PART_INDICATOR);
    lv_obj_set_style_radius(_progress_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(_progress_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);

    _reset_button = lv_button_create(_screen);
    ESP_UTILS_CHECK_NULL_RETURN(_reset_button, false, "Create reset button failed");
    lv_obj_set_style_radius(_reset_button, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(_reset_button, lv_color_hex(0x243C50), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_reset_button, lv_color_hex(0x305773), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(_reset_button, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(_reset_button, resetEventCallback, LV_EVENT_CLICKED, this);
    lv_obj_t *reset_label = label(_reset_button, LV_SYMBOL_REFRESH "  Test again", &lv_font_montserrat_16, COLOR_ACCENT);
    ESP_UTILS_CHECK_NULL_RETURN(reset_label, false, "Create reset label failed");
    lv_obj_center(reset_label);
    _warning_label = label(_screen, "Short press PWR to test.\nA long hold may power off the board.", &lv_font_montserrat_14, COLOR_MUTED);
    ESP_UTILS_CHECK_FALSE_RETURN(
        _title_label && _subtitle_label && _boot_widgets.pass_label &&
        _pwr_widgets.pass_label && _result_label && _result_hint && _warning_label,
        false, "Create Button Test labels failed"
    );
    layoutUi();
    _ui_timer = lv_timer_create(uiTimerCallback, UI_PERIOD_MS, this);
    return _ui_timer != nullptr;
}

ButtonTest::ButtonWidgets ButtonTest::createButtonCard(
    lv_obj_t *parent, const char *name, const char *subtitle)
{
    ButtonWidgets widgets = {};
    widgets.card = lv_obj_create(parent);
    if (widgets.card == nullptr) {
        return widgets;
    }
    makePassive(widgets.card);
    lv_obj_set_style_pad_all(widgets.card, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(widgets.card, 20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(widgets.card, lv_color_hex(COLOR_CARD), LV_PART_MAIN);
    lv_obj_set_style_border_width(widgets.card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(widgets.card, lv_color_hex(COLOR_CARD_BORDER), LV_PART_MAIN);

    lv_obj_t *name_label = lv_label_create(widgets.card);
    lv_obj_t *subtitle_label = lv_label_create(widgets.card);
    if (name_label == nullptr || subtitle_label == nullptr) {
        return widgets;
    }
    makePassive(name_label);
    makePassive(subtitle_label);
    lv_label_set_text(name_label, name);
    lv_label_set_text(subtitle_label, subtitle);
    lv_obj_set_style_text_font(name_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_font(subtitle_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(name_label, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_color(subtitle_label, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
    lv_obj_set_pos(name_label, 18, 14);
    lv_obj_set_pos(subtitle_label, 18, 44);

    widgets.state_panel = lv_obj_create(widgets.card);
    if (widgets.state_panel == nullptr) {
        return widgets;
    }
    makePassive(widgets.state_panel);
    lv_obj_set_size(widgets.state_panel, 126, 34);
    lv_obj_align(widgets.state_panel, LV_ALIGN_TOP_RIGHT, -16, 14);
    lv_obj_set_style_pad_all(widgets.state_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(widgets.state_panel, 10, LV_PART_MAIN);
    lv_obj_set_style_border_width(widgets.state_panel, 0, LV_PART_MAIN);
    widgets.state_label = lv_label_create(widgets.state_panel);
    widgets.count_label = lv_label_create(widgets.card);
    widgets.pass_label = lv_label_create(widgets.card);
    if (widgets.state_label == nullptr || widgets.count_label == nullptr || widgets.pass_label == nullptr) {
        widgets.pass_label = nullptr;
        return widgets;
    }
    for (auto *object : {widgets.state_label, widgets.count_label, widgets.pass_label}) {
        makePassive(object);
        lv_obj_set_style_text_font(object, &lv_font_montserrat_14, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(widgets.count_label, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
    lv_obj_center(widgets.state_label);
    lv_obj_align(widgets.count_label, LV_ALIGN_TOP_LEFT, 18, 74);
    lv_obj_align(widgets.pass_label, LV_ALIGN_TOP_RIGHT, -16, 74);
    return widgets;
}

void ButtonTest::layoutUi()
{
    // Local coordinates: Brookesia already positions the application screen.
    // Keep controls clear of the AMOLED's rounded corners without scrolling.
    const int width = std::min(_visual_width - 48, 380);
    const int left = (_visual_width - width) / 2;
    const int top = std::max(3, (_visual_height - 474) / 2);
    lv_obj_align(_title_label, LV_ALIGN_TOP_MID, 0, top);
    lv_obj_align(_subtitle_label, LV_ALIGN_TOP_MID, 0, top + 40);
    lv_obj_set_size(_boot_widgets.card, width, 100);
    lv_obj_set_pos(_boot_widgets.card, left, top + 76);
    lv_obj_set_size(_pwr_widgets.card, width, 100);
    lv_obj_set_pos(_pwr_widgets.card, left, top + 188);
    lv_obj_set_size(_result_panel, width, 76);
    lv_obj_set_pos(_result_panel, left, top + 300);
    lv_obj_align(_result_label, LV_ALIGN_TOP_LEFT, 16, 10);
    lv_obj_align(_result_hint, LV_ALIGN_TOP_LEFT, 16, 35);
    lv_obj_set_size(_progress_bar, width - 32, 5);
    lv_obj_align(_progress_bar, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_size(_reset_button, width - 40, 42);
    lv_obj_set_pos(_reset_button, left + 20, top + 388);
    lv_obj_set_width(_warning_label, width);
    lv_obj_set_style_text_align(_warning_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(_warning_label, LV_ALIGN_TOP_MID, 0, top + 440);
}

void ButtonTest::updateUi()
{
    const int boot_level = _boot_level.load();
    const int pwr_level = _pwr_level.load();
    const uint32_t boot_count = _boot_count.load();
    const uint32_t pwr_count = _pwr_count.load();
    updateButtonCard(_boot_widgets, boot_level, false, boot_count);
    updateButtonCard(_pwr_widgets, pwr_level, true, pwr_count);

    const bool error = boot_level < 0 || pwr_level < 0;
    const int passed = (boot_count > 0 ? 1 : 0) + (pwr_count > 0 ? 1 : 0);
    // Cache both completion bits, so the next-button hint changes correctly.
    const int result = (boot_count > 0 ? 1 : 0) | (pwr_count > 0 ? 2 : 0) | (error ? 4 : 0);
    if (result == _displayed_result) {
        return;
    }
    _displayed_result = result;
    lv_bar_set_value(_progress_bar, passed, LV_ANIM_OFF);
    lv_label_set_text_fmt(_result_label, "%d / 2 buttons passed", passed);
    lv_obj_set_style_text_color(_result_label, lv_color_hex(error ? COLOR_ERROR :
                                passed == 2 ? COLOR_PASS : COLOR_TEXT), LV_PART_MAIN);
    lv_label_set_text(_result_hint, error ? "Waiting for button inputs..." :
                      passed == 2 ? "All done. Both buttons work correctly." :
                      boot_count > 0 ? "Next: press and release PWR." :
                      pwr_count > 0 ? "Next: press and release BOOT." :
                      "Ready. Start with either button.");
}

void ButtonTest::updateButtonCard(ButtonWidgets &widgets, int level, bool active_high, uint32_t count)
{
    const bool level_changed = widgets.displayed_level != level;
    if (level_changed) {
        widgets.displayed_level = level;
        const bool pressed = level >= 0 && (active_high ? level == 1 : level == 0);
        lv_label_set_text(widgets.state_label, level < 0 ? "UNAVAILABLE" : pressed ? "PRESSED" : "RELEASED");
        lv_obj_set_style_bg_color(widgets.state_panel,
                                 lv_color_hex(pressed ? COLOR_PRESSED : COLOR_RELEASED), LV_PART_MAIN);
        lv_obj_set_style_text_color(widgets.state_label,
                                   lv_color_hex(level < 0 ? COLOR_ERROR : pressed ? COLOR_PASS : COLOR_TEXT), LV_PART_MAIN);
        lv_obj_set_style_border_color(widgets.card,
                                     lv_color_hex(pressed ? COLOR_PASS : COLOR_CARD_BORDER), LV_PART_MAIN);
    }
    if (widgets.displayed_count != count || level_changed) {
        widgets.displayed_count = count;
        lv_label_set_text_fmt(widgets.count_label, "Presses: %lu", static_cast<unsigned long>(count));
        lv_label_set_text(widgets.pass_label, level < 0 ? "CHECK INPUT" : count > 0 ? LV_SYMBOL_OK " PASS" : "NOT TESTED");
        lv_obj_set_style_text_color(widgets.pass_label,
                                   lv_color_hex(level < 0 ? COLOR_ERROR : count > 0 ? COLOR_PASS : COLOR_WAITING), LV_PART_MAIN);
    }
}

void ButtonTest::uiTimerCallback(lv_timer_t *timer)
{
    auto *app = static_cast<ButtonTest *>(lv_timer_get_user_data(timer));
    if (app != nullptr) {
        app->updateUi();
    }
}

void ButtonTest::resetEventCallback(lv_event_t *event)
{
    auto *app = static_cast<ButtonTest *>(lv_event_get_user_data(event));
    if (app != nullptr) {
        // Only the sampling worker changes the sequences and counters.
        app->_reset_requested.store(true);
    }
}

void ButtonTest::workerTask(void *arg)
{
    static_cast<ButtonTest *>(arg)->workerLoop();
}

void ButtonTest::workerLoop()
{
    bool boot_configured = false;
    bool pwr_configured = false;
    ButtonSequence boot_sequence = {};
    ButtonSequence pwr_sequence = {};
    TickType_t next_retry = xTaskGetTickCount();

    while (_running.load()) {
        if (_paused.load()) {
            vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
            continue;
        }
        const bool reset_test = _reset_requested.exchange(false);
        if (_reset_sequence.exchange(false) || reset_test) {
            boot_sequence = {};
            pwr_sequence = {};
            if (reset_test) {
                _boot_count.store(0);
                _pwr_count.store(0);
            }
        }
        const TickType_t now = xTaskGetTickCount();
        if ((!boot_configured || !pwr_configured) && static_cast<int32_t>(now - next_retry) >= 0) {
            if (!boot_configured) {
                boot_configured = configureInput(BOOT_GPIO, false);
            }
            if (!pwr_configured) {
                pwr_configured = configureInput(PWR_GPIO, true);
            }
            next_retry = now + pdMS_TO_TICKS(INPUT_RETRY_PERIOD_MS);
        }

        // Count complete debounced released -> pressed -> released cycles.
        // A stuck pin or a button held on entry cannot pass the test.
        if (updateButtonSequence(boot_sequence, boot_configured ? gpio_get_level(BOOT_GPIO) : -1, false)) {
            if (_boot_count.load() < 9999) {
                _boot_count.fetch_add(1);
            }
            ESP_UTILS_LOGI("BOOT button passed");
        }
        if (updateButtonSequence(pwr_sequence, pwr_configured ? gpio_get_level(PWR_GPIO) : -1, true)) {
            if (_pwr_count.load() < 9999) {
                _pwr_count.fetch_add(1);
            }
            ESP_UTILS_LOGI("PWR button passed");
        }
        _boot_level.store(boot_sequence.level);
        _pwr_level.store(pwr_sequence.level);
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
    _worker_task.store(nullptr);
    vTaskDelete(nullptr);
}

void ButtonTest::releaseUi()
{
    if (_ui_timer != nullptr) {
        lv_timer_delete(_ui_timer);
        _ui_timer = nullptr;
    }

    // Brookesia owns and deletes the application screen and all child objects.
    _screen = nullptr;
    _title_label = nullptr;
    _subtitle_label = nullptr;
    _boot_widgets = {};
    _pwr_widgets = {};
    _result_panel = nullptr;
    _result_hint = nullptr;
    _progress_bar = nullptr;
    _reset_button = nullptr;
    _displayed_result = -2;
    _result_label = nullptr;
    _warning_label = nullptr;
}

void ButtonTest::stopWorker()
{
    _running.store(false);
    for (int i = 0; i < 25 && _worker_task.load() != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (_worker_task.load() != nullptr) {
        // The worker never touches LVGL and owns its self-delete path.
        ESP_UTILS_LOGW("Board I/O worker is still leaving");
    }
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, ButtonTest, APP_NAME, []() {
    return std::shared_ptr<ButtonTest>(
        ButtonTest::requestInstance(), [](ButtonTest *) {}
    );
})

} // namespace esp_brookesia::apps
