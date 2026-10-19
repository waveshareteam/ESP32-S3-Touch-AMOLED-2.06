/*
 * Derived from 78/xiaozhi-esp32 (MIT).
 *
 * SPDX-License-Identifier: MIT
 */

#include "XiaozhiAudioProcessor.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vadn_models.h"

namespace esp_brookesia::apps {

namespace {

constexpr char TAG[] = "XiaozhiAudio";
constexpr char AFE_INPUT_FORMAT[] = "MMR";

class SemaphoreLock {
public:
    explicit SemaphoreLock(SemaphoreHandle_t semaphore): semaphore_(semaphore)
    {
        locked_ = semaphore_ && xSemaphoreTake(semaphore_, portMAX_DELAY) == pdTRUE;
    }

    ~SemaphoreLock()
    {
        if (locked_) {
            xSemaphoreGive(semaphore_);
        }
    }

    explicit operator bool() const
    {
        return locked_;
    }

private:
    SemaphoreHandle_t semaphore_ = nullptr;
    bool locked_ = false;
};

} // namespace

XiaozhiAudioProcessor::XiaozhiAudioProcessor()
{
    lifecycle_mutex_ = xSemaphoreCreateMutex();
    afe_mutex_ = xSemaphoreCreateMutex();
    input_mutex_ = xSemaphoreCreateMutex();
    output_mutex_ = xSemaphoreCreateMutex();
    callback_mutex_ = xSemaphoreCreateMutex();
}

XiaozhiAudioProcessor::~XiaozhiAudioProcessor()
{
    if (!shutdown()) {
        ESP_LOGE(TAG, "Audio processor could not be shut down cleanly");
    }

    if (callback_mutex_) {
        vSemaphoreDelete(callback_mutex_);
    }
    if (output_mutex_) {
        vSemaphoreDelete(output_mutex_);
    }
    if (input_mutex_) {
        vSemaphoreDelete(input_mutex_);
    }
    if (afe_mutex_) {
        vSemaphoreDelete(afe_mutex_);
    }
    if (lifecycle_mutex_) {
        vSemaphoreDelete(lifecycle_mutex_);
    }
}

bool XiaozhiAudioProcessor::initialize()
{
    if (!lifecycle_mutex_ || !afe_mutex_ || !input_mutex_ || !output_mutex_ ||
            !callback_mutex_) {
        ESP_LOGE(TAG, "Failed to allocate synchronization objects");
        return false;
    }

    SemaphoreLock lifecycle_lock(lifecycle_mutex_);
    if (!lifecycle_lock) {
        return false;
    }
    if (ready_.load()) {
        return true;
    }
    if (shutting_down_.load()) {
        return false;
    }

    releaseResources();
    event_group_ = xEventGroupCreate();
    if (!event_group_) {
        ESP_LOGE(TAG, "Failed to create event group");
        return false;
    }

    if (!createAfe()) {
        releaseResources();
        return false;
    }

    xEventGroupClearBits(
        event_group_,
        EVENT_WAKE_WORD_ENABLED | EVENT_VOICE_ENABLED | EVENT_SHUTDOWN |
            EVENT_TASK_EXITED
    );

    BaseType_t created = xTaskCreate(
        processingTaskEntry,
        "xiaozhi_afe",
        PROCESS_TASK_STACK_SIZE,
        this,
        PROCESS_TASK_PRIORITY,
        &processing_task_
    );
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create AFE processing task");
        processing_task_ = nullptr;
        releaseResources();
        return false;
    }

    ready_.store(true);
    ESP_LOGI(
        TAG,
        "Ready: input=%s, feed=%u frames, output=%u samples",
        AFE_INPUT_FORMAT,
        static_cast<unsigned>(feed_frames_.load()),
        static_cast<unsigned>(PCM_FRAME_SAMPLES)
    );
    return true;
}

