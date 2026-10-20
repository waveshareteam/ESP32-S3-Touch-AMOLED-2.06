/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "avi_player.h"
#include "esp_jpeg_dec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "storage_service.h"
#include "storage_service_ui.hpp"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class VideoPlayer : public systems::phone::App {
public:
    static VideoPlayer *requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);
    ~VideoPlayer();

protected:
    VideoPlayer(bool use_status_bar, bool use_navigation_bar);

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
        Mounting,
        Scanning,
        Preparing,
        Playing,
        Paused,
        Empty,
        Error,
        Stopping,
    };

    enum class Command : uint8_t {
        None,
        Play,
        Pause,
        Previous,
        Next,
    };

    static VideoPlayer *_instance;

    lv_obj_t *_canvas = nullptr;
    lv_obj_t *_screen = nullptr;
    lv_obj_t *_header = nullptr;
    lv_obj_t *_controls = nullptr;
    lv_obj_t *_hero_icon = nullptr;
    lv_obj_t *_status_label = nullptr;
    lv_obj_t *_counter_label = nullptr;
    lv_obj_t *_previous_button = nullptr;
    lv_obj_t *_play_button = nullptr;
    lv_obj_t *_next_button = nullptr;
    lv_obj_t *_play_button_label = nullptr;
    lv_timer_t *_ui_timer = nullptr;
    uint16_t _placeholder_pixel = 0;
    State _last_ui_state = State::Idle;
    bool _frame_presented = false;
    bool _chrome_visible = true;
    uint32_t _chrome_hide_at_ms = 0;

    std::atomic<State> _state{State::Idle};
    std::atomic<Command> _command{Command::None};
    std::atomic<TaskHandle_t> _worker_task{nullptr};
    std::atomic<bool> _stop_requested{false};
    std::atomic<bool> _worker_stopped{true};
    std::atomic<bool> _playing{false};
    std::atomic<bool> _ui_accept_frames{false};
    std::atomic<int> _file_count{0};
    std::atomic<int> _current_index{0};
    std::atomic<uint32_t> _decoded_frames{0};
    std::atomic<uint32_t> _dropped_frames{0};
    std::atomic<esp_err_t> _last_error{ESP_OK};
    std::atomic<bool> _cleanup_complete{true};

    storage_service_lease_t _storage_lease = {};
    brookesia::storage_ui::SdCardGate _sd_gate;
    char **_files = nullptr;
    avi_player_handle_t _avi = nullptr;
    jpeg_dec_handle_t _jpeg = nullptr;
    uint8_t *_frame_buffers[2] = {};
    size_t _frame_buffer_size = 0;
    uint16_t _video_width = 0;
    uint16_t _video_height = 0;
    std::atomic<int> _display_buffer_index{-1};
    std::atomic<int> _pending_buffer_index{-1};
    int64_t _last_queued_us = 0;
    std::atomic<bool> _audio_ready{false};
    std::atomic<bool> _audio_claimed{false};
    std::atomic<bool> _audio_session_acquired{false};
    std::atomic<bool> _audio_disable_requested{false};
    std::atomic<uint32_t> _audio_callback_users{0};

    bool startWorker();
    bool stopWorker(TickType_t timeout);
    void detachCanvas();
    void destroyUi();
    void updateUi();
    void setChromeVisible(bool visible);
    void showChrome();
    void updateControlState(State state, int file_count);
    esp_err_t scanVideos();
    void freeVideos();
    esp_err_t initJpeg();
    void deinitJpeg();
    esp_err_t ensureFrameBuffers(uint16_t width, uint16_t height, size_t bytes);
    void releaseFrameBuffers();
    bool cleanupPlaybackResources();
    esp_err_t prepareAudioForPlayback();
    esp_err_t processAudioDisableRequest(TickType_t callback_timeout, bool force);

    static void workerTask(void *arg);
    static void videoCallback(frame_data_t *data, void *arg);
    static void audioCallback(frame_data_t *data, void *arg);
    static void audioClockCallback(uint32_t rate, uint32_t bits, uint32_t channels, void *arg);
    static void playEndCallback(void *arg);
    static void controlEvent(lv_event_t *event);
    static void chromeEvent(lv_event_t *event);
    static void uiTimer(lv_timer_t *timer);
};

} // namespace esp_brookesia::apps
