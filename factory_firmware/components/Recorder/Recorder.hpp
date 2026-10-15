/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <atomic>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "storage_service_ui.hpp"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class Recorder : public systems::phone::App {
public:
    static Recorder *requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);
    ~Recorder();

protected:
    Recorder(bool use_status_bar, bool use_navigation_bar);

    bool run() override;
    bool back() override;
    bool close() override;
    bool init() override;
    bool deinit() override;
    bool pause() override;
    bool resume() override;

private:
    enum class State : uint8_t {
        Idle,
        Starting,
        Recording,
        Stopping,
        Saved,
        Error,
    };

    static Recorder *_instance;

    lv_obj_t *_status_label = nullptr;
    lv_obj_t *_time_label = nullptr;
    lv_obj_t *_path_label = nullptr;
    lv_obj_t *_level_bar = nullptr;
    lv_obj_t *_record_button = nullptr;
    lv_obj_t *_record_button_label = nullptr;
    lv_timer_t *_ui_timer = nullptr;
    brookesia::storage_ui::SdCardGate _sd_gate;

    std::atomic<State> _state{State::Idle};
    std::atomic<TaskHandle_t> _record_task{nullptr};
    std::atomic<bool> _stop_requested{false};
    std::atomic<bool> _worker_stopped{true};
    std::atomic<uint64_t> _pcm_bytes{0};
    std::atomic<int64_t> _started_us{0};
    std::atomic<uint32_t> _peak{0};
    std::atomic<esp_err_t> _last_error{ESP_OK};
    std::atomic<bool> _audio_session_acquired{false};
    std::atomic<bool> _codec_claimed{false};
    char _last_path[192] = {};

    bool startRecording();
    bool stopRecording(TickType_t timeout);
    bool releaseAudioSession();
    void destroyUi();
    void updateUi();

    static void recordTask(void *arg);
    static void buttonEvent(lv_event_t *event);
    static void uiTimer(lv_timer_t *timer);
};

} // namespace esp_brookesia::apps