bool XiaozhiAudioProcessor::shutdown()
{
    if (!lifecycle_mutex_) {
        return true;
    }

    SemaphoreLock lifecycle_lock(lifecycle_mutex_);
    if (!lifecycle_lock) {
        return false;
    }
    if (!event_group_) {
        ready_.store(false);
        shutting_down_.store(false);
        return true;
    }
    if (processing_task_ && xTaskGetCurrentTaskHandle() == processing_task_) {
        ESP_LOGE(TAG, "shutdown() cannot be called from an audio callback");
        return false;
    }

    const bool task_running = processing_task_ != nullptr;
    shutting_down_.store(true);
    ready_.store(false);
    xEventGroupClearBits(event_group_, EVENT_WAKE_WORD_ENABLED | EVENT_VOICE_ENABLED);
    xEventGroupSetBits(event_group_, EVENT_SHUTDOWN);

    {
        SemaphoreLock afe_lock(afe_mutex_);
        if (afe_lock && afe_iface_ && afe_data_) {
            afe_iface_->disable_wakenet(afe_data_);
        }
    }

    if (task_running) {
        xEventGroupWaitBits(
            event_group_, EVENT_TASK_EXITED, pdFALSE, pdTRUE, portMAX_DELAY
        );
        // The processing task gives this bit immediately before self-delete.
        // Yield once so it can return from xEventGroupSetBits before the event
        // group is released below.
        vTaskDelay(1);
    }

    // A feed call that observed the old ready state must finish before the AFE
    // instance is destroyed.
    {
        SemaphoreLock input_lock(input_mutex_);
        if (input_lock) {
            input_buffer_.clear();
        }
    }
    {
        SemaphoreLock output_lock(output_mutex_);
        if (output_lock) {
            output_buffer_.clear();
        }
    }

    releaseResources();
    shutting_down_.store(false);
    return true;
}

bool XiaozhiAudioProcessor::feed(
    const int16_t *interleaved_pcm,
    size_t frame_count
)
{
    if (!interleaved_pcm || frame_count == 0 || !ready_.load() ||
            shutting_down_.load()) {
        return false;
    }

    SemaphoreLock input_lock(input_mutex_);
    if (!input_lock || !ready_.load() || shutting_down_.load() || !event_group_ ||
            !afe_iface_ || !afe_data_) {
        return false;
    }

    EventBits_t bits = xEventGroupGetBits(event_group_);
    if ((bits & EVENT_ACTIVE) == 0 || (bits & EVENT_SHUTDOWN) != 0) {
        return false;
    }

    const size_t sample_count = frame_count * INPUT_CHANNELS;
    input_buffer_.insert(
        input_buffer_.end(),
        interleaved_pcm,
        interleaved_pcm + sample_count
    );

    const size_t feed_samples = feed_frames_.load() * INPUT_CHANNELS;
    if (feed_samples == 0) {
        return false;
    }

    size_t consumed = 0;
    while (input_buffer_.size() - consumed >= feed_samples) {
        afe_iface_->feed(afe_data_, input_buffer_.data() + consumed);
        consumed += feed_samples;
    }
    if (consumed > 0) {
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + consumed);
    }
    return true;
}

bool XiaozhiAudioProcessor::enableWakeWord(bool enable)
{
    SemaphoreLock afe_lock(afe_mutex_);
    if (!afe_lock || !ready_.load() || shutting_down_.load() || !event_group_ ||
            !afe_iface_ || !afe_data_) {
        return false;
    }

    EventBits_t bits = xEventGroupGetBits(event_group_);
    bool enabled = (bits & EVENT_WAKE_WORD_ENABLED) != 0;
    if (enabled == enable) {
        return true;
    }

    if (enable) {
        SemaphoreLock output_lock(output_mutex_);
        if (output_lock) {
            wake_word_cache_size_ = 0;
            wake_word_cache_write_ = 0;
        }
        afe_iface_->enable_wakenet(afe_data_);
        xEventGroupSetBits(event_group_, EVENT_WAKE_WORD_ENABLED);
    } else {
        xEventGroupClearBits(event_group_, EVENT_WAKE_WORD_ENABLED);
        afe_iface_->disable_wakenet(afe_data_);
    }

    if (!enable) {
        resetIfInactive();
    }
    return true;
}

bool XiaozhiAudioProcessor::enableVoiceProcessing(bool enable)
{
    SemaphoreLock afe_lock(afe_mutex_);
    if (!afe_lock || !ready_.load() || shutting_down_.load() || !event_group_ ||
            !afe_iface_ || !afe_data_) {
        return false;
    }

    EventBits_t bits = xEventGroupGetBits(event_group_);
    bool enabled = (bits & EVENT_VOICE_ENABLED) != 0;
    if (enabled == enable) {
        return true;
    }

    {
        SemaphoreLock output_lock(output_mutex_);
        if (output_lock) {
            output_buffer_.clear();
            vad_speaking_.store(false);
        }
    }

    if (enable) {
        xEventGroupSetBits(event_group_, EVENT_VOICE_ENABLED);
    } else {
        xEventGroupClearBits(event_group_, EVENT_VOICE_ENABLED);
        resetIfInactive();
    }
    return true;
}

bool XiaozhiAudioProcessor::isReady() const
{
    return ready_.load() && !shutting_down_.load();
}

