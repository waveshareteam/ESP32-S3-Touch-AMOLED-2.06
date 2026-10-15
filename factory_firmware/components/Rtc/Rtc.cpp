/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Rtc.hpp"

#include <ctime>
#include <memory>

#include "bsp/esp-bsp.h"
#include "esp_brookesia.hpp"
#include "esp_err.h"
#include "esp_lib_utils.h"

#ifdef ESP_UTILS_LOG_TAG
#undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:Rtc"

namespace esp_brookesia::apps {

namespace {

constexpr char APP_NAME[] = "RTC";
constexpr uint32_t COLOR_BACKGROUND = 0x0D1424;
constexpr uint32_t COLOR_CARD = 0x1A2540;
constexpr uint32_t COLOR_TEXT = 0xF4F7FF;
constexpr uint32_t COLOR_MUTED = 0xAAB8D1;
constexpr uint32_t COLOR_ACCENT = 0x4C9AFF;
constexpr uint32_t COLOR_ERROR = 0xFF7285;
constexpr uint32_t COLOR_SUCCESS = 0x68D7A8;

void makePassive(lv_obj_t *object)
{
    if (object == nullptr) {
        return;
    }
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

void setButtonStyle(lv_obj_t *button, lv_color_t color)
{
    lv_obj_set_style_bg_color(button, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 14, LV_PART_MAIN);
}

} // namespace

Rtc *Rtc::_instance = nullptr;

Rtc *Rtc::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new Rtc();
    }
    return _instance;
}

Rtc::Rtc()
    : App(APP_NAME, nullptr, true, false, false)
{
}

bool Rtc::init()
{
    const i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    ESP_UTILS_CHECK_NULL_RETURN(bus, false, "Get shared board I2C bus failed");

    const esp_err_t result = pcf85063a_init(&_rtc, bus, PCF85063A_ADDRESS);
    if (result != ESP_OK) {
        if (_rtc.dev_handle != nullptr) {
            i2c_master_bus_rm_device(_rtc.dev_handle);
            _rtc = {};
        }
        ESP_UTILS_LOGE("Initialize PCF85063A failed: %s", esp_err_to_name(result));
        return false;
    }

    _rtc_initialized = true;
    return true;
}

bool Rtc::deinit()
{
    releaseUi();
    _rtc_initialized = false;
    if (_rtc.dev_handle != nullptr) {
        const esp_err_t result = i2c_master_bus_rm_device(_rtc.dev_handle);
        if (result != ESP_OK) {
            ESP_UTILS_LOGW("Remove PCF85063A I2C device failed: %s", esp_err_to_name(result));
        }
        _rtc = {};
    }
    return true;
}

bool Rtc::run()
{
    ESP_UTILS_CHECK_FALSE_RETURN(_rtc_initialized, false, "RTC is not initialized");
    ESP_UTILS_CHECK_FALSE_RETURN(createUi(), false, "Create RTC UI failed");

    refreshRtc();
    _refresh_timer = lv_timer_create(refreshTimerCallback, 1000, this);
    if (_refresh_timer == nullptr) {
        ESP_UTILS_LOGE("Create RTC refresh timer failed");
        releaseUi();
        return false;
    }
    return true;
}

bool Rtc::back()
{
    return notifyCoreClosed();
}

bool Rtc::close()
{
    releaseUi();
    return true;
}

bool Rtc::pause()
{
    if (_refresh_timer != nullptr) {
        lv_timer_pause(_refresh_timer);
    }
    return true;
}

bool Rtc::resume()
{
    if (_refresh_timer != nullptr) {
        lv_timer_reset(_refresh_timer);
        lv_timer_resume(_refresh_timer);
    }
    return true;
}

