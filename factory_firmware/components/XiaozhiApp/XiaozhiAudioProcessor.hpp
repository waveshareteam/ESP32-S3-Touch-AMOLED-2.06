/*
 * Derived from 78/xiaozhi-esp32 (MIT).
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "esp_afe_sr_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "model_path.h"

namespace esp_brookesia::apps {

/**
 * Dual-microphone WakeNet front end with an analog playback reference.
 *
 * Input is 16 kHz signed 16-bit interleaved MMR PCM: MIC1, MIC2, then the
 * ES8311 playback reference captured by ES7210 MIC3. The PCM callback receives
 * the AFE's single-channel 60 ms output frame.
 */
class XiaozhiAudioProcessor {
public:
    static constexpr uint32_t SAMPLE_RATE = 16000;
    static constexpr size_t INPUT_CHANNELS = 3;
    static constexpr size_t PCM_FRAME_SAMPLES = 960;

    using WakeWordCallback = std::function<void(const std::string &wake_word)>;
    using PcmFrameCallback =
        std::function<void(const int16_t *pcm, size_t sample_count)>;
    using VadCallback = std::function<void(bool speaking)>;

    XiaozhiAudioProcessor();
    ~XiaozhiAudioProcessor();

    XiaozhiAudioProcessor(const XiaozhiAudioProcessor &) = delete;
    XiaozhiAudioProcessor &operator=(const XiaozhiAudioProcessor &) = delete;

    bool initialize();
    bool shutdown();

    /** Feed frame_count interleaved MMR samples into the AFE. */
    bool feed(const int16_t *interleaved_pcm, size_t frame_count);

    bool enableWakeWord(bool enable);
    bool enableVoiceProcessing(bool enable);

    bool isReady() const;

    /** Number of frames per channel preferred by one AFE feed call. */
    size_t getFeedFrames() const;

    /** Move the most recent two seconds of WakeNet output into pcm. */
    bool takeWakeWordPcm(std::vector<int16_t> &pcm);

    void setWakeWordCallback(WakeWordCallback callback);
    void setPcmFrameCallback(PcmFrameCallback callback);
    void setVadCallback(VadCallback callback);

private:
    static constexpr EventBits_t EVENT_WAKE_WORD_ENABLED = 1U << 0;
    static constexpr EventBits_t EVENT_VOICE_ENABLED = 1U << 1;
    static constexpr EventBits_t EVENT_SHUTDOWN = 1U << 2;
    static constexpr EventBits_t EVENT_TASK_EXITED = 1U << 3;
    static constexpr EventBits_t EVENT_ACTIVE =
        EVENT_WAKE_WORD_ENABLED | EVENT_VOICE_ENABLED;

    static constexpr uint32_t FETCH_TIMEOUT_MS = 250;
    static constexpr uint32_t PROCESS_TASK_STACK_SIZE = 8 * 1024;
    static constexpr UBaseType_t PROCESS_TASK_PRIORITY = 3;
    static constexpr size_t WAKE_WORD_CACHE_SAMPLES = SAMPLE_RATE * 2;

    mutable SemaphoreHandle_t lifecycle_mutex_ = nullptr;
    mutable SemaphoreHandle_t afe_mutex_ = nullptr;
    mutable SemaphoreHandle_t input_mutex_ = nullptr;
    mutable SemaphoreHandle_t output_mutex_ = nullptr;
    mutable SemaphoreHandle_t callback_mutex_ = nullptr;

    EventGroupHandle_t event_group_ = nullptr;
    TaskHandle_t processing_task_ = nullptr;

    srmodel_list_t *models_ = nullptr;
    const esp_afe_sr_iface_t *afe_iface_ = nullptr;
    esp_afe_sr_data_t *afe_data_ = nullptr;

    std::atomic<bool> ready_{false};
    std::atomic<bool> shutting_down_{false};
    std::atomic<size_t> feed_frames_{0};

    std::vector<std::string> wake_words_;
    std::string wakenet_model_name_;
    std::vector<int16_t> input_buffer_;
    std::vector<int16_t> output_buffer_;
    std::vector<int16_t> wake_word_cache_;
    size_t wake_word_cache_size_ = 0;
    size_t wake_word_cache_write_ = 0;
    std::array<int16_t, PCM_FRAME_SAMPLES> callback_frame_{};
    std::atomic<bool> vad_speaking_{false};

    WakeWordCallback wake_word_callback_;
    PcmFrameCallback pcm_frame_callback_;
    VadCallback vad_callback_;

    bool createAfe();
    void releaseResources();
    void resetIfInactive();
    void cacheWakeWordPcm(const afe_fetch_result_t *result);
    void handlePcm(const afe_fetch_result_t *result);
    void handleWakeWord(const afe_fetch_result_t *result);
    void handleVoice(const afe_fetch_result_t *result);
    void processingLoop();

    WakeWordCallback copyWakeWordCallback() const;
    PcmFrameCallback copyPcmFrameCallback() const;
    VadCallback copyVadCallback() const;

    static void processingTaskEntry(void *arg);
};

} // namespace esp_brookesia::apps
