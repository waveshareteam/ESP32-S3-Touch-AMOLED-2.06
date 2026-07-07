#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_memory_utils.h"

#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"
#include "lv_demos.h"
#include "esp_jpeg_dec.h"
#include "avi_player.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "main";

#define DISP_WIDTH 320
#define DISP_HEIGHT 200

static lv_obj_t *canvas = NULL;
static lv_color_t *canvas_buf[2] = {NULL};
static int current_buf_idx = 0;
static bool loop_playback = true;
static bool is_playing = false;

static jpeg_dec_handle_t jpeg_handle = NULL;

static char **avi_file_list = NULL;
static int avi_file_count = 0;

#define LVGL_PORT_INIT_CONFIG()   \
    {                             \
        .task_priority = 4,       \
        .task_stack = 10 * 1024,  \
        .task_affinity = -1,      \
        .task_max_sleep_ms = 500, \
        .timer_period_ms = 5,     \
    }

static esp_err_t get_avi_file_list(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", dir_path);
        return ESP_FAIL;
    }

    struct dirent *entry;
    int count = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            char *ext = strrchr(entry->d_name, '.');
            if (ext && (strcasecmp(ext, ".avi") == 0)) {
                count++;
            }
        }
    }

    if (count == 0) {
        closedir(dir);
        ESP_LOGW(TAG, "No AVI files found in directory %s", dir_path);
        return ESP_FAIL;
    }

    avi_file_list = (char **)malloc(sizeof(char *) * count);
    if (!avi_file_list) {
        closedir(dir);
        ESP_LOGE(TAG, "Failed to allocate memory for file list");
        return ESP_ERR_NO_MEM;
    }
    avi_file_count = count;
    count = 0;

    rewinddir(dir);
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            char *ext = strrchr(entry->d_name, '.');
            if (ext && (strcasecmp(ext, ".avi") == 0)) {
                size_t dir_len = strlen(dir_path);
                size_t file_len = strlen(entry->d_name);
                char *full_path = (char *)malloc(dir_len + file_len + 2);
                if (!full_path) {
                    ESP_LOGE(TAG, "Failed to allocate memory for file path");
                    for (int i = 0; i < count; i++) {
                        free(avi_file_list[i]);
                    }
                    free(avi_file_list);
                    avi_file_list = NULL;
                    avi_file_count = 0;
                    closedir(dir);
                    return ESP_ERR_NO_MEM;
                }

                if (dir_len > 0 && dir_path[dir_len - 1] == '/') {
                    sprintf(full_path, "%s%s", dir_path, entry->d_name);
                } else {
                    sprintf(full_path, "%s/%s", dir_path, entry->d_name);
                }

                avi_file_list[count++] = full_path;
            }
        }
    }

    closedir(dir);
    ESP_LOGI(TAG, "Found %d AVI files in directory %s", avi_file_count, dir_path);
    for (int i = 0; i < avi_file_count; i++) {
        ESP_LOGI(TAG, "AVI file %d: %s", i + 1, avi_file_list[i]);
    }

    return ESP_OK;
}

static void init_canvas(void)
{
    if (canvas == NULL) {
        for (int i = 0; i < 2; i++) {
            canvas_buf[i] = (lv_color_t *)jpeg_calloc_align(DISP_WIDTH * DISP_HEIGHT * sizeof(lv_color_t), 16);
            if (!canvas_buf[i]) {
                ESP_LOGE("init_canvas", "Failed to allocate memory for canvas buffer %d", i);
                for (int j = 0; j < i; j++) {
                    if (canvas_buf[j]) {
                        jpeg_free_align(canvas_buf[j]);
                        canvas_buf[j] = NULL;
                    }
                }
                return;
            }
        }

        canvas = lv_canvas_create(lv_scr_act());
        lv_canvas_set_buffer(canvas, canvas_buf[0], DISP_WIDTH, DISP_HEIGHT, LV_COLOR_FORMAT_RGB565);
        lv_obj_center(canvas);
    }
}

