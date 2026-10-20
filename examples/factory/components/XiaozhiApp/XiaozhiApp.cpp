/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "XiaozhiApp.hpp"
#include "chat_history.h"
#include "XiaozhiActivationClient.hpp"
#include "XiaozhiActivationPrompt.hpp"
#include "XiaozhiAudioProcessor.hpp"
#include "XiaozhiLocalization.hpp"
#include "XiaozhiUi.hpp"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utility>
#include <vector>

#include "bsp_board_extra.h"
#include "decoder/impl/esp_opus_dec.h"
#include "encoder/impl/esp_opus_enc.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_xiaozhi_info.h"
#include "freertos/idf_additions.h"
#include "sdkconfig.h"
#ifdef ESP_UTILS_LOG_TAG
#undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:XiaozhiApp"
#include "esp_lib_utils.h"

LV_IMG_DECLARE(img_app_xiaozhi);

namespace esp_brookesia::apps {

namespace {

constexpr uint32_t RETRY_INTERVAL_MS = 5000;
constexpr uint32_t TRANSPORT_RECONNECT_FALLBACK_MS = 10000;
constexpr uint32_t ACTIVATION_RETRY_INTERVAL_MS = 3000;
constexpr uint32_t ACTIVATION_RETRY_MAX_MS = 10000;
constexpr uint32_t CHAT_SAMPLE_RATE = 16000;
constexpr uint32_t DEVICE_AUDIO_SAMPLE_RATE = 24000;
constexpr uint32_t CHAT_FRAME_DURATION_MS = 60;
constexpr uint32_t PLAYBACK_DRAIN_MS = 200;
constexpr uint32_t PREROLL_QUEUE_TIMEOUT_MS = 5000;
constexpr uint32_t PREROLL_ENCODE_TIMEOUT_MS = 15000;
constexpr uint32_t UPLINK_FIRST_PACKET_TIMEOUT_MS = 3000;
constexpr uint32_t CONTROL_EVENT_POST_TIMEOUT_MS = 100;
constexpr uint32_t LIFECYCLE_WAIT_MS = 10000;
constexpr uint32_t PLAYBACK_IDLE_WAIT_MS = 1000;
constexpr size_t CONTROL_QUEUE_LENGTH = 24;
constexpr size_t PCM_QUEUE_LENGTH = 40;
constexpr size_t PLAYBACK_QUEUE_LENGTH = 10;
constexpr size_t DEVICE_TDM_CHANNELS = CODEC_VOICE_INPUT_CHANNELS;
constexpr size_t DEVICE_MIC1_SLOT = 0;
constexpr size_t DEVICE_ECHO_SLOT = 1;
constexpr size_t DEVICE_MIC2_SLOT = 2;
static_assert(
    XiaozhiAudioProcessor::INPUT_CHANNELS == 3,
    "Xiaozhi AFE input must be MIC1, MIC2, echo reference"
);
constexpr size_t INPUT_READ_FRAMES = DEVICE_AUDIO_SAMPLE_RATE / 100;
constexpr size_t MAX_SERVER_SAMPLE_RATE = 48000;
constexpr size_t MAX_SERVER_FRAME_DURATION_MS = 120;
constexpr size_t MAX_SERVER_PCM_SAMPLES =
    MAX_SERVER_SAMPLE_RATE * MAX_SERVER_FRAME_DURATION_MS / 1000;
constexpr size_t MAX_DEVICE_PCM_SAMPLES =
    DEVICE_AUDIO_SAMPLE_RATE * MAX_SERVER_FRAME_DURATION_MS / 1000 + 256;
constexpr size_t MAX_SERVER_PCM_BYTES =
    MAX_SERVER_PCM_SAMPLES * sizeof(int16_t);

void logServiceInfoHeap(const char *context)
{
    ESP_UTILS_LOGI(
        "Service info heap (%s): internal free=%u, largest=%u; PSRAM free=%u, largest=%u",
        context,
        static_cast<unsigned int>(
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
        ),
        static_cast<unsigned int>(
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
        ),
        static_cast<unsigned int>(
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        ),
        static_cast<unsigned int>(
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        )
    );
}

bool netifHasIpv4(const char *if_key)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(if_key);
    if (!netif) {
        return false;
    }
    esp_netif_ip_info_t info = {};
    return esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0;
}

void copyString(char *destination, size_t capacity, const char *source)
{
    if (destination && capacity > 0) {
        snprintf(destination, capacity, "%s", source ? source : "");
    }
}

uint32_t activationRetryDelay(int timeout_ms)
{
    if (timeout_ms <= 0) {
        return ACTIVATION_RETRY_INTERVAL_MS;
    }
    uint32_t delay = static_cast<uint32_t>(timeout_ms);
    if (delay < 1000) {
        delay = 1000;
    }
    return delay > ACTIVATION_RETRY_MAX_MS ? ACTIVATION_RETRY_MAX_MS : delay;
}

class ScopedSemaphoreLock {
public:
    ScopedSemaphoreLock(SemaphoreHandle_t semaphore, TickType_t timeout):
        _semaphore(semaphore)
    {
        _locked =
            _semaphore && xSemaphoreTake(_semaphore, timeout) == pdTRUE;
    }

    ~ScopedSemaphoreLock()
    {
        if (_locked) {
            xSemaphoreGive(_semaphore);
        }
    }

    explicit operator bool() const
    {
        return _locked;
    }

private:
    SemaphoreHandle_t _semaphore = nullptr;
    bool _locked = false;
};

constexpr uint32_t LIFECYCLE_CLOSED_BIT = 1U << 31;
constexpr uint32_t LIFECYCLE_REF_MASK = LIFECYCLE_CLOSED_BIT - 1;

class ScopedLifecycleAdmission {
public:
    explicit ScopedLifecycleAdmission(std::atomic<uint32_t> &state):
        _state(state)
    {
        uint32_t current = _state.load();
        while ((current & LIFECYCLE_CLOSED_BIT) == 0 &&
                (current & LIFECYCLE_REF_MASK) != LIFECYCLE_REF_MASK) {
            if (_state.compare_exchange_weak(current, current + 1)) {
                _acquired = true;
                break;
            }
        }
    }

    ~ScopedLifecycleAdmission()
    {
        if (_acquired) {
            _state.fetch_sub(1);
        }
    }

    explicit operator bool() const
    {
        return _acquired;
    }

private:
    std::atomic<uint32_t> &_state;
    bool _acquired = false;
};

class ScopedOptionalCounter {
public:
    explicit ScopedOptionalCounter(std::atomic<uint32_t> *counter):
        _counter(counter)
    {
        if (_counter) {
            _counter->fetch_add(1);
        }
    }

    ~ScopedOptionalCounter()
    {
        if (_counter) {
            _counter->fetch_sub(1);
        }
    }

private:
    std::atomic<uint32_t> *_counter = nullptr;
};

class ScopedXiaozhiInfo {
public:
    ScopedXiaozhiInfo() = default;

    ~ScopedXiaozhiInfo()
    {
        (void)esp_xiaozhi_chat_free_info(&value);
    }

    esp_xiaozhi_chat_info_t value = {};

    ScopedXiaozhiInfo(const ScopedXiaozhiInfo &) = delete;
    ScopedXiaozhiInfo &operator=(const ScopedXiaozhiInfo &) = delete;
};

} // namespace

XiaozhiApp *XiaozhiApp::_instance = nullptr;