size_t XiaozhiAudioProcessor::getFeedFrames() const
{
    return feed_frames_.load();
}

bool XiaozhiAudioProcessor::takeWakeWordPcm(std::vector<int16_t> &pcm)
{
    SemaphoreLock output_lock(output_mutex_);
    if (!output_lock || wake_word_cache_.empty() ||
            wake_word_cache_size_ == 0) {
        pcm.clear();
        return false;
    }

    const size_t capacity = wake_word_cache_.size();
    const size_t oldest =
        (wake_word_cache_write_ + capacity - wake_word_cache_size_) % capacity;
    pcm.resize(wake_word_cache_size_);
    const size_t first =
        std::min(wake_word_cache_size_, capacity - oldest);
    std::copy_n(
        wake_word_cache_.begin() + oldest,
        first,
        pcm.begin()
    );
    if (wake_word_cache_size_ > first) {
        std::copy_n(
            wake_word_cache_.begin(),
            wake_word_cache_size_ - first,
            pcm.begin() + first
        );
    }
    wake_word_cache_size_ = 0;
    wake_word_cache_write_ = 0;
    return true;
}

void XiaozhiAudioProcessor::setWakeWordCallback(WakeWordCallback callback)
{
    SemaphoreLock lock(callback_mutex_);
    if (lock) {
        wake_word_callback_ = std::move(callback);
    }
}

void XiaozhiAudioProcessor::setPcmFrameCallback(PcmFrameCallback callback)
{
    SemaphoreLock lock(callback_mutex_);
    if (lock) {
        pcm_frame_callback_ = std::move(callback);
    }
}

void XiaozhiAudioProcessor::setVadCallback(VadCallback callback)
{
    SemaphoreLock lock(callback_mutex_);
    if (lock) {
        vad_callback_ = std::move(callback);
    }
}

bool XiaozhiAudioProcessor::createAfe()
{
    const esp_partition_t *model_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model"
    );
    if (!model_partition) {
        ESP_LOGE(
            TAG,
            "Speech model partition 'model' is missing; run a full flash so "
            "the partition table and srmodels.bin are written"
        );
        return false;
    }

    models_ = esp_srmodel_init("model");
    if (!models_) {
        ESP_LOGE(
            TAG,
            "Speech model partition exists but could not be loaded; run a "
            "full flash including srmodels.bin"
        );
        return false;
    }
    if (models_->num <= 0) {
        ESP_LOGE(
            TAG,
            "Speech model image is empty or unreadable "
            "(count=%d, offset=0x%08lx); rebuild and full-flash the "
            "partition table and srmodels.bin",
            models_->num,
            static_cast<unsigned long>(model_partition->address)
        );
        return false;
    }
    ESP_LOGI(
        TAG, "Loaded %d speech model(s) from offset 0x%08lx",
        models_->num, static_cast<unsigned long>(model_partition->address)
    );

    char *wakenet_model = esp_srmodel_filter(models_, ESP_WN_PREFIX, nullptr);
    if (!wakenet_model) {
        ESP_LOGE(TAG, "No WakeNet model found");
        return false;
    }
    wakenet_model_name_ = wakenet_model;

    const char *words = esp_srmodel_get_wake_words(models_, wakenet_model);
    if (words) {
        const char *begin = words;
        while (*begin != '\0') {
            const char *end = std::strchr(begin, ';');
            if (!end) {
                end = begin + std::strlen(begin);
            }
            if (end > begin) {
                wake_words_.emplace_back(begin, static_cast<size_t>(end - begin));
            }
            if (*end == '\0') {
                break;
            }
            begin = end + 1;
        }
    }

    char *vad_model = esp_srmodel_filter(models_, ESP_VADN_PREFIX, nullptr);
    afe_config_t *config =
        afe_config_init(AFE_INPUT_FORMAT, models_, AFE_TYPE_FD, AFE_MODE_LOW_COST);
    if (!config) {
        ESP_LOGE(TAG, "Failed to create AFE configuration");
        return false;
    }

    config->aec_init = true;
    config->ns_init = false;
    config->vad_init = true;
    config->vad_mode = VAD_MODE_0;
    config->vad_min_noise_ms = 100;
    if (vad_model) {
        config->vad_model_name = vad_model;
    }
    config->wakenet_init = true;
    config->wakenet_model_name = wakenet_model;
    config->agc_init = false;
    config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    afe_iface_ = esp_afe_handle_from_config(config);
    if (afe_iface_) {
        afe_data_ = afe_iface_->create_from_config(config);
    }
    afe_config_free(config);

    if (!afe_iface_ || !afe_data_) {
        ESP_LOGE(TAG, "Failed to create AFE instance");
        afe_iface_ = nullptr;
        afe_data_ = nullptr;
        return false;
    }

    afe_iface_->disable_wakenet(afe_data_);
    feed_frames_.store(static_cast<size_t>(afe_iface_->get_feed_chunksize(afe_data_)));
    if (feed_frames_.load() == 0) {
        ESP_LOGE(TAG, "AFE returned an invalid feed size");
        return false;
    }

    input_buffer_.clear();
    input_buffer_.reserve(feed_frames_.load() * INPUT_CHANNELS * 2);
    output_buffer_.clear();
    output_buffer_.reserve(PCM_FRAME_SAMPLES * 2);
    wake_word_cache_.assign(WAKE_WORD_CACHE_SAMPLES, 0);
    wake_word_cache_size_ = 0;
    wake_word_cache_write_ = 0;
    afe_iface_->print_pipeline(afe_data_);
    return true;
}