static esp_err_t init_jpeg_decoder(void)
{
    if (jpeg_handle != NULL) {
        return ESP_OK;
    }

    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;

    jpeg_error_t err = jpeg_dec_open(&config, &jpeg_handle);
    if (err != JPEG_ERR_OK) {
        ESP_LOGE("init_jpeg_decoder", "JPEG decoder initialization failed: %d", err);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void deinit_jpeg_decoder(void)
{
    if (jpeg_handle != NULL) {
        jpeg_dec_close(jpeg_handle);
        jpeg_handle = NULL;
    }
}

static void video_cb(frame_data_t *data, void *arg)
{
    if (!data || !data->data || data->data_bytes == 0)
        return;

    int next_buf_idx = (current_buf_idx + 1) % 2;

    if (init_jpeg_decoder() != ESP_OK) {
        return;
    }

    jpeg_dec_io_t io = {
        .inbuf = data->data,
        .inbuf_len = data->data_bytes,
        .outbuf = (uint8_t *)canvas_buf[next_buf_idx],
    };

    jpeg_dec_header_info_t header_info;
    jpeg_error_t err = jpeg_dec_parse_header(jpeg_handle, &io, &header_info);
    if (err != JPEG_ERR_OK) {
        ESP_LOGE("video_cb", "JPEG header parsing failed: %d", err);
        return;
    }

    int outbuf_len = 0;
    err = jpeg_dec_get_outbuf_len(jpeg_handle, &outbuf_len);
    if (err != JPEG_ERR_OK) {
        ESP_LOGE("video_cb", "Failed to get output buffer length: %d", err);
        return;
    }

    if (outbuf_len > DISP_WIDTH * DISP_HEIGHT * 2) {
        ESP_LOGE("video_cb", "Output buffer too small. Required %d bytes, available %d bytes",
                 outbuf_len, DISP_WIDTH * DISP_HEIGHT * 2);
        return;
    }

    err = jpeg_dec_process(jpeg_handle, &io);
    if (err != JPEG_ERR_OK) {
        ESP_LOGE("video_cb", "JPEG decoding failed: %d", err);
        return;
    }

    bsp_display_lock(0);
    if (canvas == NULL) {
        init_canvas();
    }

    lv_canvas_set_buffer(canvas, canvas_buf[next_buf_idx], DISP_WIDTH, DISP_HEIGHT, LV_COLOR_FORMAT_RGB565);
    current_buf_idx = next_buf_idx;
    lv_obj_invalidate(canvas);

    bsp_display_unlock();
}

static void audio_cb(frame_data_t *data, void *arg)
{
    if (data && data->type == FRAME_TYPE_AUDIO && data->data && data->data_bytes > 0) {
        size_t bytes_written = 0;
        esp_err_t err = bsp_extra_i2s_write(data->data, data->data_bytes, &bytes_written, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Audio write failed: %s", esp_err_to_name(err));
        } else if (bytes_written != data->data_bytes) {
            ESP_LOGW(TAG, "Incomplete audio data (wrote %d/%d bytes)", bytes_written, data->data_bytes);
        }
    }
}

static void audio_set_clock_callback(uint32_t rate, uint32_t bits_cfg, uint32_t ch, void *arg)
{
    if (rate == 0) {
        rate = CODEC_DEFAULT_SAMPLE_RATE;
        ESP_LOGW(TAG, "Using default sample rate: %u", rate);
    }
    if (bits_cfg == 0) {
        bits_cfg = CODEC_DEFAULT_BIT_WIDTH;
        ESP_LOGW(TAG, "Using default bit width: %u", bits_cfg);
    }

    ESP_LOGI(TAG, "Setting I2S clock: sample rate=%u, bit width=%u, channels=%u", rate, bits_cfg, ch);
    i2s_slot_mode_t slot_mode = (ch == 2) ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO;
    esp_err_t err = bsp_extra_codec_set_fs(rate, bits_cfg, slot_mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set codec parameters: %s", esp_err_to_name(err));
    }
}

static void avi_end_cb(void *arg)
{
    ESP_LOGI(TAG, "AVI playback finished");
    is_playing = false;
}

static void avi_play_task(void *arg)
{
    avi_player_handle_t handle;
    avi_player_config_t cfg = {
        .buffer_size = 256 * 1024,
        .video_cb = video_cb,
        .audio_cb = audio_cb,
        .audio_set_clock_cb = audio_set_clock_callback,
        .avi_play_end_cb = avi_end_cb,
        .priority = 7,
        .coreID = 0,
        .user_data = NULL,
        .stack_size = 12 * 1024,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
        .stack_in_psram = false,
#endif
    };

    bsp_display_lock(0);
    init_canvas();
    bsp_display_unlock();

    ESP_ERROR_CHECK(avi_player_init(cfg, &handle));

    uint32_t frame_count = 0;
    uint32_t last_time = xTaskGetTickCount();
    int current_file_index = 0;

    while (loop_playback) {
        ESP_LOGI(TAG, "Starting file list playback");
        
        for (current_file_index = 0; current_file_index < avi_file_count && loop_playback; current_file_index++) {
            const char *current_file = avi_file_list[current_file_index];
            ESP_LOGI(TAG, "Playing: %s (%d/%d)", current_file, current_file_index + 1, avi_file_count);
            
            is_playing = true;
            frame_count = 0;
            last_time = xTaskGetTickCount();

            esp_err_t err = avi_player_play_from_file(handle, current_file);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to play file: %s, error: %s", current_file, esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }

            while (is_playing && loop_playback) {
                vTaskDelay(pdMS_TO_TICKS(30));

                frame_count++;
                if (frame_count % 100 == 0) {
                    uint32_t current_time = xTaskGetTickCount();
                    float fps = 100.0f / ((current_time - last_time) / 1000.0f);
                    ESP_LOGI(TAG, "Frame rate: %.2f FPS", fps);
                    last_time = current_time;
                }
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    avi_player_play_stop(handle);
    avi_player_deinit(handle);
    deinit_jpeg_decoder();

    bsp_display_lock(0);
    for (int i = 0; i < 2; i++) {
        if (canvas_buf[i]) {
            jpeg_free_align(canvas_buf[i]);
            canvas_buf[i] = NULL;
        }
    }
    if (canvas) {
        lv_obj_del(canvas);
        canvas = NULL;
    }
    bsp_display_unlock();

    if (avi_file_list) {
        for (int i = 0; i < avi_file_count; i++) {
            free(avi_file_list[i]);
        }
        free(avi_file_list);
        avi_file_list = NULL;
        avi_file_count = 0;
    }

    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_ERROR_CHECK(bsp_extra_codec_init());
    bsp_extra_codec_volume_set(80, NULL);
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = LVGL_PORT_INIT_CONFIG(),
    };

    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();

    bsp_display_lock(0);
    lv_obj_t *status_label = lv_label_create(lv_scr_act());
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(status_label, DISP_WIDTH - 20);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_20, 0);
    bsp_display_unlock();

    int retry_count = 0;
    esp_err_t mount_err = ESP_FAIL;
    while (1) {
        bsp_display_lock(0);
        lv_label_set_text_fmt(status_label, "Mounting SD card...\nAttempt: %d", retry_count + 1);
        bsp_display_unlock();
        
        mount_err = bsp_sdcard_mount();
        if (mount_err == ESP_OK) {
            break;
        }
        
        ESP_LOGW(TAG, "SD card mount attempt %d failed: %s", retry_count + 1, esp_err_to_name(mount_err));
        retry_count++;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    
    if (mount_err != ESP_OK) {
        bsp_display_lock(0);
        lv_label_set_text(status_label, "SD card error\nCheck and restart");
        lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF0000), 0);
        bsp_display_unlock();
        
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    esp_err_t list_err = get_avi_file_list("/sdcard/avi");
    if (list_err != ESP_OK || avi_file_count == 0) {
        bsp_display_lock(0);
        lv_label_set_text(status_label, "No AVI files found\nin /sdcard/avi");
        lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF0000), 0);
        bsp_display_unlock();
        
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    bsp_display_lock(0);
    lv_obj_del(status_label);
    bsp_display_unlock();

    ESP_LOGI(TAG, "SD card mounted successfully, found %d AVI files", avi_file_count);

    xTaskCreatePinnedToCore(avi_play_task, "avi_play_task", 12288, NULL, 7, NULL, 0);
}