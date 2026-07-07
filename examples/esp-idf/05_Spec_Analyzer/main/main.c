#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_dsp.h"
#include "bsp_board_extra.h"

#define TAG "audio_fft"

#define N_SAMPLES 1024
#define SAMPLE_RATE 16000
#define CHANNELS 2
#define DISPLAY_REFRESH_MS 200
#define STRIPE_COUNT 64

#define CANVAS_WIDTH 410
#define CANVAS_HEIGHT 200

__attribute__((aligned(16))) int16_t raw_data[N_SAMPLES * CHANNELS];
__attribute__((aligned(16))) float audio_buffer[N_SAMPLES];
__attribute__((aligned(16))) float wind[N_SAMPLES];
__attribute__((aligned(16))) float fft_buffer[N_SAMPLES * 2];
__attribute__((aligned(16))) float spectrum[N_SAMPLES / 2];

float display_spectrum[STRIPE_COUNT];
float peak[STRIPE_COUNT];

void audio_fft_task(void *pvParameters)
{
    esp_err_t ret = dsps_fft2r_init_fc32(NULL, CONFIG_DSP_MAX_FFT_SIZE);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "FFT init failed: %d", ret);
        vTaskDelete(NULL);
    }

    dsps_wind_hann_f32(wind, N_SAMPLES);
    ESP_LOGI(TAG, "FFT and window initialized");

    if (bsp_extra_codec_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Audio codec init failed");
        vTaskDelete(NULL);
    }

    TickType_t last_wake_time = xTaskGetTickCount();
    size_t bytes_read;

    while (1)
    {
        ret = bsp_extra_i2s_read(raw_data, N_SAMPLES * CHANNELS * sizeof(int16_t), &bytes_read, portMAX_DELAY);
        if (ret != ESP_OK || bytes_read != N_SAMPLES * CHANNELS * sizeof(int16_t))
        {
            ESP_LOGW(TAG, "I2S read error: %d, bytes: %d", ret, bytes_read);
            continue;
        }

        for (int i = 0; i < N_SAMPLES; i++)
        {
            int16_t left = raw_data[i * CHANNELS];
            int16_t right = raw_data[i * CHANNELS + 1];
            audio_buffer[i] = (left + right) / (2.0f * 32768.0f);
        }

        dsps_mul_f32(audio_buffer, wind, audio_buffer, N_SAMPLES, 1, 1, 1);

        for (int i = 0; i < N_SAMPLES; i++)
        {
            fft_buffer[2 * i] = audio_buffer[i];
            fft_buffer[2 * i + 1] = 0;
        }

        dsps_fft2r_fc32(fft_buffer, N_SAMPLES);
        dsps_bit_rev_fc32(fft_buffer, N_SAMPLES);

        for (int i = 0; i < N_SAMPLES / 2; i++)
        {
            float real = fft_buffer[2 * i];
            float imag = fft_buffer[2 * i + 1];
            float magnitude = sqrtf(real * real + imag * imag);
            spectrum[i] = 20 * log10f(magnitude / (N_SAMPLES / 2) + 1e-9);
        }

        for (int i = 0; i < STRIPE_COUNT; i++)
        {
            int fft_idx = i * (N_SAMPLES / 2) / STRIPE_COUNT;
            display_spectrum[i] = fmaxf(-90.0f, fminf(0.0f, spectrum[fft_idx]));
        }

        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(1));
    }
}

static void timer_cb(lv_timer_t *timer)
{
    lv_obj_t *canvas = (lv_obj_t *)lv_timer_get_user_data(timer);
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_COVER);

    const int stripe_width = CANVAS_WIDTH / STRIPE_COUNT;
    const int center_y = CANVAS_HEIGHT / 2;

    const int bar_gap_px = 2;

    for (int i = 0; i < STRIPE_COUNT; i++) {
         float db = display_spectrum[i];
        float db_min = -90.0f, db_max = 0.0f;

        float norm = (db - db_min) / (db_max - db_min);
        norm = fmaxf(0.0f, fminf(1.0f, norm));
        norm = sqrtf(norm);

        int bar_height = (int)(norm * (CANVAS_HEIGHT / 2));

        if (peak[i] < bar_height) {
            peak[i] = bar_height;
        } else {
            peak[i] -= 2;
            if (peak[i] < 0) peak[i] = 0;
        }

        float hue_step = 270.0f / STRIPE_COUNT;
        uint16_t hue = (uint16_t)(i * hue_step);
        lv_color_t color = lv_color_hsv_to_rgb(hue, 100, 100);

        lv_draw_rect_dsc_t rect_dsc;
        lv_draw_rect_dsc_init(&rect_dsc);
        rect_dsc.bg_color = color;
        rect_dsc.bg_opa = LV_OPA_COVER;

        int x_start = i * stripe_width + bar_gap_px / 2;
        int x_end   = (i + 1) * stripe_width - bar_gap_px / 2 - 1;

        lv_area_t bar_area = {
            .x1 = x_start,
            .y1 = center_y - bar_height,
            .x2 = x_end,
            .y2 = center_y + bar_height
        };
        lv_draw_rect(&layer, &rect_dsc, &bar_area);

        int peak_y_top = center_y - (int)peak[i] - 2;
        int peak_y_bot = center_y + (int)peak[i];

        lv_area_t particle_area_top = {
            .x1 = x_start,
            .y1 = peak_y_top,
            .x2 = x_end,
            .y2 = peak_y_top + 2
        };
        lv_draw_rect(&layer, &rect_dsc, &particle_area_top);

        lv_area_t particle_area_bot = {
            .x1 = x_start,
            .y1 = peak_y_bot,
            .x2 = x_end,
            .y2 = peak_y_bot + 2
        };
        lv_draw_rect(&layer, &rect_dsc, &particle_area_bot);
    }

    lv_canvas_finish_layer(canvas, &layer);
}


void lv_example_canvas_10(void)
{
    LV_DRAW_BUF_DEFINE_STATIC(draw_buf, CANVAS_WIDTH, CANVAS_HEIGHT, LV_COLOR_FORMAT_RGB565);
    LV_DRAW_BUF_INIT_STATIC(draw_buf);

    lv_obj_t *canvas = lv_canvas_create(lv_screen_active());
    lv_obj_set_size(canvas, CANVAS_WIDTH, CANVAS_HEIGHT);
    lv_obj_center(canvas);
    lv_canvas_set_draw_buf(canvas, &draw_buf);

    lv_timer_create(timer_cb, 33, canvas);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting Audio Spectrum Analyzer");

    lv_display_t *disp = bsp_display_start();
    if (disp)
    {
        bsp_display_backlight_on();
    }

    bsp_display_lock(pdMS_TO_TICKS(200));
    lv_example_canvas_10();
    bsp_display_unlock();

    xTaskCreate(audio_fft_task, "audio_fft", 6 * 1024, NULL, 5, NULL);
}
