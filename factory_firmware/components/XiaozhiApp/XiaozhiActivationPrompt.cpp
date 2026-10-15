/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "XiaozhiActivationPrompt.hpp"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "decoder/impl/esp_opus_dec.h"
#include "third_party/xiaozhi_esp32/ogg_demuxer.h"

#ifdef ESP_UTILS_LOG_TAG
#undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:XiaozhiPrompt"
#include "esp_lib_utils.h"

namespace esp_brookesia::apps {

namespace {

constexpr uint32_t SAMPLE_RATE = 16000;
constexpr uint32_t FRAME_DURATION_MS = 60;
constexpr size_t PCM_MONO_BUFFER_SIZE =
    SAMPLE_RATE * FRAME_DURATION_MS / 1000 * sizeof(int16_t);
constexpr size_t PCM_STEREO_BUFFER_SIZE = PCM_MONO_BUFFER_SIZE * 2;
constexpr size_t MAX_OGG_FILE_SIZE = 64 * 1024;
constexpr char PROMPT_DIRECTORY[] = BSP_SPIFFS_MOUNT_POINT "/xiaozhi";

} // namespace

XiaozhiActivationPrompt::XiaozhiActivationPrompt()
{
    _mutex = xSemaphoreCreateMutex();
}

XiaozhiActivationPrompt::~XiaozhiActivationPrompt()
{
    if (!stopAndWait(3000)) {
        ESP_UTILS_LOGE("Activation prompt task did not stop in time; waiting");
        while (_running.load() || !releaseAudioSession()) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (_mutex) {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
}

bool XiaozhiActivationPrompt::start(const char *activation_code, bool force)
{
    if (!activation_code || !activation_code[0] || !_mutex) {
        return false;
    }

    if (xSemaphoreTake(_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    if (_running.load()) {
        xSemaphoreGive(_mutex);
        return false;
    }
    if (!releaseAudioSession()) {
        xSemaphoreGive(_mutex);
        return false;
    }
    if (!force && strcmp(_announced_code, activation_code) == 0) {
        xSemaphoreGive(_mutex);
        return true;
    }
    snprintf(_pending_code, sizeof(_pending_code), "%s", activation_code);
    _pending_success = false;
    _cancel_requested.store(false);
    _last_playback_completed.store(false);
    _running.store(true);
    xSemaphoreGive(_mutex);

    BaseType_t result = xTaskCreatePinnedToCore(
                            taskEntry,
                            "xiaozhi_activation",
                            TASK_STACK_SIZE,
                            this,
                            5,
                            &_task,
                            0
                        );
    if (result != pdPASS) {
        if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
            _task = nullptr;
            xSemaphoreGive(_mutex);
        }
        _running.store(false);
        ESP_UTILS_LOGE("Failed to create activation prompt task");
        return false;
    }
    return true;
}

bool XiaozhiActivationPrompt::playSuccess()
{
    if (!_mutex || xSemaphoreTake(_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    if (_running.load()) {
        xSemaphoreGive(_mutex);
        return false;
    }
    if (!releaseAudioSession()) {
        xSemaphoreGive(_mutex);
        return false;
    }
    _pending_code[0] = '\0';
    _pending_success = true;
    _cancel_requested.store(false);
    _last_playback_completed.store(false);
    _running.store(true);
    xSemaphoreGive(_mutex);

    BaseType_t result = xTaskCreatePinnedToCore(
                            taskEntry,
                            "xiaozhi_success",
                            TASK_STACK_SIZE,
                            this,
                            5,
                            &_task,
                            0
                        );
    if (result != pdPASS) {
        if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
            _task = nullptr;
            _pending_success = false;
            xSemaphoreGive(_mutex);
        }
        _running.store(false);
        ESP_UTILS_LOGE("Failed to create success prompt task");
        return false;
    }
    return true;
}

void XiaozhiActivationPrompt::cancel()
{
    if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        _cancel_requested.store(true);
        xSemaphoreGive(_mutex);
    } else {
        _cancel_requested.store(true);
    }
}

bool XiaozhiActivationPrompt::waitUntilIdle(uint32_t timeout_ms) const
{
    TickType_t started = xTaskGetTickCount();
    while (_running.load() &&
           (xTaskGetTickCount() - started) < pdMS_TO_TICKS(timeout_ms)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return !_running.load();
}

bool XiaozhiActivationPrompt::stopAndWait(uint32_t timeout_ms)
{
    cancel();
    return waitUntilIdle(timeout_ms) && releaseAudioSession();
}

bool XiaozhiActivationPrompt::isRunning() const
{
    return _running.load();
}

bool XiaozhiActivationPrompt::lastPlaybackCompleted() const
{
    return _last_playback_completed.load();
}

bool XiaozhiActivationPrompt::shouldContinue() const
{
    return !_cancel_requested.load();
}

bool XiaozhiActivationPrompt::releaseAudioSession()
{
    if (_codec_claimed.load()) {
        const esp_err_t stop_result = bsp_extra_codec_dev_stop();
        if (stop_result != ESP_OK) {
            ESP_UTILS_LOGE("Prompt codec shutdown failed: %s", esp_err_to_name(stop_result));
            return false;
        }
        _codec_claimed.store(false);
    }
    if (_audio_session_acquired.load()) {
        const esp_err_t release_result =
            bsp_extra_audio_session_release(BSP_EXTRA_AUDIO_OWNER_XIAOZHI);
        if (release_result != ESP_OK) {
            ESP_UTILS_LOGE("Prompt audio-session release failed: %s",
                           esp_err_to_name(release_result));
            return false;
        }
        _audio_session_acquired.store(false);
    }
    return true;
}

void XiaozhiActivationPrompt::run()
{
    char activation_code[CODE_CAPACITY] = {};
    bool play_success = false;
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        snprintf(activation_code, sizeof(activation_code), "%s", _pending_code);
        play_success = _pending_success;
        xSemaphoreGive(_mutex);
    }

    void *decoder = nullptr;
    auto *mono_pcm = static_cast<uint8_t *>(malloc(PCM_MONO_BUFFER_SIZE));
    auto *stereo_pcm = static_cast<int16_t *>(malloc(PCM_STEREO_BUFFER_SIZE));
    auto *demuxer = new (std::nothrow) OggDemuxer();
    bool completed = false;

    esp_opus_dec_cfg_t decoder_config = ESP_OPUS_DEC_CONFIG_DEFAULT();
    decoder_config.sample_rate = SAMPLE_RATE;
    decoder_config.channel = ESP_AUDIO_MONO;
    decoder_config.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
    esp_err_t codec_result = ESP_ERR_INVALID_STATE;

    if ((!play_success && !activation_code[0]) || !shouldContinue()) {
        goto cleanup;
    }

    codec_result = bsp_extra_audio_session_acquire(BSP_EXTRA_AUDIO_OWNER_XIAOZHI);
    if (codec_result != ESP_OK) {
        ESP_UTILS_LOGW(
            "Activation prompt audio is busy (%s)",
            bsp_extra_audio_owner_name(bsp_extra_audio_session_get_owner())
        );
        goto cleanup;
    }
    _audio_session_acquired.store(true);
    codec_result = bsp_extra_codec_set_fs(
                       SAMPLE_RATE, 16, I2S_SLOT_MODE_STEREO
                   );
    if (codec_result == ESP_OK) {
        _codec_claimed.store(true);
    }
    if (codec_result == ESP_OK && shouldContinue()) {
        bsp_extra_codec_mute_set(false);
    }
    if (!shouldContinue()) {
        goto cleanup;
    }
    if (codec_result != ESP_OK || !mono_pcm || !stereo_pcm || !demuxer ||
            esp_opus_dec_open(&decoder_config, sizeof(decoder_config), &decoder) !=
                ESP_AUDIO_ERR_OK ||
            !decoder) {
        ESP_UTILS_LOGE("Failed to prepare activation prompt audio");
        goto cleanup;
    }

    {
        auto play_file = [&](const char *path) -> bool {
            if (!shouldContinue()) {
                return false;
            }

            FILE *file = fopen(path, "rb");
            if (!file) {
                ESP_UTILS_LOGE("Unable to open prompt: %s", path);
                return false;
            }
            if (fseek(file, 0, SEEK_END) != 0) {
                fclose(file);
                return false;
            }
            long file_size = ftell(file);
            if (file_size <= 0 || file_size > static_cast<long>(MAX_OGG_FILE_SIZE) ||
                    fseek(file, 0, SEEK_SET) != 0) {
                fclose(file);
                ESP_UTILS_LOGE("Invalid prompt size: %s", path);
                return false;
            }

            auto *ogg_data = static_cast<uint8_t *>(malloc(static_cast<size_t>(file_size)));
            if (!ogg_data) {
                fclose(file);
                return false;
            }
            size_t bytes_read = fread(ogg_data, 1, static_cast<size_t>(file_size), file);
            fclose(file);
            if (bytes_read != static_cast<size_t>(file_size)) {
                free(ogg_data);
                return false;
            }

            esp_opus_dec_reset(decoder);
            demuxer->Reset();
            bool decode_ok = true;
            size_t decoded_packets = 0;
            demuxer->OnDemuxerFinished(
                [&](const uint8_t *data, int sample_rate, size_t size) {
                    if (!decode_ok || !shouldContinue() ||
                            sample_rate != static_cast<int>(SAMPLE_RATE)) {
                        decode_ok = false;
                        return;
                    }

                    esp_audio_dec_in_raw_t input = {};
                    input.buffer = const_cast<uint8_t *>(data);
                    input.len = size;
                    input.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;
                    esp_audio_dec_out_frame_t output = {};
                    output.buffer = mono_pcm;
                    output.len = PCM_MONO_BUFFER_SIZE;
                    esp_audio_dec_info_t info = {};
                    if (esp_opus_dec_decode(decoder, &input, &output, &info) !=
                            ESP_AUDIO_ERR_OK ||
                            output.decoded_size == 0 ||
                            output.decoded_size > PCM_MONO_BUFFER_SIZE) {
                        decode_ok = false;
                        return;
                    }

                    size_t sample_count = output.decoded_size / sizeof(int16_t);
                    auto *mono_samples = reinterpret_cast<int16_t *>(mono_pcm);
                    for (size_t i = 0; i < sample_count; ++i) {
                        stereo_pcm[i * 2] = mono_samples[i];
                        stereo_pcm[i * 2 + 1] = mono_samples[i];
                    }

                    size_t bytes_written = 0;
                    size_t stereo_size = sample_count * 2 * sizeof(int16_t);
                    if (bsp_extra_i2s_write(
                            stereo_pcm,
                            stereo_size,
                            &bytes_written,
                            200
                        ) != ESP_OK || bytes_written != stereo_size) {
                        decode_ok = false;
                        return;
                    }
                    ++decoded_packets;
                }
            );
            size_t processed = demuxer->Process(
                                   ogg_data,
                                   static_cast<size_t>(file_size)
                               );
            free(ogg_data);
            return decode_ok && decoded_packets > 0 &&
                   processed == static_cast<size_t>(file_size) && shouldContinue();
        };

        char path[96] = {};
        if (play_success) {
            snprintf(path, sizeof(path), "%s/success.ogg", PROMPT_DIRECTORY);
            completed = play_file(path);
        } else {
            snprintf(path, sizeof(path), "%s/activation.ogg", PROMPT_DIRECTORY);
            completed = play_file(path);
            for (const char *digit = activation_code;
                    completed && *digit && shouldContinue();
                    ++digit) {
                if (*digit < '0' || *digit > '9') {
                    continue;
                }
                snprintf(path, sizeof(path), "%s/%c.ogg", PROMPT_DIRECTORY, *digit);
                completed = play_file(path);
            }
        }
    }

cleanup:
    if (decoder) {
        esp_opus_dec_close(decoder);
    }
    delete demuxer;
    free(stereo_pcm);
    free(mono_pcm);

    if (_codec_claimed.load() && codec_result == ESP_OK) {
        bsp_extra_codec_mute_set(true);
    }
    if (!releaseAudioSession()) {
        ESP_UTILS_LOGE("Activation prompt retained the audio session after cleanup failure");
    }

    bool playback_completed = completed && shouldContinue();
    _last_playback_completed.store(playback_completed);
    if (!play_success && playback_completed && _mutex &&
            xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        snprintf(_announced_code, sizeof(_announced_code), "%s", activation_code);
        xSemaphoreGive(_mutex);
    }

    ESP_UTILS_LOGI(
        "Prompt finished (kind=%s, complete=%d, min_stack_free=%u)",
        play_success ? "success" : "activation",
        playback_completed,
        static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr))
    );
    if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        _task = nullptr;
        _pending_success = false;
        xSemaphoreGive(_mutex);
    } else {
        _task = nullptr;
    }
    _running.store(false);
    vTaskDelete(nullptr);
}

void XiaozhiActivationPrompt::taskEntry(void *arg)
{
    static_cast<XiaozhiActivationPrompt *>(arg)->run();
}

} // namespace esp_brookesia::apps