bool Rtc::createUi()
{
    _screen = lv_screen_active();
    ESP_UTILS_CHECK_NULL_RETURN(_screen, false, "Get active screen failed");

    lv_obj_remove_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(_screen, lv_color_hex(COLOR_BACKGROUND), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(_screen, 16, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(_screen);
    ESP_UTILS_CHECK_NULL_RETURN(title, false, "Create title failed");
    makePassive(title);
    lv_label_set_text(title, "Real-time clock");
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *subtitle = lv_label_create(_screen);
    ESP_UTILS_CHECK_NULL_RETURN(subtitle, false, "Create subtitle failed");
    makePassive(subtitle);
    lv_label_set_text(subtitle, "PCF85063A on the shared I2C bus");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align_to(subtitle, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    lv_obj_t *card = lv_obj_create(_screen);
    ESP_UTILS_CHECK_NULL_RETURN(card, false, "Create time card failed");
    makePassive(card);
    lv_obj_set_size(card, LV_PCT(100), 182);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 82);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 20, LV_PART_MAIN);

    _time_label = lv_label_create(card);
    ESP_UTILS_CHECK_NULL_RETURN(_time_label, false, "Create time label failed");
    makePassive(_time_label);
    lv_obj_set_width(_time_label, LV_PCT(100));
    lv_obj_set_style_text_align(_time_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(_time_label, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(_time_label, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_align(_time_label, LV_ALIGN_CENTER, 0, -26);

    _date_label = lv_label_create(card);
    ESP_UTILS_CHECK_NULL_RETURN(_date_label, false, "Create date label failed");
    makePassive(_date_label);
    lv_obj_set_width(_date_label, LV_PCT(100));
    lv_obj_set_style_text_align(_date_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(_date_label, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(_date_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_align(_date_label, LV_ALIGN_CENTER, 0, 38);

    lv_obj_t *refresh_button = lv_button_create(_screen);
    ESP_UTILS_CHECK_NULL_RETURN(refresh_button, false, "Create refresh button failed");
    lv_obj_set_size(refresh_button, LV_PCT(100), 54);
    lv_obj_align(refresh_button, LV_ALIGN_TOP_MID, 0, 282);
    setButtonStyle(refresh_button, lv_color_hex(COLOR_ACCENT));
    lv_obj_add_event_cb(refresh_button, refreshButtonCallback, LV_EVENT_CLICKED, this);
    lv_obj_t *refresh_label = lv_label_create(refresh_button);
    ESP_UTILS_CHECK_NULL_RETURN(refresh_label, false, "Create refresh button label failed");
    makePassive(refresh_label);
    lv_label_set_text(refresh_label, "Refresh RTC");
    lv_obj_set_style_text_color(refresh_label, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_center(refresh_label);

    lv_obj_t *set_button = lv_button_create(_screen);
    ESP_UTILS_CHECK_NULL_RETURN(set_button, false, "Create system-time button failed");
    lv_obj_set_size(set_button, LV_PCT(100), 54);
    lv_obj_align(set_button, LV_ALIGN_TOP_MID, 0, 346);
    setButtonStyle(set_button, lv_color_hex(0x34435D));
    lv_obj_add_event_cb(set_button, setSystemTimeButtonCallback, LV_EVENT_CLICKED, this);
    lv_obj_t *set_label = lv_label_create(set_button);
    ESP_UTILS_CHECK_NULL_RETURN(set_label, false, "Create system-time button label failed");
    makePassive(set_label);
    lv_label_set_text(set_label, "Set from system time");
    lv_obj_set_style_text_color(set_label, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_center(set_label);

    _status_label = lv_label_create(_screen);
    ESP_UTILS_CHECK_NULL_RETURN(_status_label, false, "Create status label failed");
    makePassive(_status_label);
    lv_obj_set_width(_status_label, LV_PCT(100));
    lv_obj_set_style_text_align(_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(_status_label, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(_status_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(_status_label, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_label_set_text(_status_label, "Reading RTC...");
    return true;
}

void Rtc::releaseUi()
{
    if (_refresh_timer != nullptr) {
        lv_timer_delete(_refresh_timer);
        _refresh_timer = nullptr;
    }
    _screen = nullptr;
    _time_label = nullptr;
    _date_label = nullptr;
    _status_label = nullptr;
}

void Rtc::refreshRtc()
{
    if (!_rtc_initialized || _time_label == nullptr || _date_label == nullptr) {
        return;
    }

    pcf85063a_datetime_t datetime = {};
    const esp_err_t result = pcf85063a_get_time_date(&_rtc, &datetime);
    if (result != ESP_OK) {
        ESP_UTILS_LOGW("Read PCF85063A failed: %s", esp_err_to_name(result));
        setStatus("RTC read failed", true);
        return;
    }

    lv_label_set_text_fmt(_time_label, "%02u:%02u:%02u", datetime.hour, datetime.min, datetime.sec);
    lv_label_set_text_fmt(_date_label, "%04u-%02u-%02u", datetime.year, datetime.month, datetime.day);
    setStatus("RTC synchronized", false);
}

void Rtc::setRtcFromSystemTime()
{
    if (!_rtc_initialized) {
        setStatus("RTC is not initialized", true);
        return;
    }

    const time_t now = time(nullptr);
    struct tm local_time = {};
    if (now == static_cast<time_t>(-1) || localtime_r(&now, &local_time) == nullptr) {
        setStatus("System time is unavailable", true);
        return;
    }

    const int system_year = local_time.tm_year + 1900;
    // A freshly booted ESP32 reports the Unix epoch until SNTP (or another
    // trusted source) sets the clock.  Do not overwrite the battery-backed
    // RTC with that placeholder.  The Registry driver stores 1970..2069 in
    // the PCF85063A's two-digit year register.
    if (system_year < 2020 || system_year > 2069) {
        setStatus("Sync network time before setting RTC", true);
        return;
    }

    const pcf85063a_datetime_t datetime = {
        .year = static_cast<uint16_t>(system_year),
        .month = static_cast<uint8_t>(local_time.tm_mon + 1),
        .day = static_cast<uint8_t>(local_time.tm_mday),
        .dotw = static_cast<uint8_t>(local_time.tm_wday),
        .hour = static_cast<uint8_t>(local_time.tm_hour),
        .min = static_cast<uint8_t>(local_time.tm_min),
        .sec = static_cast<uint8_t>(local_time.tm_sec),
    };
    const esp_err_t result = pcf85063a_set_time_date(&_rtc, datetime);
    if (result != ESP_OK) {
        ESP_UTILS_LOGW("Set PCF85063A from system time failed: %s", esp_err_to_name(result));
        setStatus("Set from system time failed", true);
        return;
    }

    setStatus("RTC set from system time", false);
    refreshRtc();
}

void Rtc::setStatus(const char *message, bool is_error)
{
    if (_status_label == nullptr) {
        return;
    }
    lv_label_set_text(_status_label, message);
    lv_obj_set_style_text_color(
        _status_label,
        lv_color_hex(is_error ? COLOR_ERROR : COLOR_SUCCESS),
        LV_PART_MAIN
    );
}

void Rtc::refreshTimerCallback(lv_timer_t *timer)
{
    auto *app = static_cast<Rtc *>(timer->user_data);
    if (app != nullptr) {
        app->refreshRtc();
    }
}

void Rtc::refreshButtonCallback(lv_event_t *event)
{
    auto *app = static_cast<Rtc *>(lv_event_get_user_data(event));
    if (app != nullptr) {
        app->refreshRtc();
    }
}

void Rtc::setSystemTimeButtonCallback(lv_event_t *event)
{
    auto *app = static_cast<Rtc *>(lv_event_get_user_data(event));
    if (app != nullptr) {
        app->setRtcFromSystemTime();
    }
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, Rtc, APP_NAME, []() {
    return std::shared_ptr<Rtc>(
        Rtc::requestInstance(), [](Rtc *) {}
    );
})

} // namespace esp_brookesia::apps
