#include "lvgl.h"
#include "esp_brookesia.hpp"
#include "esp_task.h"
#include "esp_heap_caps.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:SpecAnalyzer"
#include "esp_lib_utils.h"
#include "SpecAnalyzer.hpp"
#define APP_NAME "SpecAnalyzer"

using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems;

LV_IMG_DECLARE(img_app_specanalyzer);

namespace {

constexpr uint32_t SPECTRUM_REFRESH_PERIOD_MS = 66;
constexpr uint32_t WORKER_JOIN_TIMEOUT_MS = 500;

void fill_rgb565_rect(
    uint16_t *buffer,
    int width,
    int height,
    int x1,
    int y1,
    int x2,
    int y2,
    uint16_t color
)
{
    if (!buffer || x2 < 0 || y2 < 0 || x1 >= width || y1 >= height) {
        return;
    }

    x1 = std::max(x1, 0);
    y1 = std::max(y1, 0);
    x2 = std::min(x2, width - 1);
    y2 = std::min(y2, height - 1);
    if (x1 > x2 || y1 > y2) {
        return;
    }

    for (int y = y1; y <= y2; ++y) {
        std::fill(buffer + y * width + x1, buffer + y * width + x2 + 1, color);
    }
}

} // namespace

namespace esp_brookesia::apps {

SpecAnalyzer *SpecAnalyzer::_instance = nullptr;

SpecAnalyzer *SpecAnalyzer::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (_instance == nullptr) {
        _instance = new SpecAnalyzer(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

SpecAnalyzer::SpecAnalyzer(bool use_status_bar, bool use_navigation_bar) :
    App(APP_NAME, &img_app_specanalyzer, true, use_status_bar, use_navigation_bar),
    _canvas(nullptr),
    _mic_label(nullptr),
    _timer(nullptr),
    _audio_task_handle(nullptr),
    _capture_requested(false),
    _worker_exit(false),
    _worker_stopped(true),
    _codec_released(true),
    _codec_release_result(ESP_OK),
    _audio_session_acquired(false),
    _capture_state(CaptureState::Idle),
    _shown_capture_state(CaptureState::Idle),
    _draw_buf(nullptr)
{
    portMUX_INITIALIZE(&_spectrum_mux);

    // Clear DSP state so the first rendered frame starts from silence.
    std::memset(_raw_data, 0, sizeof(_raw_data));
    std::memset(_audio_buffer, 0, sizeof(_audio_buffer));
    std::memset(_fft_buffer, 0, sizeof(_fft_buffer));
    std::memset(_spectrum, 0, sizeof(_spectrum));
    std::memset(_display_spectrum, 0, sizeof(_display_spectrum));
    std::memset(_peak, 0, sizeof(_peak));
    std::memset(_smooth_spectrum, 0, sizeof(_smooth_spectrum));
    std::memset(_published_spectrum, 0, sizeof(_published_spectrum));
    std::memset(_render_spectrum, 0, sizeof(_render_spectrum));
    std::memset(_bar_colors, 0, sizeof(_bar_colors));
    std::memset(_peak_colors, 0, sizeof(_peak_colors));
}

SpecAnalyzer::~SpecAnalyzer()
{
    const bool deinit_ok = deinit();
    if (!deinit_ok && !_worker_stopped.load()) {
        // This is a process-lifetime singleton, so destruction is exceptional.
        // Never free its storage while the worker can still dereference it and
        // never force-delete a task which may own an I2S/codec lock.
        ESP_UTILS_LOGE("Waiting for the audio worker before destroying the singleton");
        (void)stopAudioTask(portMAX_DELAY);
    }
    destroyUi();
    _instance = nullptr;
}

bool SpecAnalyzer::stopAudioTask(TickType_t timeout_ticks)
{
    _capture_requested.store(false);
    _worker_exit.store(true);

    TaskHandle_t task = _audio_task_handle.load();
    if (task == xTaskGetCurrentTaskHandle()) {
        ESP_UTILS_LOGE("Audio worker cannot join itself");
        return false;
    }
    if (task) {
        xTaskNotifyGive(task);
    }
    if (_worker_stopped.load()) {
        return true;
    }

    const TickType_t start_tick = xTaskGetTickCount();
    while (!_worker_stopped.load()) {
        if (timeout_ticks != portMAX_DELAY &&
            (xTaskGetTickCount() - start_tick) >= timeout_ticks) {
            ESP_UTILS_LOGE("Timed out waiting for the audio worker to exit");
            return false;
        }
        vTaskDelay(1);
    }
    return true;
}

bool SpecAnalyzer::releaseCodec(CaptureState success_state)
{
    esp_err_t result = ESP_OK;
    if (_audio_session_acquired.load()) {
        result = bsp_extra_codec_dev_stop();
        if (result == ESP_OK) {
            result = bsp_extra_audio_session_release(BSP_EXTRA_AUDIO_OWNER_SPEC_ANALYZER);
            if (result == ESP_OK) {
                _audio_session_acquired.store(false);
            }
        }
    }
    _codec_release_result.store(result);
    if (result == ESP_OK && !_audio_session_acquired.load()) {
        _codec_released.store(true);
        _capture_state.store(success_state);
        return true;
    }

    _capture_requested.store(false);
    _codec_released.store(false);
    _capture_state.store(CaptureState::Error);
    ESP_UTILS_LOGE("Release audio codec failed: %s", esp_err_to_name(result));
    return false;
}

void SpecAnalyzer::destroyUi(void)
{
    if (_timer) {
        lv_timer_del(_timer);
        _timer = nullptr;
    }
    if (_mic_label) {
        lv_obj_del(_mic_label);
        _mic_label = nullptr;
    }
    if (_canvas) {
        lv_obj_del(_canvas);
        _canvas = nullptr;
    }
    if (_draw_buf) {
        heap_caps_free(_draw_buf);
        _draw_buf = nullptr;
    }
}

bool SpecAnalyzer::ensureAudioTask(void)
{
    if (_audio_task_handle.load() != nullptr) {
        return !_worker_exit.load() && !_worker_stopped.load();
    }
    if (_worker_exit.load() || !_worker_stopped.load()) {
        return false;
    }

    TaskHandle_t task = nullptr;
    _worker_stopped.store(false);
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        audio_fft_task,
        "audio_fft",
        8 * 1024,
        this,
        5,
        &task,
        PRO_CPU_NUM
    );
    if (task_ret != pdPASS) {
        _worker_stopped.store(true);
        ESP_UTILS_LOGE("Audio FFT task creation failed");
        return false;
    }

    _audio_task_handle.store(task);
    return true;
}

bool SpecAnalyzer::run(void)
{
    ESP_UTILS_LOGD("Run");

    if (_worker_exit.load() || !_codec_released.load() ||
        _codec_release_result.load() != ESP_OK) {
        ESP_UTILS_LOGE("Audio lifecycle is not ready for a new capture session");
        return false;
    }

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    // Prefer PSRAM for the large canvas buffer and fall back to SRAM if PSRAM is exhausted.
    // LVGL 9's lv_color_t is RGB888, so an RGB565 canvas must explicitly use uint16_t pixels.
    constexpr size_t pixel_count = CANVAS_WIDTH * CANVAS_HEIGHT;
    constexpr size_t buf_size = pixel_count * sizeof(uint16_t);
    _draw_buf = static_cast<uint16_t *>(
        heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );

    if (!_draw_buf) {
        ESP_UTILS_LOGE("PSRAM allocation failed! Using fallback SRAM");
        _draw_buf = static_cast<uint16_t *>(heap_caps_malloc(buf_size, MALLOC_CAP_8BIT));
    }

    if (!_draw_buf) {
        ESP_UTILS_LOGE("Canvas buffer allocation failed!");
        return false;
    }
    std::fill_n(_draw_buf, pixel_count, lv_color_to_u16(lv_color_black()));
    std::memset(_peak, 0, sizeof(_peak));

    // The GUI timer directly rasterizes into this RGB565 buffer. This avoids creating
    // hundreds of LVGL draw tasks per frame and waiting for them in the timer callback.
    _canvas = lv_canvas_create(lv_scr_act());
    if (!_canvas) {
        ESP_UTILS_LOGE("Canvas creation failed!");
        heap_caps_free(_draw_buf);
        _draw_buf = nullptr;
        return false;
    }
    lv_obj_set_size(_canvas, CANVAS_WIDTH, CANVAS_HEIGHT);
    lv_obj_align(_canvas, LV_ALIGN_CENTER, 0, 0);
    lv_canvas_set_buffer(_canvas, _draw_buf, CANVAS_WIDTH, CANVAS_HEIGHT, LV_COLOR_FORMAT_RGB565);

    for (int ch = 0; ch < MIC_COUNT; ++ch) {
        const float hue_base = (ch == 0) ? 190.0f : 300.0f;
        const float hue_step = 80.0f / STRIPE_COUNT;
        for (int i = 0; i < STRIPE_COUNT; ++i) {
            const uint16_t hue = static_cast<uint16_t>(hue_base + i * hue_step) % 360;
            _bar_colors[ch][i][0] = lv_color_to_u16(lv_color_hsv_to_rgb(hue, 100, 100));
            _bar_colors[ch][i][1] = lv_color_to_u16(lv_color_hsv_to_rgb(hue, 80, 100));
            _bar_colors[ch][i][2] = lv_color_to_u16(lv_color_hsv_to_rgb(hue, 100, 70));
            _peak_colors[ch][i] = lv_color_to_u16(
                lv_color_hsv_to_rgb(static_cast<uint16_t>((hue + 20) % 360), 100, 100)
            );
        }
    }

    _mic_label = lv_label_create(scr);
    lv_label_set_text(_mic_label, "Mic starting...");
    lv_obj_set_style_text_color(_mic_label, lv_color_hex(0x00BFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_mic_label, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_mic_label, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_pad_all(_mic_label, 6, LV_PART_MAIN);
    lv_obj_align_to(_mic_label, _canvas, LV_ALIGN_TOP_MID, 0, 12);

    // One 1024-sample capture at 16 kHz arrives about every 64 ms, so refreshing
    // faster only redraws duplicate data and starves the LVGL event loop.
    _timer = lv_timer_create(timer_cb, SPECTRUM_REFRESH_PERIOD_MS, this);
    if (!_timer) {
        ESP_UTILS_LOGE("Spectrum timer creation failed!");
        close();
        return false;
    }

    // DSP and codec setup can allocate memory, generate tables and reopen I2S.
    // Keep all of it off the LVGL event thread so opening the app remains responsive.
    if (!ensureAudioTask()) {
        close();
        return false;
    }
    _shown_capture_state = CaptureState::Starting;
    _capture_state.store(CaptureState::Starting);
    _capture_requested.store(true);
    xTaskNotifyGive(_audio_task_handle.load());

    return true;
}

bool SpecAnalyzer::back(void)
{
    ESP_UTILS_LOGD("Back");
    notifyCoreClosed();
    return true;
}

bool SpecAnalyzer::close(void)
{
    ESP_UTILS_LOGD("Close");

    if (!pause()) {
        return false;
    }
    destroyUi();

    return true;
}

bool SpecAnalyzer::init()
{
    if (!_worker_stopped.load() || _audio_task_handle.load() != nullptr) {
        ESP_UTILS_LOGE("Cannot initialize while the previous audio worker is still active");
        return false;
    }
    if (!_codec_released.load() || _codec_release_result.load() != ESP_OK) {
        ESP_UTILS_LOGE("Cannot initialize because the previous codec release failed");
        return false;
    }

    _capture_requested.store(false);
    _worker_exit.store(false);
    _capture_state.store(CaptureState::Idle);
    _shown_capture_state = CaptureState::Idle;
    return true;
}

bool SpecAnalyzer::deinit()
{
    ESP_UTILS_LOGD("Deinit");

    const bool close_ok = close();
    const bool worker_ok = stopAudioTask(pdMS_TO_TICKS(WORKER_JOIN_TIMEOUT_MS));

    // An uninstall must not leave LVGL resources pointing at this app even if
    // the codec reports a release failure. The worker never accesses the UI.
    destroyUi();

    const bool codec_ok = _codec_released.load() && _codec_release_result.load() == ESP_OK;
    if (!close_ok && worker_ok && codec_ok) {
        ESP_UTILS_LOGW("Initial close timed out, but deinit completed the codec hand-off");
    }
    return worker_ok && codec_ok;
}
bool SpecAnalyzer::pause()
{
    if (_timer) {
        lv_timer_pause(_timer);
    }

    CaptureState state = _capture_state.load();
    _capture_requested.store(false);
    if (state == CaptureState::Starting || state == CaptureState::Running) {
        _capture_state.store(CaptureState::Stopping);
    }

    TaskHandle_t task = _audio_task_handle.load();
    if (task) {
        xTaskNotifyGive(task);
    }

    // Reads are split into 256-frame chunks, so the worker normally observes the
    // request in about 16 ms. A terminal state alone is insufficient: the codec
    // close result is the hand-off acknowledgement for the next audio owner.
    auto codec_release_confirmed = [this]() {
        const CaptureState current_state = _capture_state.load();
        const bool terminal = current_state == CaptureState::Idle ||
                              current_state == CaptureState::Error;
        return terminal && _codec_released.load() &&
               _codec_release_result.load() == ESP_OK;
    };

    bool stopped = codec_release_confirmed();
    for (int i = 0; !stopped && i < 50; ++i) {
        vTaskDelay(pdMS_TO_TICKS(5));
        stopped = codec_release_confirmed();
    }

    if (!stopped) {
        ESP_UTILS_LOGE(
            "Microphone codec release was not acknowledged: %s",
            esp_err_to_name(_codec_release_result.load())
        );
        if (_mic_label) {
            lv_label_set_text(_mic_label, "Mic release failed");
        }
        return false;
    }

    if (_mic_label) {
        lv_label_set_text(_mic_label, "Mic paused");
    }
    _shown_capture_state = CaptureState::Idle;
    return true;
}

bool SpecAnalyzer::resume()
{
    if (_capture_requested.load()) {
        if (_worker_exit.load() || _worker_stopped.load() ||
            _audio_task_handle.load() == nullptr) {
            ESP_UTILS_LOGE("Capture is requested without a live audio worker");
            return false;
        }
        if (_timer) {
            lv_timer_resume(_timer);
        }
        return true;
    }

    if (_worker_exit.load() || !_codec_released.load() ||
        _codec_release_result.load() != ESP_OK) {
        ESP_UTILS_LOGE("Cannot resume before the previous codec release succeeds");
        if (_mic_label) {
            lv_label_set_text(_mic_label, "Mic release failed");
        }
        return false;
    }

    if (!ensureAudioTask()) {
        if (_mic_label) {
            lv_label_set_text(_mic_label, "Mic unavailable");
        }
        return false;
    }

    _shown_capture_state = CaptureState::Starting;
    _capture_state.store(CaptureState::Starting);
    _capture_requested.store(true);
    xTaskNotifyGive(_audio_task_handle.load());
    if (_mic_label) {
        lv_label_set_text(_mic_label, "Mic starting...");
    }
    if (_timer) {
        lv_timer_resume(_timer);
    }
    return true;
}

void SpecAnalyzer::audio_fft_task(void *pvParameters)
{
    SpecAnalyzer *app = static_cast<SpecAnalyzer*>(pvParameters);
    constexpr size_t expected_bytes = N_SAMPLES * CAPTURE_CHANNELS * sizeof(int16_t);
    constexpr size_t read_frames = 256;
    constexpr size_t read_chunk_bytes = read_frames * CAPTURE_CHANNELS * sizeof(int16_t);
    constexpr size_t read_chunk_count = N_SAMPLES / read_frames;
    static_assert(N_SAMPLES % read_frames == 0, "FFT frame must contain whole I2S read chunks");
    constexpr size_t mic_channel_indices[MIC_COUNT] = {
        MIC1_CHANNEL_INDEX,
        MIC2_CHANNEL_INDEX,
    };
    bool dsp_ready = false;

    while (!app->_worker_exit.load()) {
        while (!app->_capture_requested.load() && !app->_worker_exit.load()) {
            const CaptureState state = app->_capture_state.load();
            if (state == CaptureState::Starting || state == CaptureState::Stopping) {
                app->_capture_state.store(CaptureState::Idle);
            }
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
        if (app->_worker_exit.load()) {
            break;
        }

        app->_capture_state.store(CaptureState::Starting);

        if (!dsp_ready) {
            // On ESP32-S3 a 1024-point table uses ESP-DSP's built-in table. Asking
            // for CONFIG_DSP_MAX_FFT_SIZE (4096 here) needlessly allocates and builds
            // a much larger table while the application is opening.
            esp_err_t ret = dsps_fft2r_init_fc32(nullptr, N_SAMPLES);
            if (ret != ESP_OK || dsps_fft_w_table_size < N_SAMPLES) {
                ESP_UTILS_LOGE(
                    "FFT init failed: ret=%s, table=%d",
                    esp_err_to_name(ret),
                    dsps_fft_w_table_size
                );
                app->_capture_requested.store(false);
                app->_codec_release_result.store(ESP_OK);
                app->_codec_released.store(true);
                app->_capture_state.store(CaptureState::Error);
                continue;
            }
            dsps_wind_hann_f32(app->_wind, N_SAMPLES);
            dsp_ready = true;
            ESP_UTILS_LOGI("1024-point FFT and Hann window initialized");
        }

        if (!app->_capture_requested.load()) {
            app->_capture_state.store(CaptureState::Idle);
            continue;
        }

        // From this point until releaseCodec() succeeds, pause/close must not
        // report that the shared codec is available to another application.
        app->_codec_released.store(false);
        app->_codec_release_result.store(ESP_ERR_INVALID_STATE);

        esp_err_t ret =
            bsp_extra_audio_session_acquire(BSP_EXTRA_AUDIO_OWNER_SPEC_ANALYZER);
        if (ret == ESP_OK) {
            app->_audio_session_acquired.store(true);
        }
        if (ret == ESP_OK) {
            ret = bsp_extra_codec_init();
        }
        if (ret == ESP_OK && app->_capture_requested.load()) {
            ret = bsp_extra_codec_set_voice_fs(
                CODEC_VOICE_SAMPLE_RATE,
                CODEC_DEFAULT_BIT_WIDTH,
                TDM_SLOT_COUNT,
                CAPTURE_TDM_SLOT_MASK,
                MIC_GAIN_MASK
            );
        }

        if (ret != ESP_OK) {
            ESP_UTILS_LOGE("Configure dual TDM microphone capture failed: %s", esp_err_to_name(ret));
            app->_capture_requested.store(false);
            (void)app->releaseCodec(CaptureState::Error);
            continue;
        }

        if (!app->_capture_requested.load()) {
            (void)app->releaseCodec(CaptureState::Idle);
            continue;
        }

        std::memset(app->_display_spectrum, 0, sizeof(app->_display_spectrum));
        std::memset(app->_smooth_spectrum, 0, sizeof(app->_smooth_spectrum));
        portENTER_CRITICAL(&app->_spectrum_mux);
        std::memset(app->_published_spectrum, 0, sizeof(app->_published_spectrum));
        portEXIT_CRITICAL(&app->_spectrum_mux);

        app->_capture_state.store(CaptureState::Running);
        TickType_t last_slot_log_tick = xTaskGetTickCount();
        bool capture_failed = false;

        while (app->_capture_requested.load() && !app->_worker_exit.load()) {
            size_t bytes_read = 0;
            bool frame_read_failed = false;
            for (size_t chunk = 0; chunk < read_chunk_count; ++chunk) {
                size_t chunk_bytes_read = 0;
                ret = bsp_extra_i2s_read(
                    app->_raw_data + chunk * read_frames * CAPTURE_CHANNELS,
                    read_chunk_bytes,
                    &chunk_bytes_read,
                    portMAX_DELAY
                );
                bytes_read += chunk_bytes_read;

                // Short chunks bound normal pause latency to about 16 ms, even
                // though the current BSP adapter ignores its timeout argument.
                if (!app->_capture_requested.load() || app->_worker_exit.load()) {
                    break;
                }
                if (ret != ESP_OK || chunk_bytes_read < read_chunk_bytes) {
                    if (ret == ESP_ERR_INVALID_STATE) {
                        ESP_UTILS_LOGW("I2S recorder became unavailable");
                        capture_failed = true;
                    } else {
                        ESP_UTILS_LOGW(
                            "I2S read error: %s, bytes: %u",
                            esp_err_to_name(ret),
                            static_cast<unsigned>(chunk_bytes_read)
                        );
                    }
                    frame_read_failed = true;
                    break;
                }
            }

            if (!app->_capture_requested.load() || app->_worker_exit.load()) {
                break;
            }
            if (capture_failed) {
                break;
            }
            if (frame_read_failed || bytes_read < expected_bytes) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }

            TickType_t now = xTaskGetTickCount();
            if ((now - last_slot_log_tick) >= pdMS_TO_TICKS(2000)) {
                int32_t channel_peaks[CAPTURE_CHANNELS] = {};
                for (size_t frame = 0; frame < N_SAMPLES; ++frame) {
                    for (size_t channel = 0; channel < CAPTURE_CHANNELS; ++channel) {
                        int32_t sample = static_cast<int32_t>(
                            app->_raw_data[frame * CAPTURE_CHANNELS + channel]
                        );
                        int32_t magnitude = sample < 0 ? -sample : sample;
                        if (magnitude > channel_peaks[channel]) {
                            channel_peaks[channel] = magnitude;
                        }
                    }
                }
                ESP_UTILS_LOGI(
                    "TDM peaks [MIC1, MIC3(ref), MIC2, MIC4]: %d, %d, %d, %d",
                    static_cast<int>(channel_peaks[0]),
                    static_cast<int>(channel_peaks[1]),
                    static_cast<int>(channel_peaks[2]),
                    static_cast<int>(channel_peaks[3])
                );
                last_slot_log_tick = now;
            }

            const size_t samples_read = bytes_read / sizeof(int16_t);
            for (int ch = 0; ch < MIC_COUNT; ++ch) {
                for (int i = 0; i < N_SAMPLES; ++i) {
                    const size_t sample_index = i * CAPTURE_CHANNELS + mic_channel_indices[ch];
                    app->_audio_buffer[ch][i] = (sample_index < samples_read)
                        ? (app->_raw_data[sample_index] / 32768.0f)
                        : 0.0f;
                }
            }

            for (int ch = 0; ch < MIC_COUNT; ++ch) {
                dsps_mul_f32(
                    app->_audio_buffer[ch],
                    app->_wind,
                    app->_audio_buffer[ch],
                    N_SAMPLES,
                    1,
                    1,
                    1
                );

                for (int i = 0; i < N_SAMPLES; ++i) {
                    app->_fft_buffer[ch][2 * i] = app->_audio_buffer[ch][i];
                    app->_fft_buffer[ch][2 * i + 1] = 0.0f;
                }

                dsps_fft2r_fc32(app->_fft_buffer[ch], N_SAMPLES);
                dsps_bit_rev_fc32(app->_fft_buffer[ch], N_SAMPLES);

                for (int i = 0; i < N_SAMPLES / 2; ++i) {
                    const float real = app->_fft_buffer[ch][2 * i];
                    const float imag = app->_fft_buffer[ch][2 * i + 1];
                    const float magnitude = sqrtf(real * real + imag * imag);
                    app->_spectrum[ch][i] = 20.0f * log10f(magnitude / (N_SAMPLES / 2) + 1e-9f);
                }

                for (int i = 0; i < STRIPE_COUNT; ++i) {
                    const int fft_idx = i * (N_SAMPLES / 2) / STRIPE_COUNT;
                    const float boosted_db = app->_spectrum[ch][fft_idx] + 10.0f;
                    app->_display_spectrum[ch][i] = fmaxf(-90.0f, fminf(0.0f, boosted_db));

                    constexpr float smooth_factor = 0.2f;
                    app->_smooth_spectrum[ch][i] =
                        app->_smooth_spectrum[ch][i] * (1.0f - smooth_factor) +
                        app->_display_spectrum[ch][i] * smooth_factor;
                }
            }

            portENTER_CRITICAL(&app->_spectrum_mux);
            std::memcpy(
                app->_published_spectrum,
                app->_smooth_spectrum,
                sizeof(app->_published_spectrum)
            );
            portEXIT_CRITICAL(&app->_spectrum_mux);
            taskYIELD();
        }

        // Only this worker opens, reads and closes the shared codec. Publishing the
        // terminal state after stop lets pause() safely hand it to MusicPlayer.
        const bool codec_released = app->releaseCodec(
            capture_failed ? CaptureState::Error : CaptureState::Idle
        );
        if (app->_worker_exit.load()) {
            break;
        }
        if (capture_failed) {
            app->_capture_requested.store(false);
        }
        if (!codec_released) {
            app->_capture_requested.store(false);
        }
    }

    if (!app->_codec_released.load()) {
        (void)app->releaseCodec(CaptureState::Idle);
    } else if (app->_codec_release_result.load() == ESP_OK) {
        app->_capture_state.store(CaptureState::Idle);
    }
    app->_audio_task_handle.store(nullptr);
    app->_worker_stopped.store(true);
    vTaskDelete(NULL);
}

// LVGL timer callback. It renders the smoothed FFT bins and peak markers into the canvas.
void SpecAnalyzer::timer_cb(lv_timer_t *timer)
{
    SpecAnalyzer *app = static_cast<SpecAnalyzer*>(lv_timer_get_user_data(timer));
    if (!app || !app->_canvas || !app->_draw_buf) {
        return;
    }

    const CaptureState state = app->_capture_state.load();
    if (state != app->_shown_capture_state) {
        const char *status = "Mic unavailable";
        switch (state) {
        case CaptureState::Idle:
            status = "Mic paused";
            break;
        case CaptureState::Starting:
            status = "Mic starting...";
            break;
        case CaptureState::Running:
            status = "Mic 1 / Mic 2";
            break;
        case CaptureState::Stopping:
            status = "Mic stopping...";
            break;
        case CaptureState::Error:
            status = "Mic unavailable";
            break;
        }
        if (app->_mic_label) {
            lv_label_set_text(app->_mic_label, status);
        }
        app->_shown_capture_state = state;
    }

    portENTER_CRITICAL(&app->_spectrum_mux);
    std::memcpy(
        app->_render_spectrum,
        app->_published_spectrum,
        sizeof(app->_render_spectrum)
    );
    portEXIT_CRITICAL(&app->_spectrum_mux);

    std::fill_n(
        app->_draw_buf,
        static_cast<size_t>(app->CANVAS_WIDTH) * app->CANVAS_HEIGHT,
        lv_color_to_u16(lv_color_black())
    );

    const int side_margin = 20;
    const int channel_gap = 24;
    const int channel_width = (app->CANVAS_WIDTH - side_margin * 2 - channel_gap) / app->MIC_COUNT;
    const int bar_gap = 2;
    int bar_width = channel_width / app->STRIPE_COUNT - bar_gap;
    if (bar_width < 1) {
        bar_width = 1;
    }
    const int bars_width = app->STRIPE_COUNT * bar_width + (app->STRIPE_COUNT - 1) * bar_gap;
    const int base_y = app->CANVAS_HEIGHT - 20;                  // Bottom margin
    const int max_bar_height = app->CANVAS_HEIGHT - 50;
    const int BASE_BAR_HEIGHT = 5;

    for (int ch = 0; ch < app->MIC_COUNT; ++ch) {
        const int channel_offset_x = side_margin + ch * (channel_width + channel_gap) +
                                     (channel_width - bars_width) / 2;

        for (int i = 0; i < app->STRIPE_COUNT; ++i) {
            // Normalize smoothed dB values from [-90, 0] to [0, 1].
            const float db = app->_render_spectrum[ch][i];
            float norm = (db + 90.0f) / 90.0f;
            norm = fmaxf(0.0f, fminf(1.0f, norm));

            // Nonlinear mapping keeps quiet audio visible while preserving peaks.
            int bar_height = (int)(powf(norm, 0.8) * max_bar_height);
            if (bar_height < BASE_BAR_HEIGHT) {
                bar_height = BASE_BAR_HEIGHT;
            }

            // Peak markers rise immediately and fall faster when they are higher,
            // which gives the analyzer a natural decay without another timer.
            if (app->_peak[ch][i] < bar_height) {
                app->_peak[ch][i] = bar_height;
            } else {
                const float peak_norm = app->_peak[ch][i] / static_cast<float>(max_bar_height);
                const float fall_speed = 0.32f + (peak_norm * 0.8f);
                app->_peak[ch][i] -= fall_speed;

                if (app->_peak[ch][i] < BASE_BAR_HEIGHT) {
                    app->_peak[ch][i] = BASE_BAR_HEIGHT;
                }
            }

            const int bar_x1 = channel_offset_x + i * (bar_width + bar_gap);
            const int bar_x2 = bar_x1 + bar_width - 1;
            const int bar_y1 = base_y - bar_height;
            const int top_height = std::max(1, bar_height / 5);

            fill_rgb565_rect(
                app->_draw_buf,
                app->CANVAS_WIDTH,
                app->CANVAS_HEIGHT,
                bar_x1,
                bar_y1 + top_height,
                bar_x2,
                base_y,
                app->_bar_colors[ch][i][0]
            );
            fill_rgb565_rect(
                app->_draw_buf,
                app->CANVAS_WIDTH,
                app->CANVAS_HEIGHT,
                bar_x1,
                bar_y1,
                bar_x2,
                bar_y1 + top_height - 1,
                app->_bar_colors[ch][i][1]
            );
            fill_rgb565_rect(
                app->_draw_buf,
                app->CANVAS_WIDTH,
                app->CANVAS_HEIGHT,
                bar_x1,
                base_y - 1,
                bar_x2,
                base_y,
                app->_bar_colors[ch][i][2]
            );

            const int peak_y = base_y - static_cast<int>(app->_peak[ch][i]);
            fill_rgb565_rect(
                app->_draw_buf,
                app->CANVAS_WIDTH,
                app->CANVAS_HEIGHT,
                bar_x1,
                peak_y - 2,
                bar_x2,
                peak_y,
                app->_peak_colors[ch][i]
            );
        }
    }

    lv_obj_invalidate(app->_canvas);
}

} // namespace esp_brookesia::apps
