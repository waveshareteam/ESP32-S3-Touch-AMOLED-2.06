/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "XiaozhiClient.hpp"
#include "XiaozhiDeveloperTools.hpp"

#include <atomic>
#include <new>
#include <utility>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mcp_engine.h"
#include "esp_xiaozhi_chat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esp_brookesia::apps {

namespace {

constexpr char TAG[] = "XiaozhiClient";

} // namespace

class XiaozhiClient::Impl {
public:
    Impl(Config config, Callbacks callbacks):
        _config(std::move(config)),
        _callbacks(std::move(callbacks))
    {
    }

    ~Impl()
    {
        esp_err_t result = ESP_OK;
        while ((result = stop()) != ESP_OK) {
            ESP_LOGW(TAG, "Waiting for Xiaozhi transport shutdown: %s",
                     esp_err_to_name(result));
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    esp_err_t start()
    {
        if (_chat != 0) {
            return ESP_OK;
        }
        if ((!_config.has_mqtt_config && !_config.has_websocket_config) ||
                _config.sample_rate < 8000 ||
                _config.sample_rate > 48000 ||
                _config.channels < 1 ||
                _config.channels > 2 ||
                _config.frame_duration_ms < 10 ||
                _config.frame_duration_ms > 120 ||
                _config.playback_sample_rate <= 0) {
            return ESP_ERR_INVALID_ARG;
        }

        esp_err_t ret = esp_mcp_create(&_mcp);
        if (ret != ESP_OK) {
            return ret;
        }

        ret = registerXiaozhiDeveloperTools(_mcp);
        if (ret != ESP_OK) {
            esp_mcp_destroy(_mcp);
            _mcp = nullptr;
            return ret;
        }

        esp_xiaozhi_chat_config_t chat_config = {};
        chat_config.audio_type = ESP_XIAOZHI_CHAT_AUDIO_TYPE_OPUS;
        chat_config.audio_callback = &Impl::audioCallback;
        chat_config.event_callback = &Impl::chatEventCallback;
        chat_config.audio_callback_ctx = this;
        chat_config.event_callback_ctx = this;
        chat_config.mcp_engine = _mcp;
        chat_config.owns_mcp_engine = false;
        chat_config.has_mqtt_config = _config.has_mqtt_config;
        chat_config.has_websocket_config = _config.has_websocket_config;

        ret = esp_xiaozhi_chat_init(&chat_config, &_chat);
        if (ret != ESP_OK) {
            esp_mcp_destroy(_mcp);
            _mcp = nullptr;
            _chat = 0;
            return ret;
        }

        ret = esp_event_handler_instance_register(
                  ESP_XIAOZHI_CHAT_EVENTS,
                  ESP_EVENT_ANY_ID,
                  &Impl::espEventHandler,
                  this,
                  &_event_instance
        );
        if (ret != ESP_OK) {
            (void)stop();
            return ret;
        }

        _callbacks_enabled.store(true);
        ret = esp_xiaozhi_chat_start(_chat);
        if (ret != ESP_OK) {
            (void)stop();
            return ret;
        }

        ESP_LOGI(
            TAG,
            "Started managed Xiaozhi transport (%s)",
            _config.has_mqtt_config ? "MQTT+UDP" : "WebSocket"
        );
        return ESP_OK;
    }

    esp_err_t stop()
    {
        _callbacks_enabled.store(false);
        _connected.store(false);
        _channel_open.store(false);

        if (_event_instance != nullptr) {
            (void)esp_event_handler_instance_unregister(
                ESP_XIAOZHI_CHAT_EVENTS,
                ESP_EVENT_ANY_ID,
                _event_instance
            );
            _event_instance = nullptr;
        }

        esp_err_t ret = ESP_OK;
        if (_chat != 0) {
            ret = esp_xiaozhi_chat_deinit(_chat);
            if (ret != ESP_OK) {
                // The receive task can still reference both the chat handle
                // and MCP engine. Preserve the complete dependency graph so a
                // later stop (or the destructor) can retry safely.
                return ret;
            }
            _chat = 0;
        }

        if (_mcp) {
            esp_mcp_destroy(_mcp);
            _mcp = nullptr;
        }
        return ret;
    }

    esp_err_t openAudioChannel()
    {
        if (_chat == 0 || !_connected.load()) {
            return ESP_ERR_INVALID_STATE;
        }
        if (_channel_open.load()) {
            return ESP_OK;
        }

        esp_xiaozhi_chat_audio_t audio = {};
        audio.format = "opus";
        audio.sample_rate = _config.sample_rate;
        audio.channels = _config.channels;
        audio.frame_duration = _config.frame_duration_ms;

        esp_err_t ret =
            esp_xiaozhi_chat_open_audio_channel(_chat, &audio, nullptr, 0);
        if (ret == ESP_OK) {
            _channel_open.store(true);
        }
        return ret;
    }

    esp_err_t closeAudioChannel()
    {
        if (_chat == 0) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!_channel_open.load()) {
            return ESP_OK;
        }
        if (!_connected.load()) {
            _channel_open.store(false);
            return ESP_OK;
        }
        esp_err_t ret = esp_xiaozhi_chat_close_audio_channel(_chat);
        if (ret == ESP_OK) {
            _channel_open.store(false);
        }
        return ret;
    }

    bool isConnected() const
    {
        return _connected.load();
    }

    bool isAudioChannelOpen() const
    {
        return _channel_open.load();
    }

    esp_err_t sendAudio(const uint8_t *data, size_t len, uint32_t timestamp)
    {
        (void)timestamp;
        if (_chat == 0 || !_connected.load() || !_channel_open.load()) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!data || len == 0) {
            return ESP_ERR_INVALID_ARG;
        }
        return esp_xiaozhi_chat_send_audio_data(
            _chat, reinterpret_cast<const char *>(data), len
        );
    }