XiaozhiApp *XiaozhiApp::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (!_instance) {
        _instance = new XiaozhiApp(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

XiaozhiApp::XiaozhiApp(bool use_status_bar, bool use_navigation_bar):
    App("AIChats", &img_app_xiaozhi, true, use_status_bar, use_navigation_bar)
{
    _lifecycle_mutex = xSemaphoreCreateMutex();
}

XiaozhiApp::~XiaozhiApp()
{
    _destroying.store(true);
    while (!deinit()) {
        ESP_UTILS_LOGW("Waiting to deinitialize Xiaozhi safely");
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    _lifecycle_refs.fetch_or(LIFECYCLE_CLOSED_BIT);
    while ((_lifecycle_refs.load() & LIFECYCLE_REF_MASK) != 0) {
        vTaskDelay(1);
    }
    if (_lifecycle_mutex) {
        vSemaphoreDelete(_lifecycle_mutex);
        _lifecycle_mutex = nullptr;
    }
    _instance = nullptr;
}

bool XiaozhiApp::run()
{
    _ready_prompt_played.store(false);
    createUi();
    _visible.store(true);
    updateNetworkState();
    ControlEvent event = {};
    event.type = ControlEventType::AppVisible;
    postControlEvent(event);
    return true;
}

bool XiaozhiApp::back()
{
    _visible.store(false);
    if (_activation_prompt) {
        _activation_prompt->cancel();
    }
    if (_activation_client) {
        _activation_client->cancel();
    }
    ControlEvent event = {};
    event.type = ControlEventType::AppHidden;
    event.flag = true;
    if (!postControlEvent(event, true)) {
        return false;
    }
    return notifyCoreClosed();
}

bool XiaozhiApp::close()
{
    _visible.store(false);
    if (_activation_prompt) {
        _activation_prompt->cancel();
    }
    if (_activation_client) {
        _activation_client->cancel();
    }
    ControlEvent event = {};
    event.type = ControlEventType::AppHidden;
    event.flag = true;
    if (!postControlEvent(event, true)) {
        return false;
    }
    destroyUi();
    return true;
}

bool XiaozhiApp::prepareUiResources()
{
    ScopedLifecycleAdmission lifecycle_user(_lifecycle_refs);
    if (!lifecycle_user) {
        return false;
    }
    ScopedSemaphoreLock lifecycle_lock(
        _lifecycle_mutex, pdMS_TO_TICKS(LIFECYCLE_WAIT_MS)
    );
    if (!lifecycle_lock || _destroying.load() || _deinit_in_progress.load()) {
        return false;
    }
    if (_ui_resources_prepared) {
        return true;
    }
    _ui = _ui ? _ui : new (std::nothrow) XiaozhiUi();
    if (!_ui) {
        return false;
    }
    // preload() takes the GUI lock only for font decoding; file reads remain
    // outside it. Missing fonts retain the built-in fallback for this boot.
    (void)_ui->preload();
    _ui_resources_prepared = true;
    return true;
}

bool XiaozhiApp::init()
{
    ScopedLifecycleAdmission lifecycle_user(_lifecycle_refs);
    if (!lifecycle_user) {
        return false;
    }
    bool deinit_was_complete = _deinit_complete.load();
    if (_destroying.load() || _deinit_in_progress.load()) {
        return false;
    }
    ScopedSemaphoreLock lifecycle_lock(
        _lifecycle_mutex, pdMS_TO_TICKS(LIFECYCLE_WAIT_MS)
    );
    if (!lifecycle_lock) {
        return false;
    }
    if (_destroying.load() || _deinit_in_progress.load() ||
            (!deinit_was_complete && _deinit_complete.load())) {
        return false;
    }
    if (_control_running.load()) {
        return true;
    }

    _deinit_complete.store(false);
    _shutdown.store(false);
    _activation_phase = ActivationPhase::CheckRequired;
    _control_event_lost.store(false);
    _playback_mutex = _playback_mutex ? _playback_mutex : xSemaphoreCreateMutex();
    _state_mutex = _state_mutex ? _state_mutex : xSemaphoreCreateMutex();
    _audio_tx_mutex = _audio_tx_mutex ? _audio_tx_mutex : xSemaphoreCreateMutex();
    if (!_playback_mutex) {
        setError(ESP_ERR_NO_MEM, "Create Xiaozhi playback mutex");
        return false;
    }
    _control_ack = _control_ack ? _control_ack : xSemaphoreCreateBinary();
    _control_exited = _control_exited ? _control_exited : xSemaphoreCreateBinary();
    _preroll_done = _preroll_done ? _preroll_done : xSemaphoreCreateBinary();
    _control_queue = _control_queue ? _control_queue :
                     xQueueCreate(CONTROL_QUEUE_LENGTH, sizeof(ControlEvent));
    _pcm_queue = _pcm_queue ? _pcm_queue :
                 xQueueCreateWithCaps(
                     PCM_QUEUE_LENGTH, sizeof(PcmFrame),
                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
                 );
    _playback_queue = _playback_queue ? _playback_queue :
                      xQueueCreate(PLAYBACK_QUEUE_LENGTH, sizeof(AudioPacket));
    if (!_state_mutex || !_lifecycle_mutex || !_audio_tx_mutex || !_control_ack ||
            !_control_exited || !_preroll_done || !_control_queue ||
            !_pcm_queue || !_playback_queue) {
        setError(ESP_ERR_NO_MEM, "Create Xiaozhi synchronization objects");
        return false;
    }

    _activation_client = _activation_client ? _activation_client :
                         new (std::nothrow) XiaozhiActivationClient();
    _activation_prompt = _activation_prompt ? _activation_prompt :
                         new (std::nothrow) XiaozhiActivationPrompt();
    _audio_processor = _audio_processor ? _audio_processor :
                       new (std::nothrow) XiaozhiAudioProcessor();
    _ui = _ui ? _ui : new (std::nothrow) XiaozhiUi();
    if (!_activation_client || !_activation_prompt || !_audio_processor || !_ui) {
        setError(ESP_ERR_NO_MEM, "Create Xiaozhi helpers");
        return false;
    }
    if (!_ui_resources_prepared) {
        (void)_ui->preload();
        _ui_resources_prepared = true;
    }

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        setError(ret, "Initialize network interfaces");
        return false;
    }
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        setError(ret, "Create default event loop");
        return false;
    }
    if (!_ip_event_instance) {
        ret = esp_event_handler_instance_register(
                  IP_EVENT, ESP_EVENT_ANY_ID, ipEventHandler, this, &_ip_event_instance
              );
        if (ret != ESP_OK) {
            setError(ret, "Register network event handler");
            return false;
        }
    }

    _control_running.store(true);
    if (xTaskCreatePinnedToCore(
            controlTaskEntry,
            "xiaozhi_control",
            CONTROL_TASK_STACK_SIZE,
            this,
            5,
            &_control_task,
            0
        ) != pdPASS) {
        _control_running.store(false);
        _control_task = nullptr;
        setError(ESP_ERR_NO_MEM, "Create Xiaozhi control task");
        return false;
    }
    updateNetworkState();
    return true;
}

bool XiaozhiApp::deinit()
{
    ScopedLifecycleAdmission lifecycle_user(_lifecycle_refs);
    if (!lifecycle_user) {
        return false;
    }
    if (_deinit_complete.load()) {
        return true;
    }
    bool expected = false;
    if (!_deinit_in_progress.compare_exchange_strong(expected, true)) {
        TickType_t started = xTaskGetTickCount();
        while (_deinit_in_progress.load() &&
                xTaskGetTickCount() - started <
                    pdMS_TO_TICKS(LIFECYCLE_WAIT_MS)) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        return _deinit_complete.load();
    }
    auto finish_deinit = [this](bool complete) {
        _deinit_complete.store(complete);
        _deinit_in_progress.store(false);
        return complete;
    };

    if (!_lifecycle_mutex && !_control_running.load()) {
        _shutdown.store(true);
        return finish_deinit(true);
    }

    ScopedSemaphoreLock lifecycle_lock(
        _lifecycle_mutex, pdMS_TO_TICKS(LIFECYCLE_WAIT_MS)
    );
    if (!lifecycle_lock) {
        ESP_UTILS_LOGE("Timed out waiting for Xiaozhi lifecycle lock");
        return finish_deinit(false);
    }

    _visible.store(false);
    bool was_shutdown = _shutdown.exchange(true);
    if (_activation_prompt) {
        _activation_prompt->cancel();
    }
    if (_activation_client) {
        _activation_client->cancel();
    }
    bool control_stopped = true;
    if (_control_running.load()) {
        control_stopped = _control_queue && _control_exited;
        if (control_stopped && !was_shutdown) {
            ControlEvent event = {};
            event.type = ControlEventType::Shutdown;
            control_stopped =
                xQueueSendToFront(
                    _control_queue,
                    &event,
                    pdMS_TO_TICKS(LIFECYCLE_WAIT_MS)
                ) == pdTRUE;
            if (!control_stopped) {
                _shutdown.store(false);
            }
        }
        if (control_stopped) {
            control_stopped =
                xSemaphoreTake(
                    _control_exited, pdMS_TO_TICKS(LIFECYCLE_WAIT_MS)
                ) == pdTRUE;
        }
    }
    if (!control_stopped) {
        ESP_UTILS_LOGE("Xiaozhi control task did not stop cleanly");
        return finish_deinit(false);
    }
    if (_ip_event_instance) {
        esp_event_handler_instance_unregister(
            IP_EVENT, ESP_EVENT_ANY_ID, _ip_event_instance
        );
        _ip_event_instance = nullptr;
    }

    TickType_t post_wait_started = xTaskGetTickCount();
    while (_async_post_users.load() != 0 &&
            xTaskGetTickCount() - post_wait_started <
                pdMS_TO_TICKS(LIFECYCLE_WAIT_MS)) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (_async_post_users.load() != 0) {
        ESP_UTILS_LOGE("Xiaozhi callbacks still have active producers");
        return finish_deinit(false);
    }

    // Let the task finish returning from the exit semaphore give before the
    // semaphore and queues are deleted.
    vTaskDelay(1);

    destroyUi();
    delete _ui;
    _ui = nullptr;
    _ui_resources_prepared = false;
    delete _activation_prompt;
    _activation_prompt = nullptr;
    delete _activation_client;
    _activation_client = nullptr;
    delete _audio_processor;
    _audio_processor = nullptr;

    if (_playback_queue) {
        vQueueDelete(_playback_queue);
        _playback_queue = nullptr;
    }
    if (_pcm_queue) {
        vQueueDeleteWithCaps(_pcm_queue);
        _pcm_queue = nullptr;
    }
    if (_control_queue) {
        vQueueDelete(_control_queue);
        _control_queue = nullptr;
    }
    if (_control_exited) {
        vSemaphoreDelete(_control_exited);
        _control_exited = nullptr;
    }
    if (_preroll_done) {
        vSemaphoreDelete(_preroll_done);
        _preroll_done = nullptr;
    }
    if (_control_ack) {
        vSemaphoreDelete(_control_ack);
        _control_ack = nullptr;
    }
    if (_playback_mutex) {
        vSemaphoreDelete(_playback_mutex);
        _playback_mutex = nullptr;
    }
    if (_audio_tx_mutex) {
        vSemaphoreDelete(_audio_tx_mutex);
        _audio_tx_mutex = nullptr;
    }
    if (_state_mutex) {
        vSemaphoreDelete(_state_mutex);
        _state_mutex = nullptr;
    }
    return finish_deinit(true);
}

bool XiaozhiApp::pause()
{
    _visible.store(false);
    if (_activation_prompt) {
        _activation_prompt->cancel();
    }
    if (_activation_client) {
        _activation_client->cancel();
    }
    ControlEvent event = {};
    event.type = ControlEventType::AppHidden;
    event.flag = true;
    if (!postControlEvent(event, true)) {
        return false;
    }
    if (_ui_timer) {
        lv_timer_pause(_ui_timer);
    }
    return true;
}

bool XiaozhiApp::resume()
{
    if (!_page_root) {
        createUi();
    }
    _visible.store(true);
    if (_ui_timer) {
        lv_timer_resume(_ui_timer);
    }
    updateNetworkState();
    ControlEvent event = {};
    event.type = ControlEventType::AppVisible;
    postControlEvent(event);
    return true;
}

void XiaozhiApp::createUi()
{
    destroyUi();
    if (!_ui) {
        return;
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    _page_root = lv_obj_create(screen);
    lv_obj_remove_style_all(_page_root);
    lv_obj_set_size(_page_root, lv_pct(100), lv_pct(100));
    lv_obj_center(_page_root);
    lv_obj_set_style_bg_color(_page_root, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_page_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(_page_root, LV_OBJ_FLAG_SCROLLABLE);

    // Use wrapped subtitles so longer server replies remain readable.
    if (!_ui->create(_page_root, uiActionCallback, this, true)) {
        destroyUi();
        return;
    }
    _ui_timer = lv_timer_create(uiTimerCallback, 100, this);
    refreshUi();
}

void XiaozhiApp::destroyUi()
{
    if (_ui_timer) {
        lv_timer_delete(_ui_timer);
        _ui_timer = nullptr;
    }
    if (_ui) {
        _ui->destroy();
    }
    if (_page_root) {
        lv_obj_delete(_page_root);
        _page_root = nullptr;
    }
}

void XiaozhiApp::refreshUi()
{
    if (!_ui || !_page_root) {
        return;
    }
    char code[sizeof(_activation_code)] = {};
    char message[sizeof(_activation_message)] = {};
    char role[sizeof(_chat_role)] = {};
    char text[sizeof(_chat_text)] = {};
    char emotion[sizeof(_emotion)] = {};
    if (!_state_mutex ||
            xSemaphoreTake(_state_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    memcpy(code, _activation_code, sizeof(code));
    memcpy(message, _activation_message, sizeof(message));
    memcpy(role, _chat_role, sizeof(role));
    memcpy(text, _chat_text, sizeof(text));
    memcpy(emotion, _emotion, sizeof(emotion));
    xSemaphoreGive(_state_mutex);

    State state = _state.load();
    _ui->setNetworkReady(_network_ready.load());
    _ui->setStatus(stateText(state));
    _ui->setChatMessage(role, text);
    _ui->setEmotion(emotion[0] ? emotion : "neutral");
    _ui->setActivation(
        code,
        message[0] ? message : xiaozhiUiText(
            XiaozhiUiText::ActivationInstructions
        ),
        state == State::ActivationRequired
    );
}

void XiaozhiApp::setState(State state)
{
    _state.store(state);
}

void XiaozhiApp::setError(esp_err_t error, const char *source)
{
    ESP_UTILS_LOGE(
        "%s: %s", source ? source : "Xiaozhi", esp_err_to_name(error)
    );
    if (_network_ready.load()) {
        setState(State::Error);
    }
}

void XiaozhiApp::setChatMessage(XiaozhiClient::TextRole role, const char *text)
{
    if (!_state_mutex || xSemaphoreTake(_state_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    copyString(
        _chat_role,
        sizeof(_chat_role),
        role == XiaozhiClient::TextRole::Assistant ? "assistant" : "user"
    );
    copyString(_chat_text, sizeof(_chat_text), text);
    xSemaphoreGive(_state_mutex);
}

void XiaozhiApp::setEmotion(const char *emotion)
{
    if (_state_mutex && xSemaphoreTake(_state_mutex, portMAX_DELAY) == pdTRUE) {
        copyString(_emotion, sizeof(_emotion), emotion ? emotion : "neutral");
        xSemaphoreGive(_state_mutex);
    }
}

void XiaozhiApp::clearConversationUi()
{
    if (_state_mutex && xSemaphoreTake(_state_mutex, portMAX_DELAY) == pdTRUE) {
        _chat_role[0] = '\0';
        _chat_text[0] = '\0';
        copyString(_emotion, sizeof(_emotion), "neutral");
        xSemaphoreGive(_state_mutex);
    }
}

void XiaozhiApp::copyActivation(const char *code, const char *message)
{
    if (_state_mutex && xSemaphoreTake(_state_mutex, portMAX_DELAY) == pdTRUE) {
        copyString(_activation_code, sizeof(_activation_code), code);
        copyString(_activation_message, sizeof(_activation_message), message);
        xSemaphoreGive(_state_mutex);
    }
}

void XiaozhiApp::clearActivation()
{
    if (_state_mutex && xSemaphoreTake(_state_mutex, portMAX_DELAY) == pdTRUE) {
        _activation_code[0] = '\0';
        _activation_message[0] = '\0';
        xSemaphoreGive(_state_mutex);
    }
}

bool XiaozhiApp::announceActivation(bool force)
{
    if (!_visible.load() || !_activation_prompt ||
            _state.load() != State::ActivationRequired) {
        return false;
    }
    char code[sizeof(_activation_code)] = {};
    if (_state_mutex && xSemaphoreTake(_state_mutex, portMAX_DELAY) == pdTRUE) {
        copyString(code, sizeof(code), _activation_code);
        xSemaphoreGive(_state_mutex);
    }
    return _activation_prompt->start(code, force);
}

void XiaozhiApp::stopActivationPrompt()
{
    if (!_activation_prompt) {
        return;
    }
    _activation_prompt->cancel();
    constexpr int MAX_STOP_ATTEMPTS = 3;
    for (int attempt = 1; attempt <= MAX_STOP_ATTEMPTS; ++attempt) {
        if (_activation_prompt->stopAndWait(1000)) {
            return;
        }
        ESP_UTILS_LOGW(
            "Waiting for Xiaozhi prompt audio to stop (%d/%d)",
            attempt,
            MAX_STOP_ATTEMPTS
        );
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // The prompt deliberately retains its codec/session ownership when
    // cleanup fails.  A later stop/start path can retry without freeing live
    // resources or wedging the control task forever.
    ESP_UTILS_LOGE("Xiaozhi prompt cleanup deferred after bounded retries");
}

bool XiaozhiApp::playReadyPromptOnce()
{
    if (_ready_prompt_played.exchange(true)) {
        return true;
    }
    if (!_activation_prompt || !canPrepare()) {
        ESP_UTILS_LOGW("Xiaozhi ready prompt is unavailable");
        return false;
    }

    stopActivationPrompt();
    if (_audio_tasks_running.load()) {
        shutdownAudio();
    }

    bool completed = false;
    if (_activation_prompt->playSuccess()) {
        while (!_activation_prompt->waitUntilIdle(100)) {
            if (!canPrepare()) {
                _activation_prompt->cancel();
                return false;
            }
        }
        completed = _activation_prompt->lastPlaybackCompleted();
    }
    if (completed) {
        ESP_UTILS_LOGI("Xiaozhi self-check completed; success prompt played");
    } else {
        ESP_UTILS_LOGW("Xiaozhi self-check completed without success prompt");
    }
    return completed;
}

bool XiaozhiApp::canPrepare() const
{
    return _visible.load() && _network_ready.load() && isNetworkReady() &&
           !_shutdown.load();
}

bool XiaozhiApp::isNetworkReady() const
{
    return netifHasIpv4("WIFI_STA_DEF") || netifHasIpv4("ETH_DEF");
}

void XiaozhiApp::updateNetworkState()
{
    bool ready = isNetworkReady();
    if (!ready) {
        if (_activation_prompt) {
            _activation_prompt->cancel();
        }
        if (_activation_client) {
            _activation_client->cancel();
        }
    }
    ControlEvent event = {};
    event.type = ControlEventType::NetworkChanged;
    event.flag = ready;
    postControlEvent(event);
}

bool XiaozhiApp::postControlEvent(const ControlEvent &event, bool wait_for_ack)
{
    ScopedLifecycleAdmission lifecycle_user(_lifecycle_refs);
    if (!lifecycle_user) {
        return false;
    }
    ScopedOptionalCounter async_post_user(
        wait_for_ack ? nullptr : &_async_post_users
    );
    if (_destroying.load() || _deinit_in_progress.load() ||
            !_control_queue || (!_control_running.load() &&
                            event.type != ControlEventType::Shutdown) ||
            (_shutdown.load() && event.type != ControlEventType::Shutdown)) {
        return false;
    }
    if (!wait_for_ack) {
        bool best_effort =
            event.type == ControlEventType::ChatText ||
            event.type == ControlEventType::Emotion;
        TickType_t wait_ticks =
            best_effort ? 0 : pdMS_TO_TICKS(CONTROL_EVENT_POST_TIMEOUT_MS);
        bool queued = xQueueSend(_control_queue, &event, wait_ticks) == pdTRUE;
        if (!queued) {
            ESP_UTILS_LOGW(
                "Control queue rejected event %u",
                static_cast<unsigned>(event.type)
            );
            if (!best_effort) {
                _control_event_lost.store(true);
            }
        }
        return queued;
    }
    if (!_lifecycle_mutex || !_control_ack) {
        return false;
    }
    if (xSemaphoreTake(
                _lifecycle_mutex, pdMS_TO_TICKS(LIFECYCLE_WAIT_MS)
            ) != pdTRUE) {
        return false;
    }
    if (!_control_queue || !_control_running.load() || _shutdown.load()) {
        xSemaphoreGive(_lifecycle_mutex);
        return false;
    }
    xSemaphoreTake(_control_ack, 0);
    bool ok =
        xQueueSend(
            _control_queue, &event, pdMS_TO_TICKS(LIFECYCLE_WAIT_MS)
        ) == pdTRUE;
    if (ok) {
        ok =
            xSemaphoreTake(
                _control_ack, pdMS_TO_TICKS(LIFECYCLE_WAIT_MS)
            ) == pdTRUE;
    }
    xSemaphoreGive(_lifecycle_mutex);
    return ok;
}

void XiaozhiApp::controlLoop()
{
    bool exit_requested = false;
    while (!exit_requested) {
        ControlEvent event = {};
        if (xQueueReceive(_control_queue, &event, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (event.type == ControlEventType::Shutdown) {
                exit_requested = true;
            } else {
                handleControlEvent(event);
                if (event.type == ControlEventType::AppHidden && _control_ack) {
                    xSemaphoreGive(_control_ack);
                }
            }
        }
        if (!exit_requested) {
            if (_control_event_lost.exchange(false)) {
                ESP_UTILS_LOGE("Recovering after a dropped Xiaozhi control event");
                setAudioModes(false, false);
                closeChannel(true);
                destroyChat();
                if (_network_ready.load()) {
                    setError(ESP_ERR_TIMEOUT, "Xiaozhi control queue");
                    schedulePrepare(RETRY_INTERVAL_MS);
                } else {
                    setState(State::NetworkRequired);
                }
            }
            prepareIfDue();
            TickType_t uplink_started = _uplink_started_tick.load();
            if (uplink_started != 0 &&
                    _state.load() == State::Listening &&
                    _voice_enabled.load() &&
                    _uplink_ready.load() &&
                    _opus_packets_sent.load() == 0 &&
                    xTaskGetTickCount() - uplink_started >=
                        pdMS_TO_TICKS(UPLINK_FIRST_PACKET_TIMEOUT_MS)) {
                ESP_UTILS_LOGE(
                    "No Xiaozhi Opus uplink packet after listening started"
                );
                _uplink_started_tick.store(0);
                closeChannel(true);
                destroyChat();
                if (_network_ready.load()) {
                    setError(ESP_ERR_TIMEOUT, "Start Xiaozhi audio uplink");
                    schedulePrepare(RETRY_INTERVAL_MS);
                } else {
                    setState(State::NetworkRequired);
                }
            }
        }
    }

    _visible.store(false);
    stopActivationPrompt();
    setAudioModes(false, false);
    closeChannel(true);
    destroyChat();
    while (!shutdownAudio()) {
        ESP_UTILS_LOGW("Waiting to release the Xiaozhi audio session");
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    _control_task = nullptr;
    _control_running.store(false);
    if (_control_exited) {
        xSemaphoreGive(_control_exited);
    }
    vTaskDelete(nullptr);
}

void XiaozhiApp::handleControlEvent(const ControlEvent &event)
{
    if ((event.chat_generation != 0 &&
            event.chat_generation != _chat_generation.load()) ||
            (event.turn_generation != 0 &&
             event.turn_generation != _turn_generation.load())) {
        ESP_UTILS_LOGD("Ignoring a stale Xiaozhi control event");
        return;
    }

    switch (event.type) {
    case ControlEventType::AppVisible:
        handleAppVisible();
        break;
    case ControlEventType::AppHidden:
        handleAppHidden(event.flag);
        break;
    case ControlEventType::NetworkChanged:
        _network_ready.store(event.flag);
        if (!event.flag) {
            stopActivationPrompt();
            setState(State::NetworkRequired);
            setAudioModes(false, false);
            closeChannel(false);
            destroyChat();
        } else if (!_chat_started.load() && _visible.load()) {
            if (_activation_phase == ActivationPhase::WaitingForBinding ||
                    _activation_phase == ActivationPhase::BoundPendingSuccess) {
                setState(State::ActivationRequired);
                if (_activation_phase == ActivationPhase::WaitingForBinding) {
                    announceActivation(false);
                }
            } else {
                setState(State::Preparing);
            }
            schedulePrepare(0);
        } else if (_transport_connected) {
            handleAppVisible();
        }
        break;
    case ControlEventType::ActionPressed:
        if (_state.load() == State::ActivationRequired) {
            announceActivation(true);
        } else if (_state.load() == State::Ready) {
            startListening(ListenMode::AutoStop);
        } else if (_state.load() == State::Listening &&
                   _listen_mode == ListenMode::Manual) {
            finishListening();
        } else if ((_state.load() == State::Speaking ||
                    _state.load() == State::Processing) &&
                   _listen_mode == ListenMode::Manual) {
            cancelManualConversation();
        } else if (_state.load() == State::Listening ||
                   _state.load() == State::Speaking ||
                   _state.load() == State::Processing) {
            closeChannel(true);
            setState(State::Ready);
            enterReadyAudioMode();
        }
        break;
    case ControlEventType::AnnounceActivation:
        announceActivation(true);
        break;
    case ControlEventType::WakeWord:
    {
        const State state = _state.load();
        XiaozhiClient *protocol = _protocol.load();
        if (_visible.load() && state == State::Ready) {
            startListening(ListenMode::AutoStop, event.text);
        } else if (_visible.load() && state == State::Speaking &&
                   _channel_state.load() == ChannelState::Open &&
                   protocol) {
            esp_err_t abort_ret = ESP_ERR_INVALID_STATE;
            if (_audio_tx_mutex &&
                    xSemaphoreTake(_audio_tx_mutex, portMAX_DELAY) == pdTRUE) {
                abort_ret = protocol->sendAbortSpeaking(
                    XiaozhiClient::AbortReason::WakeWordDetected
                );
                xSemaphoreGive(_audio_tx_mutex);
            }
            if (abort_ret != ESP_OK) {
                ESP_UTILS_LOGW(
                    "Failed to abort speaking after wake word: 0x%x",
                    static_cast<unsigned int>(abort_ret)
                );
                if (_audio_processor && _audio_processor->isReady()) {
                    _audio_processor->enableWakeWord(true);
                }
            } else {
                startListening(ListenMode::AutoStop, event.text);
            }
        } else if (!_visible.load()) {
            setAudioModes(false, false);
        } else {
            ESP_UTILS_LOGD("Wake word ignored in the current conversation state");
            if (_audio_processor && _audio_processor->isReady()) {
                _audio_processor->enableWakeWord(true);
            }
        }
        break;
    }
    case ControlEventType::TransportConnected:
        _transport_connected = true;
        _server_goodbye_generation.store(0);
        if (_chat_started.load() &&
                (_state.load() == State::Connecting ||
                 _state.load() == State::Preparing ||
                 _state.load() == State::Error)) {
            setState(State::Ready);
            if (_visible.load()) {
                (void)playReadyPromptOnce();
                if (canPrepare() && ensureAudioReady()) {
                    enterReadyAudioMode();
                } else if (canPrepare()) {
                    schedulePrepare(RETRY_INTERVAL_MS);
                }
            }
        }
        break;
    case ControlEventType::TransportDisconnected: {
        const uint32_t goodbye_generation =
            _server_goodbye_generation.exchange(0);
        const bool reconnect_now = goodbye_generation == event.chat_generation;
        const bool drain_final_response =
            _tts_stop_pending.load() && _state.load() == State::Speaking;
        _transport_connected = false;
        if (drain_final_response) {
            _uplink_ready.store(false);
            _channel_state.store(ChannelState::Closing);
            _restart_transport_after_playback.store(true);
            ESP_UTILS_LOGI(
                "Transport closed after final TTS; reconnecting after playback drains"
            );
            break;
        }

        _restart_transport_after_playback.store(false);
        resetChannelState();
        setAudioModes(false, false);
        if (reconnect_now) {
            ESP_UTILS_LOGI(
                "Transport closed after server goodbye; reconnecting now"
            );
            destroyChat();
        } else {
            ESP_UTILS_LOGW(
                "Xiaozhi transport disconnected; waiting for auto reconnect"
            );
        }
        if (_network_ready.load()) {
            setState(State::Connecting);
            schedulePrepare(
                reconnect_now ? 0 : TRANSPORT_RECONNECT_FALLBACK_MS
            );
        } else {
            setState(State::NetworkRequired);
        }
        break;
    }
    case ControlEventType::ChannelOpened:
        if (_channel_state.load() == ChannelState::Opening) {
            _channel_state.store(ChannelState::Open);
        }
        break;
    case ControlEventType::ChannelClosed:
        if (_channel_state.load() == ChannelState::Closing) {
            resetChannelState();
            if (_network_ready.load() && _transport_connected && _visible.load()) {
                setState(State::Ready);
                enterReadyAudioMode();
            }
        }
        break;
    case ControlEventType::ServerGoodbye:
        _server_goodbye_generation.store(event.chat_generation);
        closeChannel(false);
        if (_network_ready.load() && _transport_connected) {
            ESP_UTILS_LOGI(
                "Server ended the conversation; wake-word standby restored"
            );
            setState(State::Ready);
            enterReadyAudioMode();
        }
        break;
    case ControlEventType::ChatText:
        if (_channel_state.load() == ChannelState::Open &&
                _listen_mode != ListenMode::None) {
            ESP_UTILS_LOGI(
                "Server text received (role=%u, bytes=%u)",
                static_cast<unsigned int>(event.role),
                static_cast<unsigned int>(strlen(event.text))
            );
            setChatMessage(
                static_cast<XiaozhiClient::TextRole>(event.role), event.text
            );
            const char *history_role =
                static_cast<XiaozhiClient::TextRole>(event.role) ==
                    XiaozhiClient::TextRole::Assistant ? "assistant" : "user";
            const esp_err_t history_ret = chat_history_append(history_role, event.text);
            if (history_ret != ESP_OK && history_ret != ESP_ERR_INVALID_STATE &&
                    history_ret != ESP_ERR_TIMEOUT) {
                ESP_UTILS_LOGW(
                    "Queue SD chat history failed: %s",
                    esp_err_to_name(history_ret)
                );
            }
        }
        break;
    case ControlEventType::Emotion:
        if (_channel_state.load() == ChannelState::Open &&
                _listen_mode != ListenMode::None) {
            setEmotion(event.text);
        }
        break;
    case ControlEventType::TtsStart: {
        State state = _state.load();
        if (_channel_state.load() == ChannelState::Open &&
                _listen_mode != ListenMode::None &&
                (state == State::Listening ||
                 state == State::Processing ||
                 state == State::Speaking)) {
            if (!setAudioModes(_visible.load(), false) && _visible.load()) {
                closeChannel(false);
                shutdownAudio();
                destroyChat();
                setError(ESP_FAIL, "Enable wake-word interruption");
                schedulePrepare(RETRY_INTERVAL_MS);
                break;
            }
            _awaiting_response = false;
            _tts_active = true;
            _tts_stop_pending.store(false);
            _accept_playback.store(true);
            ESP_UTILS_LOGI("Server TTS started");
            setState(State::Speaking);
        }
        break;
    }
    case ControlEventType::TtsStop: {
        if (_channel_state.load() == ChannelState::Open &&
                (_tts_active || _state.load() == State::Speaking)) {
            TickType_t now = xTaskGetTickCount();
            _tts_active = false;
            ESP_UTILS_LOGI("Server TTS stopped; draining playback");
            _tts_stop_tick.store(now);
            _last_audio_packet_tick.store(now);
            _last_audio_write_tick.store(now);
            _drain_event_posted.store(false);
            _tts_stop_pending.store(true);
        }
        break;
    }
    case ControlEventType::PlaybackDrained:
        handlePlaybackDrained();
        break;
    case ControlEventType::ChatError:
        closeChannel(true);
        destroyChat();
        if (_network_ready.load()) {
            setError(event.error, event.detail);
            schedulePrepare(RETRY_INTERVAL_MS);
        } else {
            setState(State::NetworkRequired);
        }
        break;
    case ControlEventType::AudioError:
        closeChannel(true);
        shutdownAudio();
        if (_network_ready.load()) {
            setError(event.error, event.detail);
            bool recovered = false;
            if (_visible.load() && _transport_connected &&
                    _chat_started.load() && ensureAudioReady()) {
                setState(State::Ready);
                recovered = enterReadyAudioMode();
            }
            if (!recovered) {
                schedulePrepare(RETRY_INTERVAL_MS);
            }
        } else {
            setState(State::NetworkRequired);
        }
        break;
    case ControlEventType::Shutdown:
        break;
    }
}

void XiaozhiApp::handleAppVisible()
{
    _visible.store(true);
    if (!_network_ready.load() || !isNetworkReady()) {
        setState(State::NetworkRequired);
    } else if (_activation_phase == ActivationPhase::WaitingForBinding ||
               _activation_phase == ActivationPhase::BoundPendingSuccess) {
        setState(State::ActivationRequired);
        if (_activation_phase == ActivationPhase::WaitingForBinding) {
            announceActivation(false);
        }
        schedulePrepare(0);
    } else if (!_chat_started.load()) {
        setState(State::Preparing);
        schedulePrepare(0);
    } else if (!_transport_connected) {
        setState(State::Connecting);
    } else if (_tts_active || _tts_stop_pending.load()) {
        setState(State::Speaking);
    } else if (_awaiting_response) {
        setState(State::Processing);
    } else {
        setState(State::Ready);
        (void)playReadyPromptOnce();
        if (!ensureAudioReady()) {
            schedulePrepare(RETRY_INTERVAL_MS);
        } else {
            enterReadyAudioMode();
        }
    }
}

void XiaozhiApp::handleAppHidden(bool release_audio)
{
    _visible.store(false);
    stopActivationPrompt();
    setAudioModes(false, false);
    if (release_audio) {
        closeChannel(true);
        shutdownAudio();
    } else if (_listen_mode != ListenMode::None && !_tts_active) {
        stopUplink(true);
    }
}

void XiaozhiApp::prepareIfDue()
{
    if (!canPrepare() ||
            !tickReached(xTaskGetTickCount(), _next_prepare_tick)) {
        return;
    }

    if (_chat_started.load()) {
        if (!_transport_connected && _state.load() == State::Connecting) {
            ESP_UTILS_LOGW(
                "Xiaozhi auto reconnect timed out; restarting transport"
            );
            destroyChat();
            if (canPrepare()) {
                prepareChat();
            }
            return;
        }
        if (_transport_connected && _visible.load() &&
                _state.load() == State::Error &&
                !_audio_tasks_running.load()) {
            if (ensureAudioReady()) {
                setState(State::Ready);
                enterReadyAudioMode();
            } else {
                schedulePrepare(RETRY_INTERVAL_MS);
            }
        }
        return;
    }
    prepareChat();
}

void XiaozhiApp::schedulePrepare(uint32_t delay_ms)
{
    _next_prepare_tick = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
}

bool XiaozhiApp::prepareChat()
{
    if (!canPrepare()) {
        return false;
    }
    if (!_activation_client) {
        setError(ESP_ERR_INVALID_STATE, "Initialize Xiaozhi activation");
        schedulePrepare(RETRY_INTERVAL_MS);
        return false;
    }

    setState(State::Preparing);
    const bool was_waiting =
        _activation_phase == ActivationPhase::WaitingForBinding ||
        _activation_phase == ActivationPhase::BoundPendingSuccess;
    ScopedXiaozhiInfo service;
    logServiceInfoHeap("before request");
    esp_err_t ret = esp_xiaozhi_chat_get_info(&service.value);
    if (ret != ESP_OK) {
        logServiceInfoHeap(esp_err_to_name(ret));
    }
    if (!canPrepare()) {
        return false;
    }
    if (ret != ESP_OK) {
        if (was_waiting) {
            ESP_UTILS_LOGW(
                "Activation status check failed: %s",
                esp_err_to_name(ret)
            );
            setState(State::ActivationRequired);
        } else {
            setError(ret, "Get Xiaozhi service information");
        }
        schedulePrepare(RETRY_INTERVAL_MS);
        return false;
    }

    if (service.value.has_activation_code ||
            service.value.has_activation_challenge) {
        copyActivation(
            service.value.activation_code,
            service.value.activation_message
        );
        _activation_phase = ActivationPhase::WaitingForBinding;
        setState(State::ActivationRequired);
        announceActivation(false);

        const uint32_t delay_ms =
            activationRetryDelay(service.value.activation_timeout_ms);
        if (!service.value.has_activation_challenge ||
                !service.value.activation_challenge ||
                service.value.activation_challenge[0] == 0) {
            schedulePrepare(delay_ms);
            return false;
        }

        XiaozhiActivationClient::Info activation = {};
        activation.state =
            XiaozhiActivationClient::BindingState::ActivationRequired;
        activation.challenge = service.value.activation_challenge;
        int http_status = 0;
        ret = _activation_client->activate(activation, http_status);
        if (!canPrepare()) {
            return false;
        }
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
            ESP_UTILS_LOGW(
                "Activation request failed: %s (HTTP %d)",
                esp_err_to_name(ret),
                http_status
            );
        } else if (ret == ESP_OK) {
            _activation_phase = ActivationPhase::BoundPendingSuccess;
            schedulePrepare(0);
            return false;
        }
        schedulePrepare(delay_ms);
        return false;
    }

    if (was_waiting) {
        stopActivationPrompt();
        if (!canPrepare()) {
            return false;
        }
    }
    clearActivation();
    _activation_phase = ActivationPhase::Complete;

    bool use_mqtt = false;
    bool use_websocket = false;
#if CONFIG_XIAOZHI_APP_TRANSPORT_MQTT
    use_mqtt = service.value.has_mqtt_config;
#elif CONFIG_XIAOZHI_APP_TRANSPORT_AUTO
    use_mqtt = service.value.has_mqtt_config;
    use_websocket = service.value.has_websocket_config;
#else
    use_websocket = service.value.has_websocket_config;
#endif
    if (!use_mqtt && !use_websocket) {
#if CONFIG_XIAOZHI_APP_TRANSPORT_MQTT
        setError(ESP_ERR_NOT_SUPPORTED, "Get Xiaozhi MQTT configuration");
#elif CONFIG_XIAOZHI_APP_TRANSPORT_AUTO
        setError(ESP_ERR_NOT_SUPPORTED, "Get Xiaozhi transport configuration");
#else
        setError(ESP_ERR_NOT_SUPPORTED, "Get Xiaozhi WebSocket configuration");
#endif
        schedulePrepare(RETRY_INTERVAL_MS);
        return false;
    }
    if (!canPrepare()) {
        return false;
    }

    const uint32_t client_generation = _chat_generation.fetch_add(1) + 1;
    auto post_simple = [this, client_generation](ControlEventType type) {
        ControlEvent event = {};
        event.type = type;
        event.chat_generation = client_generation;
        (void)postControlEvent(event);
    };

    XiaozhiClient::Callbacks callbacks = {};
    callbacks.onConnected = [post_simple]() {
        post_simple(ControlEventType::TransportConnected);
    };
    callbacks.onDisconnected = [this, post_simple]() {
        if (_chat_started.load()) {
            post_simple(ControlEventType::TransportDisconnected);
        }
    };
    callbacks.onAudioChannelOpened = [post_simple]() {
        post_simple(ControlEventType::ChannelOpened);
    };
    callbacks.onAudioChannelClosed = [post_simple]() {
        post_simple(ControlEventType::ChannelClosed);
    };
    callbacks.onServerGoodbye = [this, post_simple, client_generation]() {
        if (_chat_generation.load() == client_generation) {
            uint32_t marked_generation =
                _server_goodbye_generation.load();
            while (marked_generation < client_generation &&
                    !_server_goodbye_generation.compare_exchange_weak(
                        marked_generation, client_generation
                    )) {
            }
        }
        post_simple(ControlEventType::ServerGoodbye);
    };
    callbacks.onText = [this, client_generation](
        XiaozhiClient::TextRole role,
        const std::string &text
    ) {
        ControlEvent event = {};
        event.type = ControlEventType::ChatText;
        event.role = static_cast<uint8_t>(role);
        event.chat_generation = client_generation;
        event.turn_generation = _turn_generation.load();
        copyString(event.text, sizeof(event.text), text.c_str());
        (void)postControlEvent(event);
    };
    callbacks.onEmotion = [this, client_generation](const std::string &emotion) {
        ControlEvent event = {};
        event.type = ControlEventType::Emotion;
        event.chat_generation = client_generation;
        event.turn_generation = _turn_generation.load();
        copyString(event.text, sizeof(event.text), emotion.c_str());
        (void)postControlEvent(event);
    };
    callbacks.onTts = [this, client_generation](
        XiaozhiClient::TtsState state,
        const std::string &text
    ) {
        if (_chat_generation.load() != client_generation) {
            return;
        }
        (void)text;
        if (state == XiaozhiClient::TtsState::SentenceStart) {
            return;
        }
        ControlEvent event = {};
        event.type = state == XiaozhiClient::TtsState::Start ?
                     ControlEventType::TtsStart : ControlEventType::TtsStop;
        event.chat_generation = client_generation;
        event.turn_generation = _turn_generation.load();
        if (state == XiaozhiClient::TtsState::Start &&
                _channel_state.load() == ChannelState::Open &&
                _chat_started.load() && !_shutdown.load()) {
            _accept_playback.store(true);
        }
        (void)postControlEvent(event);
    };
    callbacks.onAudio = [this, client_generation](
        const uint8_t *data,
        size_t len,
        int sample_rate,
        int frame_duration_ms,
        uint32_t timestamp
    ) {
        ScopedLifecycleAdmission lifecycle_user(_lifecycle_refs);
        if (!lifecycle_user) {
            return;
        }
        ScopedOptionalCounter async_user(&_async_post_users);
        if (_chat_generation.load() != client_generation ||
                !data || len == 0 || len > sizeof(AudioPacket::data) ||
                !_playback_queue || !_audio_tasks_running.load() ||
                !_accept_playback.load() ||
                _channel_state.load() != ChannelState::Open ||
                _shutdown.load()) {
            return;
        }

        AudioPacket packet = {};
        packet.turn_generation = _turn_generation.load();
        packet.timestamp = timestamp;
        packet.sample_rate = static_cast<uint16_t>(
            sample_rate > 0 && sample_rate <= UINT16_MAX ?
            sample_rate : CHAT_SAMPLE_RATE
        );
        packet.frame_duration_ms = static_cast<uint16_t>(
            frame_duration_ms > 0 && frame_duration_ms <= UINT16_MAX ?
            frame_duration_ms : CHAT_FRAME_DURATION_MS
        );
        packet.size = static_cast<uint16_t>(len);
        memcpy(packet.data, data, len);

        if (!_playback_mutex ||
                xSemaphoreTake(
                    _playback_mutex,
                    pdMS_TO_TICKS(CONTROL_EVENT_POST_TIMEOUT_MS)
                ) != pdTRUE) {
            return;
        }
        if (_accept_playback.load() &&
                _chat_generation.load() == client_generation &&
                packet.turn_generation == _turn_generation.load() &&
                _channel_state.load() == ChannelState::Open &&
                xQueueSend(_playback_queue, &packet, 0) == pdTRUE) {
            _last_audio_packet_tick.store(xTaskGetTickCount());
        }
        xSemaphoreGive(_playback_mutex);
    };
    callbacks.onError = [this, client_generation](
        esp_err_t error,
        const std::string &source
    ) {
        ControlEvent event = {};
        event.type = ControlEventType::ChatError;
        event.error = error;
        event.chat_generation = client_generation;
        event.turn_generation = _turn_generation.load();
        copyString(event.detail, sizeof(event.detail), source.c_str());
        (void)postControlEvent(event);
    };

    XiaozhiClient::Config protocol_config(
        use_mqtt,
        use_websocket,
        CHAT_SAMPLE_RATE,
        1,
        CHAT_FRAME_DURATION_MS,
        DEVICE_AUDIO_SAMPLE_RATE
    );
    auto *protocol = new (std::nothrow) XiaozhiClient(
        std::move(protocol_config), std::move(callbacks)
    );
    if (!protocol) {
        setError(ESP_ERR_NO_MEM, "Create Xiaozhi protocol");
        schedulePrepare(RETRY_INTERVAL_MS);
        return false;
    }

    _turn_generation.fetch_add(1);
    _protocol.store(protocol);
    _chat_initialized.store(true);
    _chat_started.store(true);
    _transport_connected = false;
    setState(State::Connecting);

    ret = protocol->start();
    if (ret != ESP_OK || !canPrepare()) {
        destroyChat();
        if (ret != ESP_OK) {
            setError(ret, "Start Xiaozhi protocol");
            schedulePrepare(RETRY_INTERVAL_MS);
        }
        return false;
    }

    schedulePrepare(TRANSPORT_RECONNECT_FALLBACK_MS);
    ESP_UTILS_LOGI("Xiaozhi transport started; waiting for connection");
    return true;
}

void XiaozhiApp::destroyChat()
{
    setAudioModes(false, false);
    _uplink_ready.store(false);
    _chat_started.store(false);
    _chat_initialized.store(false);
    _channel_state.store(ChannelState::Closing);

    XiaozhiClient *protocol = nullptr;
    if (_audio_tx_mutex &&
            xSemaphoreTake(_audio_tx_mutex, portMAX_DELAY) == pdTRUE) {
        protocol = _protocol.exchange(nullptr);
        if (protocol) {
            (void)protocol->stop();
        }
        xSemaphoreGive(_audio_tx_mutex);
    } else {
        protocol = _protocol.exchange(nullptr);
    }
    delete protocol;

    _chat_generation.fetch_add(1);
    _turn_generation.fetch_add(1);
    _transport_connected = false;
    _server_goodbye_generation.store(0);
    resetChannelState();
}

bool XiaozhiApp::ensureAudioReady()
{
    const bool input_resamplers_ready =
        _input_resamplers[0] && _input_resamplers[1] && _input_resamplers[2];
    if (_audio_tasks_running.load() && _input_task_running.load() &&
            _encoder_task_running.load() && _playback_task_running.load() &&
            _audio_processor && _audio_processor->isReady() &&
            _opus_encoder && _opus_decoder && input_resamplers_ready &&
            _input_resampler_max_samples > 0) {
        return true;
    }
    stopActivationPrompt();
    if (!shutdownAudio()) {
        setError(ESP_FAIL, "Release previous audio session");
        return false;
    }

    esp_err_t ret = bsp_extra_audio_session_acquire(BSP_EXTRA_AUDIO_OWNER_XIAOZHI);
    if (ret != ESP_OK) {
        ESP_UTILS_LOGW(
            "Xiaozhi audio is busy (%s)",
            bsp_extra_audio_owner_name(bsp_extra_audio_session_get_owner())
        );
        setError(ret, "Acquire audio session");
        return false;
    }
    _audio_session_acquired.store(true);

    ret = bsp_extra_codec_set_voice_fs(
        DEVICE_AUDIO_SAMPLE_RATE,
        16,
        CODEC_VOICE_INPUT_CHANNELS,
        BSP_EXTRA_ES7210_TDM_ALL_SLOTS_MASK,
        BSP_EXTRA_ES7210_PHYSICAL_CONNECTED_MIC_MASK
    );
    if (ret != ESP_OK) {
        setError(ret, "Configure audio codec");
        shutdownAudio();
        return false;
    }
    _codec_claimed.store(true);
    if (!_audio_processor || !_audio_processor->initialize()) {
        setError(ESP_FAIL, "Initialize Xiaozhi audio processor");
        shutdownAudio();
        return false;
    }

    esp_ae_rate_cvt_cfg_t input_resampler_config = {};
    input_resampler_config.src_rate = DEVICE_AUDIO_SAMPLE_RATE;
    input_resampler_config.dest_rate = CHAT_SAMPLE_RATE;
    input_resampler_config.channel = ESP_AUDIO_MONO;
    input_resampler_config.bits_per_sample = ESP_AUDIO_BIT16;
    input_resampler_config.complexity = 2;
    input_resampler_config.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
    int input_resampler_ret = ESP_AE_ERR_OK;
    uint32_t max_samples = 0;
    bool input_resamplers_valid = true;
    for (size_t channel = 0; channel < AFE_INPUT_CHANNELS; ++channel) {
        input_resampler_ret = esp_ae_rate_cvt_open(
            &input_resampler_config, &_input_resamplers[channel]
        );
        uint32_t channel_max_samples = 0;
        if (input_resampler_ret != ESP_AE_ERR_OK ||
                !_input_resamplers[channel] ||
                esp_ae_rate_cvt_get_max_out_sample_num(
                    _input_resamplers[channel],
                    INPUT_READ_FRAMES,
                    &channel_max_samples
                ) != ESP_AE_ERR_OK ||
                channel_max_samples == 0 ||
                (max_samples != 0 && channel_max_samples != max_samples)) {
            input_resamplers_valid = false;
            break;
        }
        max_samples = channel_max_samples;
    }
    _input_resampler_max_samples = max_samples;
    if (!input_resamplers_valid ||
            input_resampler_ret != ESP_AE_ERR_OK ||
            !_input_resamplers[AFE_INPUT_CHANNELS - 1] ||
            _input_resampler_max_samples == 0) {
        ESP_UTILS_LOGE(
            "Open Xiaozhi input resampler failed: %d",
            static_cast<int>(input_resampler_ret)
        );
        setError(ESP_FAIL, "Open Xiaozhi input resampler");
        shutdownAudio();
        return false;
    }
    _audio_processor->setWakeWordCallback([this](const std::string &word) {
        ControlEvent event = {};
        event.type = ControlEventType::WakeWord;
        copyString(event.text, sizeof(event.text), word.c_str());
        if (!postControlEvent(event)) {
            ESP_UTILS_LOGW("Dropped Xiaozhi wake-word control event");
            if (_audio_processor && _audio_processor->isReady()) {
                _audio_processor->enableWakeWord(true);
            }
        }
    });
    _audio_processor->setPcmFrameCallback(
        [this](const int16_t *pcm, size_t sample_count) {
            if (!_capture_enabled.load() || !_voice_enabled.load() ||
                    !_uplink_ready.load() || !_pcm_queue || !pcm ||
                    sample_count != XiaozhiAudioProcessor::PCM_FRAME_SAMPLES) {
                return;
            }
            uint32_t peak = 0;
            uint32_t absolute_sum = 0;
            for (size_t i = 0; i < sample_count; ++i) {
                int32_t value = pcm[i];
                uint32_t magnitude = static_cast<uint32_t>(
                    value < 0 ? -value : value
                );
                peak = magnitude > peak ? magnitude : peak;
                absolute_sum += magnitude;
            }
            _pcm_peak.store(peak);
            _pcm_mean_abs.store(absolute_sum / sample_count);

            PcmFrame frame = {};
            memcpy(frame.samples, pcm, sizeof(frame.samples));
            BaseType_t queued = xQueueSend(_pcm_queue, &frame, 0);
            if (queued != pdTRUE) {
                PcmFrame oldest = {};
                if (xQueueReceive(_pcm_queue, &oldest, 0) == pdTRUE) {
                    queued = xQueueSend(_pcm_queue, &frame, 0);
                }
                if (_voice_enabled.load() && _uplink_ready.load()) {
                    uint32_t dropped = _pcm_frames_dropped.fetch_add(1) + 1;
                    if (dropped == 1 || (dropped % 20) == 0) {
                        ESP_UTILS_LOGW(
                            "Xiaozhi PCM queue overflow; dropped %lu frame(s)",
                            static_cast<unsigned long>(dropped)
                        );
                    }
                }
            }
            if (queued == pdTRUE && _voice_enabled.load()) {
                uint32_t captured = _pcm_frames_captured.fetch_add(1) + 1;
                if (captured == 1) {
                    ESP_UTILS_LOGI(
                        "Captured first 60 ms conversation PCM frame"
                    );
                }
            }
        }
    );
    _audio_processor->setVadCallback([this](bool speaking) {
        if (_voice_enabled.load()) {
            const unsigned int queued = _pcm_queue ?
                static_cast<unsigned int>(uxQueueMessagesWaiting(_pcm_queue)) :
                0;
            ESP_UTILS_LOGI(
                "Local VAD: speech %s (pcm=%lu, opus=%lu, queued=%u, "
                "mic24_peak=%lu, mic24_mean=%lu, afe_peak=%lu, "
                "afe_mean=%lu, uplink=%d)",
                speaking ? "started" : "stopped",
                static_cast<unsigned long>(_pcm_frames_captured.load()),
                static_cast<unsigned long>(_opus_packets_sent.load()),
                queued,
                static_cast<unsigned long>(_input_pcm_peak.load()),
                static_cast<unsigned long>(_input_pcm_mean_abs.load()),
                static_cast<unsigned long>(_pcm_peak.load()),
                static_cast<unsigned long>(_pcm_mean_abs.load()),
                _uplink_ready.load()
            );
        }
    });

    esp_opus_enc_config_t enc = ESP_OPUS_ENC_CONFIG_DEFAULT();
    enc.sample_rate = CHAT_SAMPLE_RATE;
    enc.channel = ESP_AUDIO_MONO;
    enc.bits_per_sample = ESP_AUDIO_BIT16;
    enc.bitrate = ESP_OPUS_BITRATE_AUTO;
    enc.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    enc.application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO;
    enc.complexity = 0;
    enc.enable_fec = false;
    enc.enable_dtx = true;
    enc.enable_vbr = true;
    if (esp_opus_enc_open(&enc, sizeof(enc), &_opus_encoder) != ESP_AUDIO_ERR_OK ||
            !_opus_encoder ||
            esp_opus_enc_get_frame_size(
                _opus_encoder, &_opus_input_size, &_opus_output_size
            ) != ESP_AUDIO_ERR_OK ||
            _opus_input_size != static_cast<int>(sizeof(PcmFrame::samples))) {
        setError(ESP_FAIL, "Open OPUS encoder");
        shutdownAudio();
        return false;
    }

    if (!configureDecoder(DEVICE_AUDIO_SAMPLE_RATE, CHAT_FRAME_DURATION_MS)) {
        setError(ESP_FAIL, "Open OPUS decoder");
        shutdownAudio();
        return false;
    }

    _audio_tasks_running.store(true);
    ESP_UTILS_LOGI(
        "Audio task heap before create: internal free=%u, largest=%u; PSRAM free=%u, largest=%u",
        static_cast<unsigned int>(
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
        ),
        static_cast<unsigned int>(
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
        ),
        static_cast<unsigned int>(
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        ),
        static_cast<unsigned int>(
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        )
    );
    _input_task_running.store(true);
    if (xTaskCreatePinnedToCore(
            inputTaskEntry,
            "xiaozhi_input",
            INPUT_TASK_STACK_SIZE,
            this,
            8,
            &_input_task,
            0
        ) != pdPASS) {
        _input_task_running.store(false);
        setError(ESP_ERR_NO_MEM, "Create Xiaozhi input task");
        shutdownAudio();
        return false;
    }
    _encoder_task_running.store(true);
    if (xTaskCreatePinnedToCoreWithCaps(
            encoderTaskEntry,
            "xiaozhi_encoder",
            ENCODER_TASK_STACK_SIZE,
            this,
            2,
            &_encoder_task,
            0,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        ) != pdPASS) {
        _encoder_task_running.store(false);
        setError(ESP_ERR_NO_MEM, "Create Xiaozhi encoder task");
        shutdownAudio();
        return false;
    }
    _playback_task_running.store(true);
    if (xTaskCreatePinnedToCoreWithCaps(
            playbackTaskEntry,
            "xiaozhi_playback",
            PLAYBACK_TASK_STACK_SIZE,
            this,
            6,
            &_playback_task,
            0,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        ) != pdPASS) {
        _playback_task_running.store(false);
        setError(ESP_ERR_NO_MEM, "Create Xiaozhi playback task");
        shutdownAudio();
        return false;
    }
    return true;
}

bool XiaozhiApp::shutdownAudio()
{
    finishPreroll(ESP_ERR_INVALID_STATE);
    setAudioModes(false, false);
    _accept_playback.store(false);
    _audio_tasks_running.store(false);
    while (_input_task_running.load() || _encoder_task_running.load() ||
            _playback_task_running.load()) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    _input_task = nullptr;
    _encoder_task = nullptr;
    _playback_task = nullptr;
    if (_audio_processor) {
        _audio_processor->shutdown();
    }
    if (_opus_encoder) {
        esp_opus_enc_close(_opus_encoder);
        _opus_encoder = nullptr;
    }
    for (auto &input_resampler : _input_resamplers) {
        if (input_resampler) {
            esp_ae_rate_cvt_close(input_resampler);
            input_resampler = nullptr;
        }
    }
    if (_playback_mutex &&
            xSemaphoreTake(_playback_mutex, portMAX_DELAY) == pdTRUE) {
        if (_opus_decoder) {
            esp_opus_dec_close(_opus_decoder);
            _opus_decoder = nullptr;
        }
        if (_output_resampler) {
            esp_ae_rate_cvt_close(_output_resampler);
            _output_resampler = nullptr;
        }
        if (_playback_queue) {
            xQueueReset(_playback_queue);
        }
        xSemaphoreGive(_playback_mutex);
    } else {
        if (_opus_decoder) {
            esp_opus_dec_close(_opus_decoder);
            _opus_decoder = nullptr;
        }
        if (_output_resampler) {
            esp_ae_rate_cvt_close(_output_resampler);
            _output_resampler = nullptr;
        }
    }
    _opus_input_size = 0;
    _opus_output_size = 0;
    _input_resampler_max_samples = 0;
    _input_resampler_reset_requested.store(false);
    _decoder_sample_rate = 0;
    _decoder_frame_duration_ms = 0;
    if (_pcm_queue) {
        xQueueReset(_pcm_queue);
    }
    const bool released = releaseAudioSession();
    if (!released) {
        ESP_UTILS_LOGE("Xiaozhi retained the audio session after shutdown failure");
    }
    return released;
}

bool XiaozhiApp::releaseAudioSession()
{
    if (_codec_claimed.load()) {
        const esp_err_t stop_result = bsp_extra_codec_dev_stop();
        if (stop_result != ESP_OK) {
            ESP_UTILS_LOGE("Xiaozhi codec shutdown failed: %s", esp_err_to_name(stop_result));
            return false;
        }
        _codec_claimed.store(false);
    }
    if (_audio_session_acquired.load()) {
        const esp_err_t release_result =
            bsp_extra_audio_session_release(BSP_EXTRA_AUDIO_OWNER_XIAOZHI);
        if (release_result != ESP_OK) {
            ESP_UTILS_LOGE("Xiaozhi audio-session release failed: %s",
                           esp_err_to_name(release_result));
            return false;
        }
        _audio_session_acquired.store(false);
    }
    return true;
}

bool XiaozhiApp::setAudioModes(bool wake_word, bool voice)
{
    _capture_enabled.store(false);
    _input_resampler_reset_requested.store(true);
    _uplink_ready.store(false);
    _uplink_started_tick.store(0);
    _voice_enabled.store(false);
    bool success = !wake_word && !voice;
    if (_audio_processor && _audio_processor->isReady()) {
        bool voice_ok = _audio_processor->enableVoiceProcessing(voice);
        bool wake_word_ok = _audio_processor->enableWakeWord(wake_word);
        success = voice_ok && wake_word_ok;
        if (!success) {
            (void)_audio_processor->enableWakeWord(false);
            (void)_audio_processor->enableVoiceProcessing(false);
            wake_word = false;
            voice = false;
        }
    }
    if (_pcm_queue && (!voice || !success)) {
        xQueueReset(_pcm_queue);
        _pcm_frames_dropped.store(0);
    }
    _voice_enabled.store(voice && success);
    _capture_enabled.store((wake_word || voice) && success);
    return success;
}

bool XiaozhiApp::enterReadyAudioMode()
{
    const bool enable_wake_word =
        _visible.load() && _network_ready.load() && _transport_connected &&
        _chat_started.load();
    if (setAudioModes(enable_wake_word, false)) {
        return true;
    }

    ESP_UTILS_LOGE("Failed to enable Xiaozhi wake-word detection");
    shutdownAudio();
    setError(ESP_FAIL, "Enable Xiaozhi wake-word detection");
    schedulePrepare(RETRY_INTERVAL_MS);
    return false;
}

bool XiaozhiApp::configureDecoder(
    uint16_t sample_rate,
    uint16_t frame_duration_ms
)
{
    switch (sample_rate) {
    case 8000:
    case 12000:
    case 16000:
    case 24000:
    case 48000:
        break;
    default:
        ESP_UTILS_LOGE(
            "Unsupported Xiaozhi output sample rate: %u",
            static_cast<unsigned int>(sample_rate)
        );
        return false;
    }

    esp_opus_dec_frame_duration_t frame_duration;
    switch (frame_duration_ms) {
    case 5:
        frame_duration = ESP_OPUS_DEC_FRAME_DURATION_5_MS;
        break;
    case 10:
        frame_duration = ESP_OPUS_DEC_FRAME_DURATION_10_MS;
        break;
    case 20:
        frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
        break;
    case 40:
        frame_duration = ESP_OPUS_DEC_FRAME_DURATION_40_MS;
        break;
    case 60:
        frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
        break;
    case 80:
        frame_duration = ESP_OPUS_DEC_FRAME_DURATION_80_MS;
        break;
    case 100:
        frame_duration = ESP_OPUS_DEC_FRAME_DURATION_100_MS;
        break;
    case 120:
        frame_duration = ESP_OPUS_DEC_FRAME_DURATION_120_MS;
        break;
    default:
        ESP_UTILS_LOGE(
            "Unsupported Xiaozhi Opus frame duration: %u ms",
            static_cast<unsigned int>(frame_duration_ms)
        );
        return false;
    }

    if (_opus_decoder && _decoder_sample_rate == sample_rate &&
            _decoder_frame_duration_ms == frame_duration_ms) {
        return true;
    }

    if (_opus_decoder) {
        esp_opus_dec_close(_opus_decoder);
        _opus_decoder = nullptr;
    }
    if (_output_resampler) {
        esp_ae_rate_cvt_close(_output_resampler);
        _output_resampler = nullptr;
    }
    _decoder_sample_rate = 0;
    _decoder_frame_duration_ms = 0;

    esp_opus_dec_cfg_t decoder_config = ESP_OPUS_DEC_CONFIG_DEFAULT();
    decoder_config.sample_rate = sample_rate;
    decoder_config.channel = ESP_AUDIO_MONO;
    decoder_config.frame_duration = frame_duration;
    decoder_config.self_delimited = false;
    auto decoder_ret = esp_opus_dec_open(
        &decoder_config, sizeof(decoder_config), &_opus_decoder
    );
    if (decoder_ret != ESP_AUDIO_ERR_OK || !_opus_decoder) {
        ESP_UTILS_LOGE(
            "Open Xiaozhi Opus decoder failed: %d",
            static_cast<int>(decoder_ret)
        );
        _opus_decoder = nullptr;
        return false;
    }

    if (sample_rate != DEVICE_AUDIO_SAMPLE_RATE) {
        esp_ae_rate_cvt_cfg_t resampler_config = {};
        resampler_config.src_rate = sample_rate;
        resampler_config.dest_rate = DEVICE_AUDIO_SAMPLE_RATE;
        resampler_config.channel = ESP_AUDIO_MONO;
        resampler_config.bits_per_sample = ESP_AUDIO_BIT16;
        resampler_config.complexity = 2;
        resampler_config.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
        auto resampler_ret = esp_ae_rate_cvt_open(
            &resampler_config, &_output_resampler
        );
        if (resampler_ret != ESP_AE_ERR_OK || !_output_resampler) {
            ESP_UTILS_LOGE(
                "Open Xiaozhi output resampler failed: %d",
                static_cast<int>(resampler_ret)
            );
            esp_opus_dec_close(_opus_decoder);
            _opus_decoder = nullptr;
            _output_resampler = nullptr;
            return false;
        }
    }

    _decoder_sample_rate = sample_rate;
    _decoder_frame_duration_ms = frame_duration_ms;
    ESP_UTILS_LOGI(
        "Xiaozhi output configured: %u Hz, %u ms%s",
        static_cast<unsigned int>(sample_rate),
        static_cast<unsigned int>(frame_duration_ms),
        sample_rate == DEVICE_AUDIO_SAMPLE_RATE ? "" : ", resampled to 24000 Hz"
    );
    return true;
}

void XiaozhiApp::finishPreroll(esp_err_t result)
{
    _preroll_result.store(result);
    _preroll_frames_remaining.store(0);
    if (_preroll_active.exchange(false) && _preroll_done) {
        xSemaphoreGive(_preroll_done);
    }
}

esp_err_t XiaozhiApp::sendWakeWordPcm()
{
    if (!_audio_processor || !_audio_processor->isReady() ||
            !_pcm_queue || !_preroll_done) {
        return ESP_ERR_INVALID_STATE;
    }

    std::vector<int16_t> pcm;
    if (!_audio_processor->takeWakeWordPcm(pcm)) {
        ESP_UTILS_LOGW("Wake word PCM cache is empty");
        return ESP_OK;
    }

    constexpr size_t samples_per_frame =
        XiaozhiAudioProcessor::PCM_FRAME_SAMPLES;
    const size_t frame_count =
        (pcm.size() + samples_per_frame - 1) / samples_per_frame;
    if (frame_count == 0 || frame_count > PCM_QUEUE_LENGTH) {
        ESP_UTILS_LOGW(
            "Invalid wake word PCM cache size: %u samples",
            static_cast<unsigned int>(pcm.size())
        );
        return ESP_ERR_INVALID_SIZE;
    }

    (void)xSemaphoreTake(_preroll_done, 0);
    xQueueReset(_pcm_queue);
    _preroll_result.store(ESP_OK);
    _preroll_frames_remaining.store(static_cast<uint32_t>(frame_count));
    _preroll_active.store(true);
    _uplink_ready.store(true);

    for (size_t i = 0; i < frame_count; ++i) {
        PcmFrame frame = {};
        const size_t offset = i * samples_per_frame;
        const size_t remaining = pcm.size() - offset;
        const size_t copy_samples =
            remaining < samples_per_frame ? remaining : samples_per_frame;
        memcpy(
            frame.samples,
            pcm.data() + offset,
            copy_samples * sizeof(int16_t)
        );
        if (xQueueSend(
                _pcm_queue,
                &frame,
                pdMS_TO_TICKS(PREROLL_QUEUE_TIMEOUT_MS)
            ) != pdTRUE) {
            finishPreroll(ESP_ERR_TIMEOUT);
            break;
        }
    }

    if (_preroll_active.load() &&
            xSemaphoreTake(
                _preroll_done,
                pdMS_TO_TICKS(PREROLL_ENCODE_TIMEOUT_MS)
            ) != pdTRUE) {
        finishPreroll(ESP_ERR_TIMEOUT);
    }

    esp_err_t result =
        static_cast<esp_err_t>(_preroll_result.load());
    _preroll_active.store(false);
    _uplink_ready.store(false);
    if (result != ESP_OK) {
        xQueueReset(_pcm_queue);
        return result;
    }

    ESP_UTILS_LOGI(
        "Sent %u wake word Opus preroll packet(s)",
        static_cast<unsigned int>(frame_count)
    );
    return ESP_OK;
}

esp_err_t XiaozhiApp::openAudioChannel()
{
    XiaozhiClient *protocol = _protocol.load();
    if (!protocol) {
        return ESP_ERR_INVALID_STATE;
    }

    ChannelState channel = _channel_state.load();
    if (channel == ChannelState::Open) {
        return ESP_OK;
    }
    if (channel != ChannelState::Closed) {
        return ESP_ERR_INVALID_STATE;
    }

    _channel_state.store(ChannelState::Opening);
    esp_err_t ret = protocol->openAudioChannel();
    if (ret != ESP_OK) {
        resetChannelState();
        return ret;
    }

    _channel_state.store(ChannelState::Open);
    _server_goodbye_generation.store(0);
    return ESP_OK;
}

esp_err_t XiaozhiApp::startAutoStopTurn(const char *wake_word)
{
    esp_err_t ret = openAudioChannel();
    if (ret != ESP_OK) {
        return ret;
    }

    _listen_mode = ListenMode::AutoStop;
    _awaiting_response = false;
    _tts_active = false;
    _tts_stop_pending.store(false);
    _accept_playback.store(false);

    if (wake_word && wake_word[0]) {
        ret = sendWakeWordPcm();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    if (!_audio_tx_mutex ||
            xSemaphoreTake(_audio_tx_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    XiaozhiClient *protocol = _protocol.load();
    if (!protocol || _channel_state.load() != ChannelState::Open) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        if (wake_word && wake_word[0]) {
            ret = protocol->sendWakeWordDetected(wake_word);
        }
        if (ret == ESP_OK) {
            ret = protocol->sendStartListening(
                XiaozhiClient::ListeningMode::AutoStop
            );
        }
    }
    xSemaphoreGive(_audio_tx_mutex);
    if (ret == ESP_OK) {
        ESP_UTILS_LOGI("AUTO listening turn started");
    }
    return ret;
}

bool XiaozhiApp::startListening(ListenMode mode, const char *wake_word)
{
    auto restore_audio = [this]() {
        const bool can_capture =
            _visible.load() && _network_ready.load() && _transport_connected &&
            _chat_started.load() && _state.load() == State::Ready &&
            _audio_processor && _audio_processor->isReady();
        if (can_capture) {
            enterReadyAudioMode();
        } else {
            setAudioModes(false, false);
        }
    };

    const bool auto_stop_mode = mode == ListenMode::AutoStop;
    _accept_playback.store(false);
    _uplink_ready.store(false);
    if (!_network_ready.load() || !_transport_connected ||
            !_chat_started.load()) {
        restore_audio();
        return false;
    }
    if (!ensureAudioReady()) {
        restore_audio();
        schedulePrepare(RETRY_INTERVAL_MS);
        return false;
    }

    XiaozhiClient *protocol = _protocol.load();
    ChannelState channel = _channel_state.load();
    if (!protocol ||
            (channel != ChannelState::Closed &&
             channel != ChannelState::Open)) {
        restore_audio();
        return false;
    }

    _turn_generation.fetch_add(1);
    _pcm_frames_captured.store(0);
    _pcm_frames_dropped.store(0);
    _opus_packets_sent.store(0);
    _pcm_peak.store(0);
    _pcm_mean_abs.store(0);
    _input_pcm_peak.store(0);
    _input_pcm_mean_abs.store(0);
    if (_pcm_queue) {
        xQueueReset(_pcm_queue);
    }
    if (!resetPlaybackBuffer(true) ||
            !waitForPlaybackIdle(PLAYBACK_IDLE_WAIT_MS)) {
        closeChannel(true);
        shutdownAudio();
        setError(ESP_ERR_TIMEOUT, "Reset Xiaozhi playback");
        schedulePrepare(RETRY_INTERVAL_MS);
        restore_audio();
        return false;
    }

    if (_channel_state.load() == ChannelState::Closed) {
        setState(State::Connecting);
    }
    esp_err_t ret = ESP_OK;
    if (auto_stop_mode) {
        ret = startAutoStopTurn(wake_word);
    } else {
        ret = openAudioChannel();
        if (ret == ESP_OK && _audio_tx_mutex &&
                xSemaphoreTake(_audio_tx_mutex, portMAX_DELAY) == pdTRUE) {
            ret = protocol->sendStartListening(
                XiaozhiClient::ListeningMode::ManualStop
            );
            xSemaphoreGive(_audio_tx_mutex);
        } else if (ret == ESP_OK) {
            ret = ESP_ERR_INVALID_STATE;
        }
    }
    if (ret != ESP_OK) {
        closeChannel(true);
        destroyChat();
        setError(ret, "Start Xiaozhi listening");
        schedulePrepare(RETRY_INTERVAL_MS);
        return false;
    }

    _listen_mode = mode;
    _awaiting_response = false;
    _tts_active = false;
    _tts_stop_pending.store(false);
    setState(State::Listening);
    _pcm_frames_captured.store(0);
    _pcm_frames_dropped.store(0);
    _opus_packets_sent.store(0);
    _pcm_peak.store(0);
    _pcm_mean_abs.store(0);
    _input_pcm_peak.store(0);
    _input_pcm_mean_abs.store(0);
    if (_pcm_queue) {
        xQueueReset(_pcm_queue);
    }
    if (!setAudioModes(false, true)) {
        closeChannel(true);
        shutdownAudio();
        setError(ESP_FAIL, "Enable Xiaozhi voice processing");
        schedulePrepare(RETRY_INTERVAL_MS);
        return false;
    }
    _uplink_ready.store(true);
    _uplink_started_tick.store(xTaskGetTickCount());
    ESP_UTILS_LOGI(
        "Listening started (mode=%s, source=%s)",
        auto_stop_mode ? "auto" : "manual",
        wake_word && wake_word[0] ? "wake" : "button"
    );
    return true;
}

void XiaozhiApp::finishListening()
{
    if (_listen_mode != ListenMode::Manual ||
            _channel_state.load() != ChannelState::Open) {
        return;
    }
    esp_err_t ret = stopUplink(true);
    if (ret != ESP_OK) {
        closeChannel(true);
        destroyChat();
        if (_network_ready.load()) {
            setError(ret, "Stop Xiaozhi listening");
            schedulePrepare(RETRY_INTERVAL_MS);
        }
        return;
    }
    _awaiting_response = true;
    setState(State::Processing);
}

esp_err_t XiaozhiApp::stopUplink(bool notify_server)
{
    setAudioModes(false, false);
    if (!_audio_tx_mutex ||
            xSemaphoreTake(_audio_tx_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = ESP_OK;
    if (notify_server) {
        ret = ESP_ERR_INVALID_STATE;
        XiaozhiClient *protocol = _protocol.load();
        if (_channel_state.load() == ChannelState::Open && protocol) {
            ret = protocol->sendStopListening();
        }
    }
    xSemaphoreGive(_audio_tx_mutex);
    return ret;
}

void XiaozhiApp::cancelManualConversation()
{
    closeChannel(true);
    clearConversationUi();
    setState(State::Ready);
    enterReadyAudioMode();
}

void XiaozhiApp::closeChannel(bool abort_speaking)
{
    setAudioModes(false, false);
    _accept_playback.store(false);
    _turn_generation.fetch_add(1);
    resetPlaybackBuffer(false);
    XiaozhiClient *protocol = _protocol.load();
    ChannelState channel = _channel_state.load();
    if (protocol && (channel == ChannelState::Open ||
                   channel == ChannelState::Opening)) {
        _channel_state.store(ChannelState::Closing);
        if (_audio_tx_mutex &&
                xSemaphoreTake(_audio_tx_mutex, portMAX_DELAY) == pdTRUE) {
            if (abort_speaking && (_tts_active ||
                                   _state.load() == State::Speaking)) {
                (void)protocol->sendAbortSpeaking(
                    XiaozhiClient::AbortReason::None
                );
            }
            if (_listen_mode != ListenMode::None) {
                (void)protocol->sendStopListening();
            }
            (void)protocol->closeAudioChannel();
            xSemaphoreGive(_audio_tx_mutex);
        }
    }
    resetChannelState();
}

void XiaozhiApp::resetChannelState()
{
    _uplink_ready.store(false);
    _channel_state.store(ChannelState::Closed);
    _accept_playback.store(false);
    _awaiting_response = false;
    _tts_active = false;
    _tts_stop_pending.store(false);
    _drain_event_posted.store(false);
    _restart_transport_after_playback.store(false);
    _listen_mode = ListenMode::None;
    clearConversationUi();
    resetPlaybackBuffer(false);
}

void XiaozhiApp::handlePlaybackDrained()
{
    _accept_playback.store(false);
    bool ready = false;
    if (_playback_mutex &&
            xSemaphoreTake(
                _playback_mutex, pdMS_TO_TICKS(CONTROL_EVENT_POST_TIMEOUT_MS)
            ) == pdTRUE) {
        ready = playbackDrainReady();
        xSemaphoreGive(_playback_mutex);
    }
    if (!ready) {
        _accept_playback.store(true);
        _drain_event_posted.store(false);
        return;
    }

    _tts_stop_pending.store(false);
    _drain_event_posted.store(false);
    if (_restart_transport_after_playback.exchange(false)) {
        ESP_UTILS_LOGI(
            "Final playback drained; restarting Xiaozhi transport now"
        );
        destroyChat();
        if (_network_ready.load()) {
            setState(State::Connecting);
            schedulePrepare(0);
        } else {
            setState(State::NetworkRequired);
        }
        return;
    }

    if (!_visible.load() || !_network_ready.load() ||
            _channel_state.load() != ChannelState::Open) {
        return;
    }
    if (_listen_mode == ListenMode::AutoStop) {
        if (!resetPlaybackBuffer(true)) {
            closeChannel(false);
            shutdownAudio();
            setError(ESP_FAIL, "Reset Xiaozhi decoder after playback");
            schedulePrepare(RETRY_INTERVAL_MS);
            return;
        }

        _turn_generation.fetch_add(1);
        _pcm_frames_captured.store(0);
        _pcm_frames_dropped.store(0);
        _opus_packets_sent.store(0);
        _pcm_peak.store(0);
        _pcm_mean_abs.store(0);
        _input_pcm_peak.store(0);
        _input_pcm_mean_abs.store(0);
        if (_pcm_queue) {
            xQueueReset(_pcm_queue);
        }

        esp_err_t ret = startAutoStopTurn();
        if (ret != ESP_OK) {
            closeChannel(false);
            destroyChat();
            setError(ret, "Restart Xiaozhi AUTO listening");
            schedulePrepare(RETRY_INTERVAL_MS);
            return;
        }

        _listen_mode = ListenMode::AutoStop;
        _awaiting_response = false;
        setState(State::Listening);
        if (!setAudioModes(false, true)) {
            closeChannel(false);
            shutdownAudio();
            destroyChat();
            setError(ESP_FAIL, "Resume Xiaozhi voice processing");
            schedulePrepare(RETRY_INTERVAL_MS);
            return;
        }
        _uplink_ready.store(true);
        _uplink_started_tick.store(xTaskGetTickCount());
        ESP_UTILS_LOGI("Listening resumed (mode=auto)");
    } else {
        clearConversationUi();
        _listen_mode = ListenMode::None;
        _awaiting_response = false;
        setState(State::Ready);
        enterReadyAudioMode();
    }
}

void XiaozhiApp::checkPlaybackDrained()
{
    if (_drain_event_posted.load() || !playbackDrainReady()) {
        return;
    }
    if (!_drain_event_posted.exchange(true)) {
        ControlEvent event = {};
        event.type = ControlEventType::PlaybackDrained;
        event.chat_generation = _chat_generation.load();
        event.turn_generation = _turn_generation.load();
        if (!postControlEvent(event)) {
            _drain_event_posted.store(false);
        }
    }
}

bool XiaozhiApp::playbackDrainReady() const
{
    if (!_tts_stop_pending.load() || _playback_busy.load() ||
            !_playback_queue || uxQueueMessagesWaiting(_playback_queue) != 0) {
        return false;
    }
    TickType_t now = xTaskGetTickCount();
    TickType_t delay = pdMS_TO_TICKS(PLAYBACK_DRAIN_MS);
    return now - _tts_stop_tick.load() >= delay &&
           now - _last_audio_packet_tick.load() >= delay &&
           now - _last_audio_write_tick.load() >= delay;
}

bool XiaozhiApp::resetPlaybackBuffer(bool reset_decoder)
{
    if (!_playback_mutex ||
            xSemaphoreTake(
                _playback_mutex, pdMS_TO_TICKS(PLAYBACK_IDLE_WAIT_MS)
            ) != pdTRUE) {
        return false;
    }

    if (_playback_queue) {
        xQueueReset(_playback_queue);
    }
    bool ok = true;
    if (reset_decoder) {
        ok = _opus_decoder &&
             esp_opus_dec_reset(_opus_decoder) == ESP_AUDIO_ERR_OK;
        if (ok && _output_resampler) {
            ok = esp_ae_rate_cvt_reset(_output_resampler) == ESP_AE_ERR_OK;
        }
    }
    xSemaphoreGive(_playback_mutex);
    return ok;
}

bool XiaozhiApp::waitForPlaybackIdle(uint32_t timeout_ms) const
{
    TickType_t started = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    while (_playback_busy.load()) {
        if (xTaskGetTickCount() - started >= timeout) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return true;
}

void XiaozhiApp::inputAudio()
{
    const size_t input_bytes =
        INPUT_READ_FRAMES * DEVICE_TDM_CHANNELS * sizeof(int16_t);
    auto *device_tdm = static_cast<int16_t *>(malloc(input_bytes));
    auto *device_planar = static_cast<int16_t *>(
        malloc(INPUT_READ_FRAMES * AFE_INPUT_CHANNELS * sizeof(int16_t))
    );
    auto *chat_planar = static_cast<int16_t *>(
        malloc(
            _input_resampler_max_samples * AFE_INPUT_CHANNELS * sizeof(int16_t)
        )
    );
    auto *afe_interleaved = static_cast<int16_t *>(
        malloc(
            _input_resampler_max_samples * AFE_INPUT_CHANNELS * sizeof(int16_t)
        )
    );
    uint32_t read_failures = 0;
    uint32_t resample_failures = 0;
    const bool input_resamplers_ready =
        _input_resamplers[0] && _input_resamplers[1] && _input_resamplers[2];
    if (!device_tdm || !device_planar || !chat_planar || !afe_interleaved ||
            !input_resamplers_ready) {
        free(device_tdm);
        free(device_planar);
        free(chat_planar);
        free(afe_interleaved);
        ControlEvent event = {};
        event.type = ControlEventType::AudioError;
        event.error = ESP_ERR_NO_MEM;
        copyString(
            event.detail, sizeof(event.detail), "Allocate audio input buffer"
        );
        postControlEvent(event);
        _input_task_running.store(false);
        vTaskDelete(nullptr);
        return;
    }
    while (_audio_tasks_running.load()) {
        if (_input_resampler_reset_requested.exchange(false)) {
            for (auto input_resampler : _input_resamplers) {
                (void)esp_ae_rate_cvt_reset(input_resampler);
            }
        }
        if (!_capture_enabled.load()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        size_t bytes_read = 0;
        esp_err_t ret = bsp_extra_i2s_read(
            device_tdm, input_bytes, &bytes_read, 100
        );
        if (ret == ESP_OK && bytes_read > 0 && _audio_processor) {
            read_failures = 0;
            const uint32_t input_frames = static_cast<uint32_t>(
                bytes_read / (DEVICE_TDM_CHANNELS * sizeof(int16_t))
            );
            uint32_t peak = 0;
            uint64_t absolute_sum = 0;
            for (uint32_t frame = 0; frame < input_frames; ++frame) {
                const int16_t mic1 =
                    device_tdm[frame * DEVICE_TDM_CHANNELS + DEVICE_MIC1_SLOT];
                const int16_t mic2 =
                    device_tdm[frame * DEVICE_TDM_CHANNELS + DEVICE_MIC2_SLOT];
                const int16_t echo =
                    device_tdm[frame * DEVICE_TDM_CHANNELS + DEVICE_ECHO_SLOT];
                device_planar[frame] = mic1;
                device_planar[INPUT_READ_FRAMES + frame] = mic2;
                device_planar[INPUT_READ_FRAMES * 2 + frame] = echo;
                const int16_t microphone_samples[2] = {mic1, mic2};
                for (int16_t sample : microphone_samples) {
                    int32_t value = sample;
                    uint32_t magnitude = static_cast<uint32_t>(
                        value < 0 ? -value : value
                    );
                    peak = magnitude > peak ? magnitude : peak;
                    absolute_sum += magnitude;
                }
            }
            _input_pcm_peak.store(peak);
            _input_pcm_mean_abs.store(
                input_frames ?
                    static_cast<uint32_t>(absolute_sum / (input_frames * 2)) : 0
            );

            int resample_ret = ESP_AE_ERR_OK;
            uint32_t output_samples = 0;
            bool channels_match = input_frames > 0;
            for (size_t channel = 0;
                    channel < AFE_INPUT_CHANNELS && channels_match; ++channel) {
                uint32_t channel_output_samples = _input_resampler_max_samples;
                resample_ret = esp_ae_rate_cvt_process(
                    _input_resamplers[channel],
                    reinterpret_cast<esp_ae_sample_t>(
                        device_planar + channel * INPUT_READ_FRAMES
                    ),
                    input_frames,
                    reinterpret_cast<esp_ae_sample_t>(
                        chat_planar +
                        channel * _input_resampler_max_samples
                    ),
                    &channel_output_samples
                );
                if (resample_ret != ESP_AE_ERR_OK ||
                        channel_output_samples == 0 ||
                        channel_output_samples > _input_resampler_max_samples ||
                        (output_samples != 0 &&
                         channel_output_samples != output_samples)) {
                    channels_match = false;
                    break;
                }
                output_samples = channel_output_samples;
            }
            if (channels_match) {
                for (uint32_t frame = 0; frame < output_samples; ++frame) {
                    for (size_t channel = 0;
                            channel < AFE_INPUT_CHANNELS; ++channel) {
                        afe_interleaved[frame * AFE_INPUT_CHANNELS + channel] =
                            chat_planar[
                                channel * _input_resampler_max_samples + frame
                            ];
                    }
                }
                resample_failures = 0;
                _audio_processor->feed(afe_interleaved, output_samples);
            } else {
                ++resample_failures;
                if (resample_failures == 1 ||
                        (resample_failures % 50) == 0) {
                    ESP_UTILS_LOGW(
                        "Xiaozhi input resample failed: %d "
                        "(in=%lu, out=%lu, count=%lu)",
                        static_cast<int>(resample_ret),
                        static_cast<unsigned long>(input_frames),
                        static_cast<unsigned long>(output_samples),
                        static_cast<unsigned long>(resample_failures)
                    );
                }
            }
        } else if (ret != ESP_OK) {
            ++read_failures;
            if (read_failures == 1 || (read_failures % 50) == 0) {
                ESP_UTILS_LOGW(
                    "Xiaozhi audio input read failed: 0x%x (count=%lu)",
                    static_cast<unsigned int>(ret),
                    static_cast<unsigned long>(read_failures)
                );
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    free(afe_interleaved);
    free(chat_planar);
    free(device_planar);
    free(device_tdm);
    _input_task_running.store(false);
    vTaskDelete(nullptr);
}

void XiaozhiApp::encodeAudio()
{
    auto *encoded = static_cast<uint8_t *>(malloc(_opus_output_size));
    PcmFrame frame = {};
    uint32_t encode_failures = 0;
    while (encoded && _audio_tasks_running.load()) {
        bool preroll = _preroll_active.load();
        if ((!_voice_enabled.load() && !preroll) ||
                !_uplink_ready.load()) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (!_pcm_queue ||
                xQueueReceive(_pcm_queue, &frame, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        preroll = _preroll_active.load();
        if ((!_voice_enabled.load() && !preroll) ||
                !_uplink_ready.load()) {
            continue;
        }
        esp_audio_enc_in_frame_t input = {};
        input.buffer = reinterpret_cast<uint8_t *>(frame.samples);
        input.len = sizeof(frame.samples);
        esp_audio_enc_out_frame_t output = {};
        output.buffer = encoded;
        output.len = _opus_output_size;
        if (esp_opus_enc_process(_opus_encoder, &input, &output) !=
                ESP_AUDIO_ERR_OK || output.encoded_bytes <= 0) {
            if (preroll) {
                finishPreroll(ESP_FAIL);
                continue;
            }
            ++encode_failures;
            if (encode_failures == 1 || (encode_failures % 50) == 0) {
                ESP_UTILS_LOGW(
                    "OPUS encode failed or returned no data (count=%lu)",
                    static_cast<unsigned long>(encode_failures)
                );
            }
            continue;
        }
        esp_err_t send_ret = ESP_OK;
        bool attempted_send = false;
        if (_audio_tx_mutex &&
                xSemaphoreTake(_audio_tx_mutex, portMAX_DELAY) == pdTRUE) {
            XiaozhiClient *protocol = _protocol.load();
            if ((_voice_enabled.load() || _preroll_active.load()) &&
                    _uplink_ready.load() &&
                    _channel_state.load() == ChannelState::Open && protocol) {
                attempted_send = true;
                send_ret = protocol->sendAudio(
                    encoded,
                    static_cast<size_t>(output.encoded_bytes),
                    0
                );
            }
            xSemaphoreGive(_audio_tx_mutex);
        }
        if (attempted_send && send_ret == ESP_OK) {
            if (preroll) {
                uint32_t remaining = _preroll_frames_remaining.load();
                while (remaining > 0 &&
                        !_preroll_frames_remaining.compare_exchange_weak(
                            remaining, remaining - 1
                        )) {
                }
                if (remaining == 1) {
                    finishPreroll(ESP_OK);
                }
                continue;
            }
            uint32_t sent = _opus_packets_sent.fetch_add(1) + 1;
            if (sent == 1) {
                _uplink_started_tick.store(0);
                ESP_UTILS_LOGI(
                    "Sent Opus packet #%lu (%d bytes)",
                    static_cast<unsigned long>(sent), output.encoded_bytes
                );
            } else if ((sent % 500) == 0) {
                ESP_UTILS_LOGD(
                    "Sent Opus packet #%lu",
                    static_cast<unsigned long>(sent)
                );
            }
        } else if (preroll) {
            finishPreroll(
                attempted_send ? send_ret : ESP_ERR_INVALID_STATE
            );
        } else if (attempted_send) {
            _uplink_ready.store(false);
            _voice_enabled.store(false);
            _capture_enabled.store(false);
            ControlEvent event = {};
            event.type = ControlEventType::ChatError;
            event.error = send_ret;
            event.chat_generation = _chat_generation.load();
            event.turn_generation = _turn_generation.load();
            copyString(
                event.detail, sizeof(event.detail), "Send Xiaozhi audio data"
            );
            postControlEvent(event);
        }
    }
    if (!encoded) {
        ControlEvent event = {};
        event.type = ControlEventType::AudioError;
        event.error = ESP_ERR_NO_MEM;
        copyString(event.detail, sizeof(event.detail), "Allocate OPUS output buffer");
        postControlEvent(event);
    }
    free(encoded);
    _encoder_task_running.store(false);
    vTaskDeleteWithCaps(nullptr);
}

void XiaozhiApp::playAudio()
{
    auto *server_pcm = static_cast<int16_t *>(malloc(MAX_SERVER_PCM_BYTES));
    auto *device_pcm = static_cast<int16_t *>(
        malloc(MAX_DEVICE_PCM_SAMPLES * sizeof(int16_t))
    );
    auto *stereo = static_cast<int16_t *>(
        malloc(MAX_DEVICE_PCM_SAMPLES * 2 * sizeof(int16_t))
    );
    if (!server_pcm || !device_pcm || !stereo) {
        ControlEvent event = {};
        event.type = ControlEventType::AudioError;
        event.error = ESP_ERR_NO_MEM;
        copyString(event.detail, sizeof(event.detail), "Allocate playback buffers");
        postControlEvent(event);
    }

    uint32_t decode_failures = 0;
    AudioPacket packet = {};
    while (server_pcm && device_pcm && stereo &&
            _audio_tasks_running.load()) {
        if (!_playback_queue ||
                xQueuePeek(_playback_queue, &packet, pdMS_TO_TICKS(50)) != pdTRUE) {
            checkPlaybackDrained();
            continue;
        }
        if (_state.load() != State::Speaking) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        bool received = false;
        bool decoded = false;
        bool output_error = false;
        size_t playback_samples = 0;
        const int16_t *playback_pcm = nullptr;
        if (_playback_mutex &&
                xSemaphoreTake(_playback_mutex, portMAX_DELAY) == pdTRUE) {
            received =
                xQueueReceive(_playback_queue, &packet, 0) == pdTRUE;
            if (received) {
                _playback_busy.store(true);
            }
            if (received && _accept_playback.load() &&
                    packet.turn_generation == _turn_generation.load()) {
                if (!configureDecoder(
                        packet.sample_rate, packet.frame_duration_ms
                    )) {
                    output_error = true;
                } else {
                    esp_audio_dec_in_raw_t input = {};
                    input.buffer = packet.data;
                    input.len = packet.size;
                    input.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;
                    esp_audio_dec_out_frame_t output = {};
                    output.buffer = reinterpret_cast<uint8_t *>(server_pcm);
                    output.len = MAX_SERVER_PCM_BYTES;
                    esp_audio_dec_info_t info = {};
                    bool opus_ok =
                        esp_opus_dec_decode(
                            _opus_decoder, &input, &output, &info
                        ) == ESP_AUDIO_ERR_OK &&
                        output.decoded_size > 0 &&
                        output.decoded_size <= MAX_SERVER_PCM_BYTES;
                    if (opus_ok) {
                        uint32_t server_samples =
                            output.decoded_size / sizeof(int16_t);
                        if (_output_resampler) {
                            uint32_t maximum_samples = 0;
                            if (esp_ae_rate_cvt_get_max_out_sample_num(
                                    _output_resampler,
                                    server_samples,
                                    &maximum_samples
                                ) != ESP_AE_ERR_OK ||
                                    maximum_samples == 0 ||
                                    maximum_samples > MAX_DEVICE_PCM_SAMPLES) {
                                output_error = true;
                            } else {
                                uint32_t actual_samples = maximum_samples;
                                if (esp_ae_rate_cvt_process(
                                        _output_resampler,
                                        reinterpret_cast<esp_ae_sample_t>(
                                            server_pcm
                                        ),
                                        server_samples,
                                        reinterpret_cast<esp_ae_sample_t>(
                                            device_pcm
                                        ),
                                        &actual_samples
                                    ) == ESP_AE_ERR_OK &&
                                        actual_samples > 0 &&
                                        actual_samples <=
                                            MAX_DEVICE_PCM_SAMPLES) {
                                    playback_pcm = device_pcm;
                                    playback_samples = actual_samples;
                                    decoded = true;
                                } else {
                                    output_error = true;
                                }
                            }
                        } else if (server_samples <= MAX_DEVICE_PCM_SAMPLES) {
                            playback_pcm = server_pcm;
                            playback_samples = server_samples;
                            decoded = true;
                        } else {
                            output_error = true;
                        }
                    }
                }
            }
            xSemaphoreGive(_playback_mutex);
        }
        if (!received) {
            checkPlaybackDrained();
            continue;
        }

        if (output_error) {
            _accept_playback.store(false);
            ControlEvent event = {};
            event.type = ControlEventType::AudioError;
            event.error = ESP_FAIL;
            copyString(
                event.detail,
                sizeof(event.detail),
                "Configure Xiaozhi output audio"
            );
            postControlEvent(event);
        } else if (!decoded && _accept_playback.load() &&
                packet.turn_generation == _turn_generation.load()) {
            ++decode_failures;
            if (decode_failures == 1 || (decode_failures % 20) == 0) {
                ESP_UTILS_LOGW(
                    "Xiaozhi Opus decode failed (count=%lu)",
                    static_cast<unsigned long>(decode_failures)
                );
            }
        } else if (decoded && _accept_playback.load() &&
                packet.turn_generation == _turn_generation.load()) {
            for (size_t i = 0; i < playback_samples; ++i) {
                stereo[i * 2] = playback_pcm[i];
                stereo[i * 2 + 1] = playback_pcm[i];
            }
            size_t written = 0;
            size_t write_size =
                playback_samples * 2 * sizeof(int16_t);
            esp_err_t write_ret = bsp_extra_i2s_write(
                stereo, write_size, &written, 200
            );
            if (write_ret == ESP_OK && written == write_size) {
                _last_audio_write_tick.store(xTaskGetTickCount());
            } else {
                _accept_playback.store(false);
                ControlEvent event = {};
                event.type = ControlEventType::AudioError;
                event.error = write_ret == ESP_OK ? ESP_FAIL : write_ret;
                copyString(
                    event.detail, sizeof(event.detail), "Write Xiaozhi audio"
                );
                postControlEvent(event);
            }
        }
        _playback_busy.store(false);
        checkPlaybackDrained();
    }
    free(server_pcm);
    free(device_pcm);
    free(stereo);
    _playback_busy.store(false);
    _playback_task_running.store(false);
    vTaskDeleteWithCaps(nullptr);
}

const char *XiaozhiApp::stateText(State state)
{
    switch (state) {
    case State::NetworkRequired:
        return xiaozhiUiText(XiaozhiUiText::NetworkRequired);
    case State::Preparing:
        return xiaozhiUiText(XiaozhiUiText::Preparing);
    case State::ActivationRequired:
        return xiaozhiUiText(XiaozhiUiText::ActivationRequired);
    case State::Connecting:
        return xiaozhiUiText(XiaozhiUiText::Connecting);
    case State::Ready:
        return xiaozhiUiText(XiaozhiUiText::Ready);
    case State::Listening:
        return xiaozhiUiText(XiaozhiUiText::Listening);
    case State::Processing:
        return xiaozhiUiText(XiaozhiUiText::Processing);
    case State::Speaking:
        return xiaozhiUiText(XiaozhiUiText::Speaking);
    case State::Error:
        return xiaozhiUiText(XiaozhiUiText::Error);
    }
    return "";
}

bool XiaozhiApp::tickReached(TickType_t now, TickType_t target)
{
    return static_cast<int32_t>(now - target) >= 0;
}

void XiaozhiApp::controlTaskEntry(void *arg)
{
    static_cast<XiaozhiApp *>(arg)->controlLoop();
}

void XiaozhiApp::inputTaskEntry(void *arg)
{
    static_cast<XiaozhiApp *>(arg)->inputAudio();
}

void XiaozhiApp::encoderTaskEntry(void *arg)
{
    static_cast<XiaozhiApp *>(arg)->encodeAudio();
}

void XiaozhiApp::playbackTaskEntry(void *arg)
{
    static_cast<XiaozhiApp *>(arg)->playAudio();
}

void XiaozhiApp::ipEventHandler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)event_base;
    (void)event_id;
    (void)event_data;
    auto *instance = static_cast<XiaozhiApp *>(arg);
    if (!instance) {
        return;
    }
    ScopedOptionalCounter async_user(&instance->_async_post_users);
    if (!instance->_shutdown.load()) {
        instance->updateNetworkState();
    }
}

void XiaozhiApp::uiTimerCallback(lv_timer_t *timer)
{
    auto *instance = static_cast<XiaozhiApp *>(lv_timer_get_user_data(timer));
    if (instance) {
        instance->refreshUi();
    }
}

void XiaozhiApp::uiActionCallback(void *context)
{
    auto *instance = static_cast<XiaozhiApp *>(context);
    if (!instance) {
        return;
    }
    ControlEvent event = {};
    event.type = ControlEventType::ActionPressed;
    instance->postControlEvent(event);
}

} // namespace esp_brookesia::apps