void XiaozhiAudioProcessor::releaseResources()
{
    ready_.store(false);
    feed_frames_.store(0);
    processing_task_ = nullptr;

    {
        SemaphoreLock afe_lock(afe_mutex_);
        if (afe_lock && afe_iface_ && afe_data_) {
            afe_iface_->destroy(afe_data_);
        }
        afe_data_ = nullptr;
        afe_iface_ = nullptr;
    }

    if (models_) {
        esp_srmodel_deinit(models_);
        models_ = nullptr;
    }
    if (event_group_) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }

    wake_words_.clear();
    wakenet_model_name_.clear();
    input_buffer_.clear();
    output_buffer_.clear();
    wake_word_cache_.clear();
    wake_word_cache_size_ = 0;
    wake_word_cache_write_ = 0;
    vad_speaking_.store(false);
}

void XiaozhiAudioProcessor::resetIfInactive()
{
    if (!event_group_ || !afe_iface_ || !afe_data_) {
        return;
    }
    EventBits_t bits = xEventGroupGetBits(event_group_);
    if ((bits & EVENT_ACTIVE) != 0) {
        return;
    }

    {
        SemaphoreLock input_lock(input_mutex_);
        if (input_lock) {
            input_buffer_.clear();
        }
    }
    afe_iface_->reset_buffer(afe_data_);
}

void XiaozhiAudioProcessor::cacheWakeWordPcm(
    const afe_fetch_result_t *result
)
{
    if (!result || !result->data || result->data_size <= 0 ||
            wake_word_cache_.empty()) {
        return;
    }

    const size_t capacity = wake_word_cache_.size();
    const int16_t *data = result->data;
    size_t samples =
        static_cast<size_t>(result->data_size) / sizeof(int16_t);

    SemaphoreLock output_lock(output_mutex_);
    if (!output_lock) {
        return;
    }
    if (samples >= capacity) {
        data += samples - capacity;
        samples = capacity;
        std::copy_n(
            data,
            samples,
            wake_word_cache_.begin()
        );
        wake_word_cache_size_ = capacity;
        wake_word_cache_write_ = 0;
        return;
    }

    const size_t first =
        std::min(samples, capacity - wake_word_cache_write_);
    std::copy_n(data, first, wake_word_cache_.begin() + wake_word_cache_write_);
    if (samples > first) {
        std::copy_n(
            data + first,
            samples - first,
            wake_word_cache_.begin()
        );
    }
    wake_word_cache_write_ = (wake_word_cache_write_ + samples) % capacity;
    wake_word_cache_size_ =
        std::min(capacity, wake_word_cache_size_ + samples);
}

void XiaozhiAudioProcessor::handlePcm(const afe_fetch_result_t *result)
{
    if (!result || !result->data || result->data_size <= 0 || !event_group_) {
        return;
    }

    const size_t sample_count =
        static_cast<size_t>(result->data_size) / sizeof(int16_t);
    size_t input_offset = 0;
    while (input_offset < sample_count) {
        bool frame_ready = false;
        {
            SemaphoreLock output_lock(output_mutex_);
            if (!output_lock || !event_group_ ||
                    (xEventGroupGetBits(event_group_) & EVENT_SHUTDOWN) != 0) {
                return;
            }

            size_t required = PCM_FRAME_SAMPLES - output_buffer_.size();
            size_t copy_count = std::min(required, sample_count - input_offset);
            output_buffer_.insert(
                output_buffer_.end(),
                result->data + input_offset,
                result->data + input_offset + copy_count
            );
            input_offset += copy_count;

            if (output_buffer_.size() == PCM_FRAME_SAMPLES) {
                std::copy(
                    output_buffer_.begin(), output_buffer_.end(),
                    callback_frame_.begin()
                );
                output_buffer_.clear();
                frame_ready = true;
            }
        }

        if (frame_ready) {
            PcmFrameCallback callback = copyPcmFrameCallback();
            if (callback) {
                callback(callback_frame_.data(), callback_frame_.size());
            }
        }
    }
}