    esp_err_t sendWakeWordDetected(const std::string &wake_word)
    {
        if (_chat == 0 || !_connected.load() || !_channel_open.load()) {
            return ESP_ERR_INVALID_STATE;
        }
        return esp_xiaozhi_chat_send_wake_word(_chat, wake_word.c_str());
    }

    esp_err_t sendStartListening(ListeningMode mode)
    {
        if (_chat == 0 || !_connected.load() || !_channel_open.load()) {
            return ESP_ERR_INVALID_STATE;
        }

        int component_mode = ESP_XIAOZHI_CHAT_LISTENING_MODE_AUTO;
        switch (mode) {
        case ListeningMode::AutoStop:
            // esp_xiaozhi 0.1.1 maps AUTO to the protocol string "auto".
            component_mode = ESP_XIAOZHI_CHAT_LISTENING_MODE_AUTO;
            break;
        case ListeningMode::ManualStop:
            component_mode = ESP_XIAOZHI_CHAT_LISTENING_MODE_MANUAL;
            break;
        case ListeningMode::Realtime:
            component_mode = ESP_XIAOZHI_CHAT_LISTENING_MODE_REALTIME;
            break;
        }
        return esp_xiaozhi_chat_send_start_listening(_chat, component_mode);
    }

    esp_err_t sendStopListening()
    {
        if (_chat == 0 || !_connected.load() || !_channel_open.load()) {
            return ESP_ERR_INVALID_STATE;
        }
        return esp_xiaozhi_chat_send_stop_listening(_chat);
    }

