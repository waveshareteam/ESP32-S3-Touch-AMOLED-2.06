/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Recorder.hpp"

#include <algorithm>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "bsp_board_extra.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "storage_service.h"

#ifdef ESP_UTILS_LOG_TAG
#undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:Recorder"
#include "esp_lib_utils.h"

LV_IMG_DECLARE(img_app_recorder);

namespace {

constexpr uint32_t RECORD_SAMPLE_RATE = CODEC_VOICE_SAMPLE_RATE;
constexpr uint16_t RECORD_CHANNELS = 2;
constexpr uint16_t RECORD_BITS = CODEC_DEFAULT_BIT_WIDTH;
constexpr size_t TDM_CHANNELS = CODEC_VOICE_INPUT_CHANNELS;
constexpr size_t READ_FRAMES = 256;
constexpr size_t READ_SAMPLES = READ_FRAMES * TDM_CHANNELS;
constexpr size_t OUTPUT_SAMPLES = READ_FRAMES * RECORD_CHANNELS;
constexpr TickType_t WORKER_JOIN_TIMEOUT = pdMS_TO_TICKS(1500);
constexpr int64_t WAV_CHECKPOINT_INTERVAL_US = 5 * 1000 * 1000;

struct __attribute__((packed)) WavHeader {
    char riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t riff_size = 36;
    char wave[4] = {'W', 'A', 'V', 'E'};
    char fmt[4] = {'f', 'm', 't', ' '};
    uint32_t fmt_size = 16;
    uint16_t format = 1;
    uint16_t channels = RECORD_CHANNELS;
    uint32_t sample_rate = RECORD_SAMPLE_RATE;
    uint32_t byte_rate = RECORD_SAMPLE_RATE * RECORD_CHANNELS * (RECORD_BITS / 8);
    uint16_t block_align = RECORD_CHANNELS * (RECORD_BITS / 8);
    uint16_t bits_per_sample = RECORD_BITS;
    char data[4] = {'d', 'a', 't', 'a'};
    uint32_t data_size = 0;
};

static_assert(sizeof(WavHeader) == 44, "WAV header must be 44 bytes");
constexpr uint64_t WAV_MAX_DATA_BYTES = UINT32_MAX - sizeof(WavHeader);

bool checkpoint_wav_header(FILE *file, WavHeader &header, uint64_t data_bytes)
{
    if (file == nullptr || data_bytes > WAV_MAX_DATA_BYTES) {
        return false;
    }

    header.data_size = static_cast<uint32_t>(data_bytes);
    header.riff_size = header.data_size + 36;

    /* Flush PCM first, then make the header describe exactly that durable
     * prefix. Seek back to EOF so subsequent fwrite() calls remain append-like. */
    if (fflush(file) != 0 ||
            fseek(file, 0, SEEK_SET) != 0 ||
            fwrite(&header, 1, sizeof(header), file) != sizeof(header) ||
            fflush(file) != 0 ||
            fsync(fileno(file)) != 0 ||
            fseek(file, 0, SEEK_END) != 0) {
        return false;
    }
    return true;
}

const char *base_name(const char *path)
{
    const char *slash = path ? strrchr(path, '/') : nullptr;
    return slash ? slash + 1 : (path ? path : "");
}

esp_err_t create_recording_directory()
{
    const char *paths[] = {
        STORAGE_SERVICE_MOUNT_POINT "/Waveshare",
        STORAGE_SERVICE_MOUNT_POINT "/Waveshare/Recordings",
    };
    for (const char *path : paths) {
        if (mkdir(path, 0775) != 0 && errno != EEXIST) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

void create_recording_path(char *path, size_t path_size)
{
    time_t now = time(nullptr);
    struct tm local = {};
    localtime_r(&now, &local);
    const unsigned uptime_milliseconds =
        static_cast<unsigned>((esp_timer_get_time() / 1000) % 1000);
    if (local.tm_year + 1900 >= 2024) {
        snprintf(
            path,
            path_size,
            STORAGE_SERVICE_MOUNT_POINT
            "/Waveshare/Recordings/REC-%04d%02d%02d-%02d%02d%02d-%03u.wav",
            local.tm_year + 1900,
            local.tm_mon + 1,
            local.tm_mday,
            local.tm_hour,
            local.tm_min,
            local.tm_sec,
            uptime_milliseconds
        );
    } else {
        snprintf(
            path,
            path_size,
            STORAGE_SERVICE_MOUNT_POINT "/Waveshare/Recordings/REC-boot-%lld.wav",
            static_cast<long long>(esp_timer_get_time() / 1000)
        );
    }
}

} // namespace

namespace esp_brookesia::apps {

Recorder *Recorder::_instance = nullptr;

Recorder *Recorder::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (!_instance) {
        _instance = new Recorder(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

Recorder::Recorder(bool use_status_bar, bool use_navigation_bar)
    : App("Recorder", &img_app_recorder, true, use_status_bar, use_navigation_bar)
{
}

Recorder::~Recorder()
{
    (void)stopRecording(portMAX_DELAY);
    destroyUi();
    _instance = nullptr;
}

bool Recorder::init()
{
    if (!_worker_stopped.load() || _record_task.load()) {
        return false;
    }
    _stop_requested.store(false);
    _state.store(State::Idle);
    _last_error.store(ESP_OK);
    return true;
}

bool Recorder::run()
{
    if (!_sd_gate.ready()) {
        return _sd_gate.open(lv_screen_active(), [](void *context) {
            auto *app = static_cast<Recorder *>(context);
            if (!app->run()) (void)app->notifyCoreClosed();
        }, [](void *context) {
            (void)static_cast<Recorder *>(context)->back();
        }, this);
    }
    destroyUi();
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Voice Recorder");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 54);

    _time_label = lv_label_create(screen);
    lv_label_set_text(_time_label, "00:00");
    lv_obj_set_style_text_font(_time_label, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_obj_set_style_text_color(_time_label, lv_color_hex(0xFF4D67), LV_PART_MAIN);
    lv_obj_align(_time_label, LV_ALIGN_TOP_MID, 0, 112);

    _level_bar = lv_bar_create(screen);
    lv_obj_set_size(_level_bar, 280, 16);
    lv_bar_set_range(_level_bar, 0, 100);
    lv_bar_set_value(_level_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(_level_bar, lv_color_hex(0x24242C), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_level_bar, lv_color_hex(0x35D07F), LV_PART_INDICATOR);
    lv_obj_align(_level_bar, LV_ALIGN_TOP_MID, 0, 180);

    _record_button = lv_button_create(screen);
    lv_obj_set_size(_record_button, 130, 130);
    lv_obj_set_style_radius(_record_button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(_record_button, lv_color_hex(0xEA3152), LV_PART_MAIN);
    lv_obj_set_style_shadow_color(_record_button, lv_color_hex(0xEA3152), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(_record_button, 22, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(_record_button, LV_OPA_40, LV_PART_MAIN);
    lv_obj_align(_record_button, LV_ALIGN_CENTER, 0, 42);
    lv_obj_add_event_cb(_record_button, buttonEvent, LV_EVENT_CLICKED, this);

    _record_button_label = lv_label_create(_record_button);
    lv_label_set_text(_record_button_label, "REC");
    lv_obj_set_style_text_font(_record_button_label, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_center(_record_button_label);

    _status_label = lv_label_create(screen);
    lv_label_set_text(_status_label, "Tap to record to SD card");
    lv_obj_set_width(_status_label, 330);
    lv_obj_set_style_text_align(_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(_status_label, lv_color_hex(0xB7BAC5), LV_PART_MAIN);
    lv_obj_align(_status_label, LV_ALIGN_BOTTOM_MID, 0, -68);

    _path_label = lv_label_create(screen);
    lv_label_set_text(_path_label, "");
    lv_label_set_long_mode(_path_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_path_label, 300);
    lv_obj_set_style_text_align(_path_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(_path_label, lv_color_hex(0x777B87), LV_PART_MAIN);
    lv_obj_align(_path_label, LV_ALIGN_BOTTOM_MID, 0, -38);

    _ui_timer = lv_timer_create(uiTimer, 100, this);
    updateUi();
    return _ui_timer != nullptr;
}

bool Recorder::back()
{
    return notifyCoreClosed();
}

bool Recorder::close()
{
    const bool stopped = stopRecording(WORKER_JOIN_TIMEOUT);
    destroyUi();
    _sd_gate.close();
    return stopped;
}

bool Recorder::deinit()
{
    return close();
}

bool Recorder::pause()
{
    _sd_gate.pause();
    return stopRecording(WORKER_JOIN_TIMEOUT);
}

bool Recorder::resume()
{
    _sd_gate.resume();
    if (_ui_timer) {
        lv_timer_resume(_ui_timer);
    }
    return true;
}

void Recorder::destroyUi()
{
    if (_ui_timer) {
        lv_timer_del(_ui_timer);
        _ui_timer = nullptr;
    }
    _status_label = nullptr;
    _time_label = nullptr;
    _path_label = nullptr;
    _level_bar = nullptr;
    _record_button = nullptr;
    _record_button_label = nullptr;
}

bool Recorder::startRecording()
{
    if (_record_task.load() || !_worker_stopped.load() ||
            _audio_session_acquired.load() || _codec_claimed.load()) {
        return false;
    }
    _stop_requested.store(false);
    _pcm_bytes.store(0);
    _peak.store(0);
    _last_error.store(ESP_OK);
    _last_path[0] = '\0';
    _state.store(State::Starting);
    _worker_stopped.store(false);

    TaskHandle_t task = nullptr;
    BaseType_t result = xTaskCreatePinnedToCore(
        recordTask,
        "sd_recorder",
        8 * 1024,
        this,
        5,
        &task,
        PRO_CPU_NUM
    );
    if (result != pdPASS) {
        _worker_stopped.store(true);
        _state.store(State::Error);
        _last_error.store(ESP_ERR_NO_MEM);
        return false;
    }
    _record_task.store(task);
    // Do not let the new task finish and clear its handle before the creator
    // has published that handle on the other core.
    xTaskNotifyGive(task);
    return true;
}

bool Recorder::stopRecording(TickType_t timeout)
{
    _stop_requested.store(true);
    State state = _state.load();
    if (state == State::Starting || state == State::Recording) {
        _state.store(State::Stopping);
    }
    TaskHandle_t task = _record_task.load();
    if (task == xTaskGetCurrentTaskHandle()) {
        return false;
    }
    const TickType_t start = xTaskGetTickCount();
    while (!_worker_stopped.load()) {
        if (timeout != portMAX_DELAY && xTaskGetTickCount() - start >= timeout) {
            ESP_UTILS_LOGE("Timed out waiting for recorder worker");
            return false;
        }
        vTaskDelay(1);
    }
    return releaseAudioSession();
}

bool Recorder::releaseAudioSession()
{
    if (_codec_claimed.load()) {
        const esp_err_t stop_result = bsp_extra_codec_dev_stop();
        if (stop_result != ESP_OK) {
            _last_error.store(stop_result);
            ESP_UTILS_LOGE(
                "Recorder codec shutdown failed; retaining audio session: %s",
                esp_err_to_name(stop_result)
            );
            return false;
        }
        _codec_claimed.store(false);
    }
    if (_audio_session_acquired.load()) {
        const esp_err_t release_result =
            bsp_extra_audio_session_release(BSP_EXTRA_AUDIO_OWNER_RECORDER);
        if (release_result != ESP_OK) {
            _last_error.store(release_result);
            return false;
        }
        _audio_session_acquired.store(false);
    }
    return true;
}

void Recorder::buttonEvent(lv_event_t *event)
{
    auto *self = static_cast<Recorder *>(lv_event_get_user_data(event));
    if (!self || lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    State state = self->_state.load();
    if (state == State::Starting || state == State::Recording) {
        self->_stop_requested.store(true);
        self->_state.store(State::Stopping);
    } else if (state != State::Stopping) {
        (void)self->startRecording();
    }
}

void Recorder::uiTimer(lv_timer_t *timer)
{
    auto *self = static_cast<Recorder *>(lv_timer_get_user_data(timer));
    if (self) {
        self->updateUi();
    }
}

void Recorder::updateUi()
{
    State state = _state.load();
    const bool active = state == State::Starting || state == State::Recording ||
                        state == State::Stopping;
    const int64_t started = _started_us.load();
    uint64_t elapsed_seconds = 0;
    if (started > 0 && active) {
        elapsed_seconds = static_cast<uint64_t>(esp_timer_get_time() - started) / 1000000ULL;
    } else {
        const uint64_t bytes_per_second =
            RECORD_SAMPLE_RATE * RECORD_CHANNELS * (RECORD_BITS / 8);
        elapsed_seconds = bytes_per_second ? _pcm_bytes.load() / bytes_per_second : 0;
    }
    if (_time_label) {
        lv_label_set_text_fmt(
            _time_label,
            "%02llu:%02llu",
            static_cast<unsigned long long>(elapsed_seconds / 60),
            static_cast<unsigned long long>(elapsed_seconds % 60)
        );
    }
    if (_level_bar) {
        const uint32_t peak_percent = std::min<uint32_t>(100, _peak.load() * 100 / 32767);
        lv_bar_set_value(_level_bar, static_cast<int32_t>(peak_percent), LV_ANIM_ON);
        if (!active) {
            _peak.store(0);
        }
    }
    if (_record_button_label) {
        lv_label_set_text(_record_button_label, active ? "STOP" : "REC");
    }
    if (_record_button) {
        lv_obj_set_style_bg_color(
            _record_button,
            active ? lv_color_hex(0xF5F5F5) : lv_color_hex(0xEA3152),
            LV_PART_MAIN
        );
        if (_record_button_label) {
            lv_obj_set_style_text_color(
                _record_button_label,
                active ? lv_color_hex(0xEA3152) : lv_color_white(),
                LV_PART_MAIN
            );
        }
    }
    if (_status_label) {
        const char *status = "Tap to record to SD card";
        switch (state) {
        case State::Starting: status = "Preparing SD and microphones..."; break;
        case State::Recording: status = "Recording MIC1 + MIC2"; break;
        case State::Stopping: status = "Saving WAV..."; break;
        case State::Saved: status = "Recording saved"; break;
        case State::Error: status = "Recording failed"; break;
        case State::Idle: default: break;
        }
        lv_label_set_text(_status_label, status);
    }
    if (_path_label) {
        if (state == State::Error) {
            lv_label_set_text(_path_label, esp_err_to_name(_last_error.load()));
        } else if (_worker_stopped.load() && _last_path[0]) {
            lv_label_set_text(_path_label, base_name(_last_path));
        } else {
            lv_label_set_text(_path_label, "24 kHz / 16-bit / stereo WAV");
        }
    }
}

void Recorder::recordTask(void *arg)
{
    auto *self = static_cast<Recorder *>(arg);
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    storage_service_lease_t lease = {};
    FILE *file = nullptr;
    char final_path[sizeof(self->_last_path)] = {};
    char partial_path[sizeof(self->_last_path)] = {};
    uint64_t data_bytes = 0;
    esp_err_t result = storage_service_acquire(&lease);

    if (result == ESP_OK) {
        result = create_recording_directory();
    }
    if (result == ESP_OK) {
        create_recording_path(final_path, sizeof(final_path));
        const int length = snprintf(
            partial_path, sizeof(partial_path), "%s.partial", final_path
        );
        if (length < 0 || static_cast<size_t>(length) >= sizeof(partial_path)) {
            result = ESP_ERR_INVALID_SIZE;
        } else {
            strlcpy(self->_last_path, final_path, sizeof(self->_last_path));
        }
    }
    if (result == ESP_OK) {
        /* O_EXCL preserves a recoverable partial file if a generated name ever
         * collides. ESP-IDF's FatFs VFS maps this to FA_CREATE_NEW. */
        const int fd = open(partial_path, O_RDWR | O_CREAT | O_EXCL, 0664);
        if (fd < 0) {
            result = ESP_FAIL;
        } else {
            file = fdopen(fd, "wb+");
            if (file == nullptr) {
                (void)::close(fd);
                (void)remove(partial_path);
                result = ESP_FAIL;
            }
        }
    }

    WavHeader header;
    if (result == ESP_OK && !checkpoint_wav_header(file, header, 0)) {
        result = ESP_FAIL;
    }
    if (result == ESP_OK) {
        result = bsp_extra_audio_session_acquire(BSP_EXTRA_AUDIO_OWNER_RECORDER);
        if (result == ESP_OK) {
            self->_audio_session_acquired.store(true);
        }
    }
    if (result == ESP_OK) {
        result = bsp_extra_codec_init();
        if (result == ESP_OK) {
            self->_codec_claimed.store(true);
        }
    }
    if (result == ESP_OK) {
        result = bsp_extra_codec_set_voice_fs(
            RECORD_SAMPLE_RATE,
            RECORD_BITS,
            TDM_CHANNELS,
            BSP_EXTRA_ES7210_TDM_ALL_SLOTS_MASK,
            BSP_EXTRA_ES7210_PHYSICAL_FRONT_MIC_MASK
        );
    }

    int16_t *raw = nullptr;
    int16_t *stereo = nullptr;
    if (result == ESP_OK) {
        raw = static_cast<int16_t *>(heap_caps_malloc(
            READ_SAMPLES * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        stereo = static_cast<int16_t *>(heap_caps_malloc(
            OUTPUT_SAMPLES * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (!raw || !stereo) {
            result = ESP_ERR_NO_MEM;
        }
    }

    if (result == ESP_OK) {
        self->_started_us.store(esp_timer_get_time());
        self->_state.store(State::Recording);
        int64_t next_checkpoint_us = esp_timer_get_time() + WAV_CHECKPOINT_INTERVAL_US;
        while (!self->_stop_requested.load()) {
            size_t bytes_read = 0;
            result = bsp_extra_i2s_read(
                raw,
                READ_SAMPLES * sizeof(int16_t),
                &bytes_read,
                portMAX_DELAY
            );
            if (result != ESP_OK || bytes_read != READ_SAMPLES * sizeof(int16_t)) {
                if (result == ESP_OK) result = ESP_FAIL;
                break;
            }

            uint32_t peak = 0;
            for (size_t frame = 0; frame < READ_FRAMES; ++frame) {
                const int16_t left = raw[frame * TDM_CHANNELS + 0];
                const int16_t right = raw[frame * TDM_CHANNELS + 2];
                stereo[frame * 2] = left;
                stereo[frame * 2 + 1] = right;
                const uint32_t left_abs = left < 0 ? static_cast<uint32_t>(-static_cast<int32_t>(left)) : left;
                const uint32_t right_abs = right < 0 ? static_cast<uint32_t>(-static_cast<int32_t>(right)) : right;
                peak = std::max(peak, std::max(left_abs, right_abs));
            }
            const size_t output_bytes = OUTPUT_SAMPLES * sizeof(int16_t);
            if (data_bytes > WAV_MAX_DATA_BYTES - output_bytes) {
                result = ESP_ERR_INVALID_SIZE;
                break;
            }
            if (fwrite(stereo, 1, output_bytes, file) != output_bytes) {
                result = ESP_FAIL;
                break;
            }
            data_bytes += output_bytes;
            self->_pcm_bytes.store(data_bytes);
            self->_peak.store(peak);
            const int64_t now_us = esp_timer_get_time();
            if (now_us >= next_checkpoint_us) {
                if (!checkpoint_wav_header(file, header, data_bytes)) {
                    result = ESP_FAIL;
                    break;
                }
                next_checkpoint_us = now_us + WAV_CHECKPOINT_INTERVAL_US;
            }
        }
    }

    self->_state.store(State::Stopping);
    if (!self->releaseAudioSession() && result == ESP_OK) {
        result = self->_last_error.load();
    }
    heap_caps_free(raw);
    heap_caps_free(stereo);

    if (file != nullptr) {
        const bool final_checkpoint_ok = checkpoint_wav_header(file, header, data_bytes);
        if (!final_checkpoint_ok && result == ESP_OK) {
            result = ESP_FAIL;
        }
        if (fclose(file) != 0 && result == ESP_OK) {
            result = ESP_FAIL;
        }
        file = nullptr;

        /* ESP-IDF's FatFs rename maps to same-volume f_rename(), refuses an
         * existing destination, and syncs the filesystem before returning. */
        if (result == ESP_OK && final_checkpoint_ok) {
            if (rename(partial_path, final_path) != 0) {
                result = ESP_FAIL;
            }
        }

        if (result != ESP_OK) {
            if (data_bytes == 0) {
                (void)remove(partial_path);
                self->_last_path[0] = '\0';
            } else {
                strlcpy(self->_last_path, partial_path, sizeof(self->_last_path));
            }
        }
    } else if (result != ESP_OK) {
        self->_last_path[0] = '\0';
    }
    storage_service_release(&lease);

    self->_last_error.store(result);
    self->_started_us.store(0);
    self->_peak.store(0);
    self->_state.store(result == ESP_OK ? State::Saved : State::Error);
    self->_record_task.store(nullptr);
    self->_worker_stopped.store(true);
    vTaskDelete(nullptr);
}

} // namespace esp_brookesia::apps