void XiaozhiAudioProcessor::handleWakeWord(const afe_fetch_result_t *result)
{
    if (!result || !event_group_) {
        return;
    }
    cacheWakeWordPcm(result);
    if (result->wakeup_state != WAKENET_DETECTED) {
        return;
    }

    std::string wake_word = wakenet_model_name_;
    int model_index = result->wakenet_model_index - 1;
    if (model_index >= 0 && model_index < static_cast<int>(wake_words_.size())) {
        wake_word = wake_words_[model_index];
    } else if (!wake_words_.empty()) {
        ESP_LOGW(
            TAG,
            "Invalid WakeNet model index %d; using the first wake word",
            result->wakenet_model_index
        );
        wake_word = wake_words_.front();
    }

    // Reset only the WakeNet detection state; voice PCM keeps flowing.
    {
        SemaphoreLock afe_lock(afe_mutex_);
        if (!afe_lock || shutting_down_.load() ||
                (xEventGroupGetBits(event_group_) & EVENT_SHUTDOWN) != 0) {
            return;
        }
        xEventGroupClearBits(event_group_, EVENT_WAKE_WORD_ENABLED);
        afe_iface_->disable_wakenet(afe_data_);
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
    WakeWordCallback callback = copyWakeWordCallback();
    if (callback) {
        callback(wake_word);
    }
}

void XiaozhiAudioProcessor::handleVoice(const afe_fetch_result_t *result)
{
    if (!result || !event_group_ ||
            (xEventGroupGetBits(event_group_) & EVENT_VOICE_ENABLED) == 0) {
        return;
    }

    bool speaking = result->vad_state == VAD_SPEECH;
    bool was_speaking = vad_speaking_.exchange(speaking);
    if (speaking != was_speaking) {
        VadCallback callback = copyVadCallback();
        if (callback) {
            callback(speaking);
        }
    }
    handlePcm(result);
}

void XiaozhiAudioProcessor::processingLoop()
{
    while (event_group_) {
        EventBits_t bits = xEventGroupWaitBits(
            event_group_, EVENT_ACTIVE | EVENT_SHUTDOWN, pdFALSE, pdFALSE,
            portMAX_DELAY
        );
        if ((bits & EVENT_SHUTDOWN) != 0) {
            break;
        }

        afe_fetch_result_t *result = afe_iface_->fetch_with_delay(
            afe_data_, pdMS_TO_TICKS(FETCH_TIMEOUT_MS)
        );
        bits = xEventGroupGetBits(event_group_);
        if ((bits & EVENT_SHUTDOWN) != 0) {
            break;
        }
        if (!result || result->ret_value == ESP_FAIL) {
            continue;
        }

        const bool wake_word_enabled =
            (bits & EVENT_WAKE_WORD_ENABLED) != 0;
        const bool voice_enabled = (bits & EVENT_VOICE_ENABLED) != 0;
        if (wake_word_enabled) {
            handleWakeWord(result);
        }
        if (voice_enabled) {
            handleVoice(result);
        }
    }
}

XiaozhiAudioProcessor::WakeWordCallback
XiaozhiAudioProcessor::copyWakeWordCallback() const
{
    SemaphoreLock lock(callback_mutex_);
    return lock ? wake_word_callback_ : WakeWordCallback{};
}

XiaozhiAudioProcessor::PcmFrameCallback
XiaozhiAudioProcessor::copyPcmFrameCallback() const
{
    SemaphoreLock lock(callback_mutex_);
    return lock ? pcm_frame_callback_ : PcmFrameCallback{};
}

XiaozhiAudioProcessor::VadCallback XiaozhiAudioProcessor::copyVadCallback() const
{
    SemaphoreLock lock(callback_mutex_);
    return lock ? vad_callback_ : VadCallback{};
}

void XiaozhiAudioProcessor::processingTaskEntry(void *arg)
{
    auto *processor = static_cast<XiaozhiAudioProcessor *>(arg);
    processor->processingLoop();
    if (processor->event_group_) {
        xEventGroupSetBits(processor->event_group_, EVENT_TASK_EXITED);
    }
    vTaskDelete(nullptr);
}

} // namespace esp_brookesia::apps
