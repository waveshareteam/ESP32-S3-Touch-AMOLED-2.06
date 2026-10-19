/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "esp_ae_rate_cvt.h"
#include "esp_event.h"
#include "XiaozhiClient.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class XiaozhiActivationClient;
class XiaozhiActivationPrompt;
class XiaozhiAudioProcessor;
class XiaozhiUi;

class XiaozhiApp: public systems::phone::App {
public:
    static XiaozhiApp *requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);
    ~XiaozhiApp();
    // Call before installation so SPIFFS font reads do not block LVGL.
    bool prepareUiResources();

protected:
    XiaozhiApp(bool use_status_bar, bool use_navigation_bar);

    bool run() override;
    bool back() override;
    bool close() override;
    bool init() override;
    bool deinit() override;
    bool pause() override;
    bool resume() override;

private:
    enum class State : uint8_t {
        NetworkRequired,
        Preparing,
        ActivationRequired,
        Connecting,
        Ready,
        Listening,
        Processing,
        Speaking,
        Error,
    };

    enum class ChannelState : uint8_t {
        Closed,
        Opening,
        Open,
        Closing,
    };

    enum class ListenMode : uint8_t {
        None,
        Manual,
        AutoStop,
    };

    enum class ActivationPhase : uint8_t {
        CheckRequired,
        WaitingForBinding,
        BoundPendingSuccess,
        Complete,
    };

    enum class ControlEventType : uint8_t {
        AppVisible,
        AppHidden,
        NetworkChanged,
        ActionPressed,
        AnnounceActivation,
        WakeWord,
        TransportConnected,
        TransportDisconnected,
        ChannelOpened,
        ChannelClosed,
        ServerGoodbye,
        ChatText,
        Emotion,
        TtsStart,
        TtsStop,
        PlaybackDrained,
        ChatError,
        AudioError,
        Shutdown,
    };

    struct ControlEvent {
        ControlEventType type = ControlEventType::NetworkChanged;
        int32_t value = 0;
        esp_err_t error = ESP_OK;
        uint8_t role = 0;
        bool flag = false;
        uint32_t chat_generation = 0;
        uint32_t turn_generation = 0;
        char text[512] = {};
        char detail[128] = {};
    };

    struct PcmFrame {
        int16_t samples[960];
    };

    struct AudioPacket {
        uint32_t turn_generation;
        uint32_t timestamp;
        uint16_t sample_rate;
        uint16_t frame_duration_ms;
        uint16_t size;
        uint8_t data[1500];
    };

    static constexpr int CONTROL_TASK_STACK_SIZE = 12 * 1024;
    static constexpr int INPUT_TASK_STACK_SIZE = 6 * 1024;
    static constexpr int ENCODER_TASK_STACK_SIZE = 40 * 1024;
    static constexpr int PLAYBACK_TASK_STACK_SIZE = 24 * 1024;
    static constexpr size_t AFE_INPUT_CHANNELS = 3;

    static XiaozhiApp *_instance;

    lv_obj_t *_page_root = nullptr;
    lv_timer_t *_ui_timer = nullptr;
    XiaozhiUi *_ui = nullptr;
    bool _ui_resources_prepared = false;
    XiaozhiActivationClient *_activation_client = nullptr;
    XiaozhiActivationPrompt *_activation_prompt = nullptr;
    XiaozhiAudioProcessor *_audio_processor = nullptr;

    SemaphoreHandle_t _state_mutex = nullptr;
    SemaphoreHandle_t _lifecycle_mutex = nullptr;
    SemaphoreHandle_t _audio_tx_mutex = nullptr;
    SemaphoreHandle_t _playback_mutex = nullptr;
    SemaphoreHandle_t _control_ack = nullptr;
    SemaphoreHandle_t _control_exited = nullptr;
    SemaphoreHandle_t _preroll_done = nullptr;
    QueueHandle_t _control_queue = nullptr;
    QueueHandle_t _pcm_queue = nullptr;
    QueueHandle_t _playback_queue = nullptr;

    TaskHandle_t _control_task = nullptr;
    TaskHandle_t _input_task = nullptr;
    TaskHandle_t _encoder_task = nullptr;
    TaskHandle_t _playback_task = nullptr;

    esp_event_handler_instance_t _ip_event_instance = nullptr;

    std::atomic<State> _state{State::NetworkRequired};
    std::atomic<ChannelState> _channel_state{ChannelState::Closed};
    std::atomic<bool> _network_ready{false};
    std::atomic<bool> _visible{false};
    std::atomic<bool> _shutdown{false};
    std::atomic<bool> _destroying{false};
    std::atomic<bool> _deinit_in_progress{false};
    std::atomic<bool> _deinit_complete{false};
    std::atomic<uint32_t> _lifecycle_refs{0};
    std::atomic<uint32_t> _async_post_users{0};
    std::atomic<bool> _control_running{false};
    std::atomic<bool> _audio_tasks_running{false};
    std::atomic<bool> _audio_session_acquired{false};
    std::atomic<bool> _codec_claimed{false};
    std::atomic<bool> _input_task_running{false};
    std::atomic<bool> _encoder_task_running{false};
    std::atomic<bool> _playback_task_running{false};
    std::atomic<bool> _capture_enabled{false};
    std::atomic<bool> _voice_enabled{false};
    std::atomic<bool> _uplink_ready{false};
    std::atomic<bool> _preroll_active{false};
    std::atomic<uint32_t> _preroll_frames_remaining{0};
    std::atomic<int> _preroll_result{ESP_OK};
    std::atomic<TickType_t> _uplink_started_tick{0};
    std::atomic<uint32_t> _pcm_frames_captured{0};
    std::atomic<uint32_t> _pcm_frames_dropped{0};
    std::atomic<uint32_t> _opus_packets_sent{0};
    std::atomic<uint32_t> _pcm_peak{0};
    std::atomic<uint32_t> _pcm_mean_abs{0};
    std::atomic<uint32_t> _input_pcm_peak{0};
    std::atomic<uint32_t> _input_pcm_mean_abs{0};
    std::atomic<bool> _accept_playback{false};
    std::atomic<bool> _tts_stop_pending{false};
    std::atomic<bool> _playback_busy{false};
    std::atomic<bool> _drain_event_posted{false};
    std::atomic<bool> _chat_initialized{false};
    std::atomic<bool> _chat_started{false};
    std::atomic<bool> _ready_prompt_played{false};
    std::atomic<XiaozhiClient *> _protocol{nullptr};
    std::atomic<bool> _input_resampler_reset_requested{false};
    std::atomic<uint32_t> _chat_generation{0};
    std::atomic<uint32_t> _turn_generation{0};
    std::atomic<bool> _control_event_lost{false};
    std::atomic<TickType_t> _last_audio_packet_tick{0};
    std::atomic<TickType_t> _last_audio_write_tick{0};
    std::atomic<TickType_t> _tts_stop_tick{0};

    bool _transport_connected = false;
    std::atomic<uint32_t> _server_goodbye_generation{0};
    std::atomic<bool> _restart_transport_after_playback{false};
    bool _awaiting_response = false;
    bool _tts_active = false;
    ListenMode _listen_mode = ListenMode::None;
    ActivationPhase _activation_phase = ActivationPhase::CheckRequired;
    TickType_t _next_prepare_tick = 0;

    void *_opus_encoder = nullptr;
    void *_opus_decoder = nullptr;
    esp_ae_rate_cvt_handle_t _input_resamplers[AFE_INPUT_CHANNELS] = {};
    esp_ae_rate_cvt_handle_t _output_resampler = nullptr;
    int _opus_input_size = 0;
    int _opus_output_size = 0;
    uint32_t _input_resampler_max_samples = 0;
    int _decoder_sample_rate = 0;
    int _decoder_frame_duration_ms = 0;

    char _activation_code[64] = {};
    char _activation_message[192] = {};
    char _chat_role[16] = {};
    char _chat_text[512] = {};
    char _emotion[32] = {"neutral"};

    void createUi();
    void destroyUi();
    void refreshUi();
    void setState(State state);
    void setError(esp_err_t error, const char *source);
    void setChatMessage(XiaozhiClient::TextRole role, const char *text);
    void setEmotion(const char *emotion);
    void clearConversationUi();
    void copyActivation(const char *code, const char *message);
    void clearActivation();
    bool announceActivation(bool force);
    void stopActivationPrompt();
    bool playReadyPromptOnce();
    bool canPrepare() const;

    bool isNetworkReady() const;
    void updateNetworkState();

    bool postControlEvent(const ControlEvent &event, bool wait_for_ack = false);
    void controlLoop();
    void handleControlEvent(const ControlEvent &event);
    void handleAppVisible();
    void handleAppHidden(bool release_audio);
    void prepareIfDue();
    void schedulePrepare(uint32_t delay_ms);
    bool prepareChat();
    void destroyChat();

    bool ensureAudioReady();
    bool shutdownAudio();
    bool releaseAudioSession();
    bool setAudioModes(bool wake_word, bool voice);
    bool enterReadyAudioMode();
    esp_err_t sendWakeWordPcm();
    void finishPreroll(esp_err_t result);
    bool configureDecoder(uint16_t sample_rate, uint16_t frame_duration_ms);
    esp_err_t openAudioChannel();
    esp_err_t startAutoStopTurn(const char *wake_word = nullptr);
    bool startListening(ListenMode mode, const char *wake_word = nullptr);
    void finishListening();
    esp_err_t stopUplink(bool notify_server);
    void cancelManualConversation();
    void closeChannel(bool abort_speaking);
    void resetChannelState();
    void handlePlaybackDrained();
    void checkPlaybackDrained();
    bool playbackDrainReady() const;
    bool resetPlaybackBuffer(bool reset_decoder);
    bool waitForPlaybackIdle(uint32_t timeout_ms) const;

    void inputAudio();
    void encodeAudio();
    void playAudio();

    static const char *stateText(State state);
    static bool tickReached(TickType_t now, TickType_t target);
    static void controlTaskEntry(void *arg);
    static void inputTaskEntry(void *arg);
    static void encoderTaskEntry(void *arg);
    static void playbackTaskEntry(void *arg);
    static void ipEventHandler(
        void *arg,
        esp_event_base_t event_base,
        int32_t event_id,
        void *event_data
    );
    static void uiTimerCallback(lv_timer_t *timer);
    static void uiActionCallback(void *context);
};

} // namespace esp_brookesia::apps
