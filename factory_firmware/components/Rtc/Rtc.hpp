/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "lvgl.h"
#include "pcf85063a.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class Rtc final : public systems::phone::App {
public:
    static Rtc *requestInstance();
    ~Rtc() override = default;

    using systems::phone::App::endRecordResource;
    using systems::phone::App::startRecordResource;

protected:
    Rtc();

    bool init() override;
    bool deinit() override;
    bool run() override;
    bool back() override;
    bool close() override;
    bool pause() override;
    bool resume() override;

private:
    static Rtc *_instance;

    static void refreshTimerCallback(lv_timer_t *timer);
    static void refreshButtonCallback(lv_event_t *event);
    static void setSystemTimeButtonCallback(lv_event_t *event);

    bool createUi();
    void releaseUi();
    void refreshRtc();
    void setRtcFromSystemTime();
    void setStatus(const char *message, bool is_error);

    pcf85063a_dev_t _rtc = {};
    bool _rtc_initialized = false;
    lv_obj_t *_screen = nullptr;
    lv_obj_t *_time_label = nullptr;
    lv_obj_t *_date_label = nullptr;
    lv_obj_t *_status_label = nullptr;
    lv_timer_t *_refresh_timer = nullptr;
};

} // namespace esp_brookesia::apps
