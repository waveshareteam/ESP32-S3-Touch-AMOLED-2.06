/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <functional>
#include <stddef.h>
#include <stdint.h>
#include <string>

#include "esp_err.h"

namespace esp_brookesia::apps {

/**
 * Application-facing adapter for the espressif/esp_xiaozhi component.
 *
 * Board audio, wake-word processing, Opus coding, UI, and application state
 * stay outside this class. This adapter only translates the managed
 * component's C callbacks and lifecycle into the app's C++ control events.
 */
class XiaozhiClient final {
public:
    enum class ListeningMode : uint8_t {
        AutoStop,
        ManualStop,
        Realtime,
    };

    enum class AbortReason : uint8_t {
        None,
        WakeWordDetected,
    };

    enum class TextRole : uint8_t {
        User,
        Assistant,
    };

    enum class TtsState : uint8_t {
        Start,
        Stop,
        SentenceStart,
    };

    struct Config {
        bool has_mqtt_config = false;
        bool has_websocket_config = false;
        int sample_rate = 16000;
        int channels = 1;
        int frame_duration_ms = 60;
        int playback_sample_rate = 24000;

        Config() = default;
        Config(
            bool mqtt_config,
            bool websocket_config,
            int uplink_sample_rate = 16000,
            int uplink_channels = 1,
            int frame_duration = 60,
            int output_sample_rate = 24000
        );
    };

    struct Callbacks {
        // Callback payloads are copied by the app before returning.
        std::function<void()> onConnected;
        std::function<void()> onDisconnected;
        std::function<void()> onAudioChannelOpened;
        std::function<void()> onAudioChannelClosed;
        std::function<void()> onServerGoodbye;
        std::function<void(TextRole role, const std::string &text)> onText;
        std::function<void(const std::string &emotion)> onEmotion;
        std::function<void(TtsState state, const std::string &text)> onTts;

        // The audio buffer is valid only for the duration of this callback.
        std::function<void(
            const uint8_t *data,
            size_t len,
            int sample_rate,
            int frame_duration_ms,
            uint32_t timestamp
        )> onAudio;
        std::function<void(esp_err_t error, const std::string &source)> onError;
    };

    explicit XiaozhiClient(Config config, Callbacks callbacks = {});
    ~XiaozhiClient();

    XiaozhiClient(const XiaozhiClient &) = delete;
    XiaozhiClient &operator=(const XiaozhiClient &) = delete;
    XiaozhiClient(XiaozhiClient &&) = delete;
    XiaozhiClient &operator=(XiaozhiClient &&) = delete;

    esp_err_t start();
    esp_err_t stop();
    esp_err_t openAudioChannel();
    esp_err_t closeAudioChannel();
    bool isConnected() const;
    bool isAudioChannelOpen() const;

    esp_err_t sendAudio(
        const uint8_t *data,
        size_t len,
        uint32_t timestamp = 0
    );
    esp_err_t sendWakeWordDetected(const std::string &wake_word);
    esp_err_t sendStartListening(ListeningMode mode);
    esp_err_t sendStopListening();
    esp_err_t sendAbortSpeaking(AbortReason reason);

private:
    class Impl;
    Impl *_impl = nullptr;
};

} // namespace esp_brookesia::apps