    esp_err_t sendAbortSpeaking(AbortReason reason)
    {
        if (_chat == 0 || !_connected.load() || !_channel_open.load()) {
            return ESP_ERR_INVALID_STATE;
        }
        const esp_xiaozhi_chat_abort_speaking_reason_t component_reason =
            reason == AbortReason::WakeWordDetected ?
            ESP_XIAOZHI_CHAT_ABORT_SPEAKING_REASON_WAKE_WORD_DETECTED :
            ESP_XIAOZHI_CHAT_ABORT_SPEAKING_REASON_STOP_LISTENING;
        return esp_xiaozhi_chat_send_abort_speaking(_chat, component_reason);
    }

private:
    Config _config;
    Callbacks _callbacks;
    esp_xiaozhi_chat_handle_t _chat = 0;
    esp_mcp_t *_mcp = nullptr;
    esp_event_handler_instance_t _event_instance = nullptr;
    std::atomic<bool> _callbacks_enabled{false};
    std::atomic<bool> _connected{false};
    std::atomic<bool> _channel_open{false};

    static void audioCallback(const uint8_t *data, int len, void *context)
    {
        auto *self = static_cast<Impl *>(context);
        if (!self || !self->_callbacks_enabled.load() || !data || len <= 0 ||
                !self->_callbacks.onAudio) {
            return;
        }

        // esp_xiaozhi 0.1.1 does not expose server hello audio metadata.
        // Opus supports decoding directly at the requested 24 kHz output rate,
        // so the app always decodes into the board's physical playback rate.
        self->_callbacks.onAudio(
            data,
            static_cast<size_t>(len),
            self->_config.playback_sample_rate,
            self->_config.frame_duration_ms,
            0
        );
    }

    static void chatEventCallback(
        esp_xiaozhi_chat_event_t event,
        void *event_data,
        void *context
    )
    {
        auto *self = static_cast<Impl *>(context);
        if (!self || !self->_callbacks_enabled.load()) {
            return;
        }

        switch (event) {
        case ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT: {
            auto *text =
                static_cast<esp_xiaozhi_chat_text_data_t *>(event_data);
            if (text && text->text && self->_callbacks.onText) {
                self->_callbacks.onText(
                    text->role == ESP_XIAOZHI_CHAT_TEXT_ROLE_ASSISTANT ?
                    TextRole::Assistant : TextRole::User,
                    text->text
                );
            }
            break;
        }
        case ESP_XIAOZHI_CHAT_EVENT_CHAT_EMOJI:
            if (event_data && self->_callbacks.onEmotion) {
                self->_callbacks.onEmotion(static_cast<const char *>(event_data));
            }
            break;
        case ESP_XIAOZHI_CHAT_EVENT_CHAT_TTS_STATE: {
            auto *tts = static_cast<esp_xiaozhi_chat_tts_state_t *>(event_data);
            if (!tts || !self->_callbacks.onTts) {
                break;
            }
            TtsState state = TtsState::SentenceStart;
            if (tts->state == ESP_XIAOZHI_CHAT_TTS_STATE_START) {
                state = TtsState::Start;
            } else if (tts->state == ESP_XIAOZHI_CHAT_TTS_STATE_STOP) {
                state = TtsState::Stop;
            }
            self->_callbacks.onTts(state, tts->text ? tts->text : "");
            break;
        }
        case ESP_XIAOZHI_CHAT_EVENT_CHAT_ERROR: {
            auto *error =
                static_cast<esp_xiaozhi_chat_error_info_t *>(event_data);
            if (self->_callbacks.onError) {
                self->_callbacks.onError(
                    error ? error->code : ESP_FAIL,
                    error && error->source ? error->source : "esp_xiaozhi"
                );
            }
            break;
        }
        case ESP_XIAOZHI_CHAT_EVENT_CHAT_SPEECH_STARTED:
        case ESP_XIAOZHI_CHAT_EVENT_CHAT_SPEECH_STOPPED:
        case ESP_XIAOZHI_CHAT_EVENT_CHAT_SYSTEM_CMD:
            // CHAT_TTS_STATE is the authoritative state notification.
            break;
        }
    }

