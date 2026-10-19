#pragma once

#include <atomic>

#include "systems/phone/esp_brookesia_phone_app.hpp"
#include "esp_dsp.h"
#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esp_brookesia::apps
{
    class SpecAnalyzer : public systems::phone::App
    {
    public:
        static SpecAnalyzer *requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);
        ~SpecAnalyzer();

        using systems::phone::App::endRecordResource;
        using systems::phone::App::startRecordResource;

    protected:
        SpecAnalyzer(bool use_status_bar, bool use_navigation_bar);

        bool run(void) override;
        bool back(void) override;
        bool close(void) override;
        bool init(void) override;
        bool deinit(void) override;
        bool pause(void) override;
        bool resume(void) override;

    private:
        static SpecAnalyzer *_instance;

        enum class CaptureState : uint8_t {
            Idle,
            Starting,
            Running,
            Stopping,
            Error,
        };

        static constexpr uint16_t N_SAMPLES = 1024;
        // ES7210 serializes [MIC1, MIC3(reference), MIC2, MIC4]. Preserve the
        // complete frame, then select the two front microphones for the FFT.
        static constexpr uint16_t TDM_SLOT_COUNT = CODEC_VOICE_INPUT_CHANNELS;
        static constexpr uint16_t CAPTURE_TDM_SLOT_MASK =
            BSP_EXTRA_ES7210_TDM_ALL_SLOTS_MASK;
        static constexpr uint16_t CAPTURE_CHANNELS = TDM_SLOT_COUNT;
        static constexpr uint16_t MIC_COUNT = 2;
        static constexpr uint16_t MIC1_CHANNEL_INDEX = 0;
        static constexpr uint16_t MIC2_CHANNEL_INDEX = 2;
        static constexpr uint16_t MIC_GAIN_MASK =
            BSP_EXTRA_ES7210_PHYSICAL_FRONT_MIC_MASK;
        static constexpr uint16_t STRIPE_COUNT = 48;
        // Scale the analyzer to the target panel while keeping enough margin
        // for Brookesia's status and navigation bars.
        static constexpr uint16_t CANVAS_WIDTH = BSP_LCD_H_RES * 70 / 100;
        static constexpr uint16_t CANVAS_HEIGHT = (BSP_LCD_V_RES >= 720) ? 360 : (BSP_LCD_V_RES / 2);

        lv_obj_t *_canvas;
        lv_obj_t *_mic_label;
        lv_timer_t *_timer;
        std::atomic<TaskHandle_t> _audio_task_handle;
        std::atomic<bool> _capture_requested;
        std::atomic<bool> _worker_exit;
        std::atomic<bool> _worker_stopped;
        std::atomic<bool> _codec_released;
        std::atomic<esp_err_t> _codec_release_result;
        std::atomic<bool> _audio_session_acquired;
        std::atomic<CaptureState> _capture_state;
        CaptureState _shown_capture_state;
        portMUX_TYPE _spectrum_mux;

        // Dual-microphone visualization buffers.
        __attribute__((aligned(16))) int16_t _raw_data[N_SAMPLES * CAPTURE_CHANNELS]; // Packed microphone samples
        __attribute__((aligned(16))) float _audio_buffer[MIC_COUNT][N_SAMPLES];   // Normalized microphone samples
        __attribute__((aligned(16))) float _wind[N_SAMPLES];                      // Shared Hann window
        __attribute__((aligned(16))) float _fft_buffer[MIC_COUNT][N_SAMPLES * 2]; // Complex FFT input
        __attribute__((aligned(16))) float _spectrum[MIC_COUNT][N_SAMPLES / 2];   // Spectrum in dB
        float _display_spectrum[MIC_COUNT][STRIPE_COUNT];                         // Mapped spectrum for bars
        float _peak[MIC_COUNT][STRIPE_COUNT];                                     // Peak marker position
        float _smooth_spectrum[MIC_COUNT][STRIPE_COUNT];                          // Audio-task-owned smoothing state
        float _published_spectrum[MIC_COUNT][STRIPE_COUNT];                       // Protected cross-task snapshot
        float _render_spectrum[MIC_COUNT][STRIPE_COUNT];                          // LVGL-task-owned render snapshot

        uint16_t _bar_colors[MIC_COUNT][STRIPE_COUNT][3];
        uint16_t _peak_colors[MIC_COUNT][STRIPE_COUNT];
        uint16_t *_draw_buf; // PSRAM-preferred RGB565 canvas draw buffer

        bool ensureAudioTask(void);
        bool stopAudioTask(TickType_t timeout_ticks);
        bool releaseCodec(CaptureState success_state);
        void destroyUi(void);
        static void audio_fft_task(void *pvParameters);
        static void timer_cb(lv_timer_t *timer);
    };
}