    static void espEventHandler(
        void *context,
        esp_event_base_t event_base,
        int32_t event_id,
        void *event_data
    )
    {
        (void)event_base;
        (void)event_data;
        auto *self = static_cast<Impl *>(context);
        if (!self || !self->_callbacks_enabled.load()) {
            return;
        }

        switch (event_id) {
        case ESP_XIAOZHI_CHAT_EVENT_CONNECTED:
            self->_connected.store(true);
            if (self->_callbacks.onConnected) {
                self->_callbacks.onConnected();
            }
            break;
        case ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED:
            self->_connected.store(false);
            self->_channel_open.store(false);
            if (self->_callbacks.onDisconnected) {
                self->_callbacks.onDisconnected();
            }
            break;
        case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_OPENED:
            self->_channel_open.store(true);
            if (self->_callbacks.onAudioChannelOpened) {
                self->_callbacks.onAudioChannelOpened();
            }
            break;
        case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_CLOSED:
            self->_channel_open.store(false);
            if (self->_callbacks.onAudioChannelClosed) {
                self->_callbacks.onAudioChannelClosed();
            }
            break;
        case ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE:
            // Clear the component session while the goodbye transport event
            // is still ordered ahead of a possible disconnect event.
            if (self->_channel_open.exchange(false) && self->_chat != 0) {
                (void)esp_xiaozhi_chat_close_audio_channel(self->_chat);
            }
            if (self->_callbacks.onServerGoodbye) {
                self->_callbacks.onServerGoodbye();
            }
            break;
        default:
            break;
        }
    }
};

XiaozhiClient::Config::Config(
    bool mqtt_config,
    bool websocket_config,
    int uplink_sample_rate,
    int uplink_channels,
    int frame_duration,
    int output_sample_rate
):
    has_mqtt_config(mqtt_config),
    has_websocket_config(websocket_config),
    sample_rate(uplink_sample_rate),
    channels(uplink_channels),
    frame_duration_ms(frame_duration),
    playback_sample_rate(output_sample_rate)
{
}

XiaozhiClient::XiaozhiClient(Config config, Callbacks callbacks):
    _impl(new (std::nothrow) Impl(std::move(config), std::move(callbacks)))
{
}

XiaozhiClient::~XiaozhiClient()
{
    delete _impl;
}

esp_err_t XiaozhiClient::start()
{
    return _impl ? _impl->start() : ESP_ERR_NO_MEM;
}

esp_err_t XiaozhiClient::stop()
{
    return _impl ? _impl->stop() : ESP_ERR_NO_MEM;
}

esp_err_t XiaozhiClient::openAudioChannel()
{
    return _impl ? _impl->openAudioChannel() : ESP_ERR_NO_MEM;
}

esp_err_t XiaozhiClient::closeAudioChannel()
{
    return _impl ? _impl->closeAudioChannel() : ESP_ERR_NO_MEM;
}

bool XiaozhiClient::isConnected() const
{
    return _impl && _impl->isConnected();
}

bool XiaozhiClient::isAudioChannelOpen() const
{
    return _impl && _impl->isAudioChannelOpen();
}

esp_err_t XiaozhiClient::sendAudio(
    const uint8_t *data,
    size_t len,
    uint32_t timestamp
)
{
    return _impl ? _impl->sendAudio(data, len, timestamp) : ESP_ERR_NO_MEM;
}

esp_err_t XiaozhiClient::sendWakeWordDetected(const std::string &wake_word)
{
    return _impl ?
           _impl->sendWakeWordDetected(wake_word) : ESP_ERR_NO_MEM;
}

esp_err_t XiaozhiClient::sendStartListening(ListeningMode mode)
{
    return _impl ? _impl->sendStartListening(mode) : ESP_ERR_NO_MEM;
}

esp_err_t XiaozhiClient::sendStopListening()
{
    return _impl ? _impl->sendStopListening() : ESP_ERR_NO_MEM;
}

esp_err_t XiaozhiClient::sendAbortSpeaking(AbortReason reason)
{
    return _impl ? _impl->sendAbortSpeaking(reason) : ESP_ERR_NO_MEM;
}

} // namespace esp_brookesia::apps
