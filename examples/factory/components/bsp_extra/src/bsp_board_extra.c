/*
 * SPDX-FileCopyrightText: 2015-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"

#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"

static const char *TAG = "bsp_extra_board";

static esp_codec_dev_handle_t play_dev_handle;
static esp_codec_dev_handle_t record_dev_handle;

static bool _is_audio_init = false;
static bool _is_player_init = false;
static int _volume_intensity = CODEC_DEFAULT_VOLUME;
static audio_player_cb_t _audio_callback = NULL;
static void *_audio_callback_user_data = NULL;
static char _audio_file_path[256];
static portMUX_TYPE _audio_session_mux = portMUX_INITIALIZER_UNLOCKED;
static bsp_extra_audio_owner_t _audio_session_owner = BSP_EXTRA_AUDIO_OWNER_NONE;


/**************************************************************************************************
 *
 * Extra Board Function
 *
 **************************************************************************************************/

static void update_result(esp_err_t *result, esp_err_t candidate)
{
    if (*result == ESP_OK && candidate != ESP_OK) {
        *result = candidate;
    }
}

static bool is_valid_audio_owner(bsp_extra_audio_owner_t owner)
{
    return owner > BSP_EXTRA_AUDIO_OWNER_NONE && owner < BSP_EXTRA_AUDIO_OWNER_MAX;
}

const char *bsp_extra_audio_owner_name(bsp_extra_audio_owner_t owner)
{
    switch (owner) {
    case BSP_EXTRA_AUDIO_OWNER_NONE:
        return "None";
    case BSP_EXTRA_AUDIO_OWNER_MUSIC:
        return "MusicPlayer";
    case BSP_EXTRA_AUDIO_OWNER_VIDEO:
        return "VideoPlayer";
    case BSP_EXTRA_AUDIO_OWNER_RECORDER:
        return "Recorder";
    case BSP_EXTRA_AUDIO_OWNER_SPEC_ANALYZER:
        return "SpecAnalyzer";
    case BSP_EXTRA_AUDIO_OWNER_XIAOZHI:
        return "Xiaozhi";
    case BSP_EXTRA_AUDIO_OWNER_MAX:
    default:
        return "Invalid";
    }
}

bsp_extra_audio_owner_t bsp_extra_audio_session_get_owner(void)
{
    bsp_extra_audio_owner_t owner;
    portENTER_CRITICAL(&_audio_session_mux);
    owner = _audio_session_owner;
    portEXIT_CRITICAL(&_audio_session_mux);
    return owner;
}

esp_err_t bsp_extra_audio_session_acquire(bsp_extra_audio_owner_t owner)
{
    if (!is_valid_audio_owner(owner)) {
        ESP_LOGE(TAG, "Invalid audio owner: %d", (int)owner);
        return ESP_ERR_INVALID_ARG;
    }

    bsp_extra_audio_owner_t current_owner;
    portENTER_CRITICAL(&_audio_session_mux);
    current_owner = _audio_session_owner;
    if (current_owner == BSP_EXTRA_AUDIO_OWNER_NONE) {
        _audio_session_owner = owner;
    }
    portEXIT_CRITICAL(&_audio_session_mux);

    if (current_owner != BSP_EXTRA_AUDIO_OWNER_NONE) {
        ESP_LOGW(TAG, "Audio session busy: requested=%s, current=%s",
                 bsp_extra_audio_owner_name(owner),
                 bsp_extra_audio_owner_name(current_owner));
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Audio session acquired by %s", bsp_extra_audio_owner_name(owner));
    return ESP_OK;
}

esp_err_t bsp_extra_audio_session_release(bsp_extra_audio_owner_t owner)
{
    if (!is_valid_audio_owner(owner)) {
        ESP_LOGE(TAG, "Invalid audio owner: %d", (int)owner);
        return ESP_ERR_INVALID_ARG;
    }

    bsp_extra_audio_owner_t current_owner;
    portENTER_CRITICAL(&_audio_session_mux);
    current_owner = _audio_session_owner;
    if (current_owner == owner) {
        _audio_session_owner = BSP_EXTRA_AUDIO_OWNER_NONE;
    }
    portEXIT_CRITICAL(&_audio_session_mux);

    if (current_owner != owner) {
        ESP_LOGE(TAG, "Audio session release rejected: requested=%s, current=%s",
                 bsp_extra_audio_owner_name(owner),
                 bsp_extra_audio_owner_name(current_owner));
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Audio session released by %s", bsp_extra_audio_owner_name(owner));
    return ESP_OK;
}

static esp_err_t audio_mute_function(AUDIO_PLAYER_MUTE_SETTING setting)
{
    const bool mute = setting == AUDIO_PLAYER_MUTE;
    ESP_RETURN_ON_ERROR(bsp_extra_codec_mute_set(mute), TAG, "Set codec mute failed");
    if (!mute) {
        ESP_RETURN_ON_ERROR(
            esp_codec_dev_set_out_vol(play_dev_handle, _volume_intensity),
            TAG,
            "Restore codec volume failed"
        );
    }
    return ESP_OK;
}

static void audio_event_callback(audio_player_cb_ctx_t *ctx)
{
    if (_audio_callback) {
        ctx->user_ctx = _audio_callback_user_data;
        _audio_callback(ctx);
    }
}

static bool is_supported_audio_file(const char *name)
{
    const char *extension = name ? strrchr(name, '.') : NULL;
    return extension &&
           (strcasecmp(extension, ".mp3") == 0 || strcasecmp(extension, ".wav") == 0);
}

static void free_file_instance(file_iterator_instance_t *instance)
{
    if (!instance) {
        return;
    }
    if (instance->list) {
        for (size_t i = 0; i < instance->count; ++i) {
            free(instance->list[i]);
        }
        free(instance->list);
    }
    free((void *)instance->directory_path);
    free(instance);
}


esp_err_t bsp_extra_i2s_read(void *audio_buffer, size_t len, size_t *bytes_read, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (bytes_read) {
        *bytes_read = 0;
    }
    ESP_RETURN_ON_FALSE(record_dev_handle && audio_buffer, ESP_ERR_INVALID_STATE, TAG, "Recorder is not initialized");
    esp_err_t ret = ESP_OK;
    ret = esp_codec_dev_read(record_dev_handle, audio_buffer, len);
    if (ret == ESP_OK && bytes_read) {
        *bytes_read = len;
    }
    return ret;
}

esp_err_t bsp_extra_i2s_write(void *audio_buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (bytes_written) {
        *bytes_written = 0;
    }
    ESP_RETURN_ON_FALSE(play_dev_handle && audio_buffer, ESP_ERR_INVALID_STATE, TAG, "Player is not initialized");
    esp_err_t ret = ESP_OK;
    ret = esp_codec_dev_write(play_dev_handle, audio_buffer, len);
    if (ret == ESP_OK && bytes_written) {
        *bytes_written = len;
    }
    return ret;
}

esp_err_t bsp_extra_codec_set_fs(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch)
{
    ESP_RETURN_ON_FALSE(play_dev_handle && record_dev_handle, ESP_ERR_INVALID_STATE,
                        TAG, "Codec is not initialized");
    ESP_RETURN_ON_ERROR(bsp_extra_codec_dev_stop(), TAG, "Close codec devices failed");

    esp_codec_dev_sample_info_t output_fs = {
        .sample_rate = rate,
        .channel = ch,
        .bits_per_sample = bits_cfg,
    };

    esp_err_t ret = esp_codec_dev_open(play_dev_handle, &output_fs);
    if (ret == ESP_OK) {
        ret = esp_codec_dev_set_out_mute(play_dev_handle, false);
    }
    if (ret == ESP_OK) {
        ret = esp_codec_dev_set_out_vol(play_dev_handle, _volume_intensity);
    }
    if (ret != ESP_OK) {
        (void)bsp_extra_codec_dev_stop();
        return ret;
    }

    ESP_LOGI(TAG, "Playback ready: ES8311 %lu Hz/%lu-bit/%dch, PA GPIO%d=%d",
             (unsigned long)rate, (unsigned long)bits_cfg, (int)ch,
             (int)BSP_POWER_AMP_IO, bsp_extra_codec_pa_is_enabled());
    return ESP_OK;
}

esp_err_t bsp_extra_codec_set_voice_fs(uint32_t rate, uint32_t bits_cfg,
                                       uint8_t record_channels,
                                       uint16_t record_tdm_slot_mask,
                                       uint16_t record_mic_gain_mask)
{
    ESP_RETURN_ON_FALSE(play_dev_handle && record_dev_handle, ESP_ERR_INVALID_STATE,
                        TAG, "Codec is not initialized");
    ESP_RETURN_ON_FALSE(record_channels > 0 && record_channels <= 16 &&
                        record_tdm_slot_mask != 0 &&
                        (record_tdm_slot_mask >> record_channels) == 0 &&
                        record_mic_gain_mask != 0 &&
                        (record_mic_gain_mask >> 4) == 0,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid record channel selection");
    ESP_RETURN_ON_ERROR(bsp_extra_codec_dev_stop(), TAG, "Close codec devices failed");

    esp_codec_dev_sample_info_t output_fs = {
        .sample_rate = rate,
        .channel = I2S_SLOT_MODE_STEREO,
        .bits_per_sample = bits_cfg,
    };
    esp_codec_dev_sample_info_t input_fs = {
        .sample_rate = rate,
        .channel = record_channels,
        .channel_mask = record_tdm_slot_mask,
        .bits_per_sample = bits_cfg,
    };

    esp_err_t ret = esp_codec_dev_open(play_dev_handle, &output_fs);
    if (ret == ESP_OK) {
        ret = esp_codec_dev_set_out_mute(play_dev_handle, false);
    }
    if (ret == ESP_OK) {
        ret = esp_codec_dev_set_out_vol(play_dev_handle, _volume_intensity);
    }
    if (ret != ESP_OK) {
        (void)bsp_extra_codec_dev_stop();
        return ret;
    }

    ret = esp_codec_dev_open(record_dev_handle, &input_fs);
    if (ret == ESP_OK) {
        ret = esp_codec_dev_set_in_channel_gain(
                  record_dev_handle, record_mic_gain_mask,
                  CODEC_DEFAULT_ADC_VOLUME
              );
    }
    if (ret != ESP_OK) {
        (void)bsp_extra_codec_dev_stop();
        return ret;
    }

    ESP_LOGI(TAG,
             "Voice ready: ES8311+ES7210 %lu Hz/%lu-bit, slots=0x%x, mics=0x%x, PA GPIO%d=%d",
             (unsigned long)rate, (unsigned long)bits_cfg,
             (unsigned int)record_tdm_slot_mask, (unsigned int)record_mic_gain_mask,
             (int)BSP_POWER_AMP_IO, bsp_extra_codec_pa_is_enabled());
    return ESP_OK;
}

bool bsp_extra_codec_pa_is_enabled(void)
{
    /* audio_codec_gpio configures the PA pin as output-only. ESP-IDF documents
     * that gpio_get_level() always returns 0 while the input path is disabled,
     * even when the output latch is high. Enabling the input buffer preserves
     * the output driver and lets this diagnostic read the physical pad level. */
    esp_err_t ret = gpio_input_enable(BSP_POWER_AMP_IO);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Enable PA GPIO input sensing failed: %s", esp_err_to_name(ret));
        return false;
    }
    return gpio_get_level(BSP_POWER_AMP_IO) != 0;
}

esp_err_t bsp_extra_codec_volume_set(int volume, int *volume_set)
{
    ESP_RETURN_ON_FALSE(play_dev_handle, ESP_ERR_INVALID_STATE, TAG, "Player is not initialized");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(play_dev_handle, volume), TAG, "Set Codec volume failed");
    _volume_intensity = volume;

    if (volume_set) {
        *volume_set = volume;
    }

    ESP_LOGI(TAG, "Setting volume: %d", volume);

    return ESP_OK;
}

int bsp_extra_codec_volume_get(void)
{
    return _volume_intensity;
}

esp_err_t bsp_extra_codec_mute_set(bool enable)
{
    ESP_RETURN_ON_FALSE(play_dev_handle, ESP_ERR_INVALID_STATE, TAG, "Player is not initialized");
    return esp_codec_dev_set_out_mute(play_dev_handle, enable);
}

esp_err_t bsp_extra_codec_dev_stop(void)
{
    esp_err_t ret = ESP_OK;

    /* Close capture before playback. Both codec devices share one duplex I2S
     * data interface; this is the same ordering used by the P4 reference and
     * avoids leaving the TX side pending behind an active RX side. */
    if (record_dev_handle) {
        update_result(&ret, esp_codec_dev_close(record_dev_handle));
    }
    if (play_dev_handle) {
        update_result(&ret, esp_codec_dev_close(play_dev_handle));
    }

    if (play_dev_handle) {
        ESP_LOGI(TAG, "Audio stopped: PA GPIO%d=%d", (int)BSP_POWER_AMP_IO,
                 bsp_extra_codec_pa_is_enabled());
    }
    return ret;
}

esp_err_t bsp_extra_codec_dev_resume(void)
{
    return bsp_extra_codec_set_fs(CODEC_DEFAULT_SAMPLE_RATE, CODEC_DEFAULT_BIT_WIDTH, CODEC_DEFAULT_CHANNEL);
}

esp_err_t bsp_extra_codec_init(void)
{
    if (_is_audio_init) {
        return ESP_OK;
    }

    esp_err_t ret = bsp_audio_init_voice_24k();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Initialize mixed STD/TDM audio bus failed: %s", esp_err_to_name(ret));
        return ret;
    }

    play_dev_handle = bsp_audio_codec_speaker_init();
    if (!play_dev_handle) {
        ret = ESP_FAIL;
        goto cleanup;
    }

    record_dev_handle = bsp_audio_codec_microphone_init();
    if (!record_dev_handle) {
        ret = ESP_FAIL;
        goto cleanup;
    }

    ret = bsp_extra_codec_set_fs(
        CODEC_DEFAULT_SAMPLE_RATE,
        CODEC_DEFAULT_BIT_WIDTH,
        CODEC_DEFAULT_CHANNEL
    );
    if (ret != ESP_OK) {
        goto cleanup;
    }

    _is_audio_init = true;
    return ESP_OK;

cleanup:
    (void)bsp_audio_deinit();
    play_dev_handle = NULL;
    record_dev_handle = NULL;
    return ret;
}

esp_err_t bsp_extra_player_init(void)
{
    if (_is_player_init) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(bsp_extra_codec_init(), TAG, "Initialize codec failed");
    ESP_RETURN_ON_ERROR(bsp_extra_codec_dev_resume(), TAG, "Resume codec failed");

    audio_player_config_t config = {
        .mute_fn = audio_mute_function,
        .write_fn = bsp_extra_i2s_write,
        .clk_set_fn = bsp_extra_codec_set_fs,
        .priority = 5,
        .coreID = 0,
        .force_stereo = true,
    };
    ESP_RETURN_ON_ERROR(audio_player_new(config), TAG, "Create audio player failed");

    esp_err_t ret = audio_player_callback_register(audio_event_callback, NULL);
    if (ret != ESP_OK) {
        (void)audio_player_delete();
        (void)bsp_extra_codec_dev_stop();
        return ret;
    }

    _audio_file_path[0] = '\0';
    _is_player_init = true;
    return ESP_OK;
}

esp_err_t bsp_extra_player_del(void)
{
    if (!_is_player_init) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(audio_player_delete(), TAG, "Delete audio player failed");
    _is_player_init = false;
    _audio_file_path[0] = '\0';
    return bsp_extra_codec_dev_stop();
}

bool bsp_extra_player_is_initialized(void)
{
    return _is_player_init;
}

esp_err_t bsp_extra_file_instance_init(const char *path, file_iterator_instance_t **ret_instance)
{
    ESP_RETURN_ON_FALSE(path && ret_instance, ESP_ERR_INVALID_ARG, TAG, "Invalid iterator arguments");
    *ret_instance = NULL;

    DIR *directory = opendir(path);
    ESP_RETURN_ON_FALSE(directory, ESP_ERR_NOT_FOUND, TAG, "Audio directory not found: %s", path);

    size_t file_count = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(directory)) != NULL) {
        if (is_supported_audio_file(entry->d_name)) {
            ++file_count;
        }
    }
    closedir(directory);

    file_iterator_instance_t *instance = calloc(1, sizeof(*instance));
    ESP_RETURN_ON_FALSE(instance, ESP_ERR_NO_MEM, TAG, "Allocate file iterator failed");

    instance->directory_path = strdup(path);
    if (!instance->directory_path) {
        free_file_instance(instance);
        return ESP_ERR_NO_MEM;
    }

    if (file_count > 0) {
        instance->list = calloc(file_count, sizeof(*instance->list));
        if (!instance->list) {
            free_file_instance(instance);
            return ESP_ERR_NO_MEM;
        }

        directory = opendir(path);
        if (!directory) {
            free_file_instance(instance);
            return ESP_ERR_NOT_FOUND;
        }

        while (instance->count < file_count && (entry = readdir(directory)) != NULL) {
            if (!is_supported_audio_file(entry->d_name)) {
                continue;
            }
            instance->list[instance->count] = strdup(entry->d_name);
            if (!instance->list[instance->count]) {
                closedir(directory);
                free_file_instance(instance);
                return ESP_ERR_NO_MEM;
            }
            ++instance->count;
        }
        closedir(directory);
    }

    ESP_LOGI(TAG, "Found %u MP3/WAV file(s) in %s", (unsigned)instance->count, path);
    *ret_instance = instance;
    return ESP_OK;
}

void bsp_extra_file_instance_deinit(file_iterator_instance_t **instance)
{
    if (!instance) {
        return;
    }
    free_file_instance(*instance);
    *instance = NULL;
}

esp_err_t bsp_extra_player_play_index(file_iterator_instance_t *instance, int index)
{
    ESP_RETURN_ON_FALSE(_is_player_init, ESP_ERR_INVALID_STATE, TAG, "Audio player is not initialized");
    ESP_RETURN_ON_FALSE(instance && index >= 0 && (size_t)index < instance->count,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid track index");

    char filename[sizeof(_audio_file_path)];
    int length = file_iterator_get_full_path_from_index(instance, (size_t)index, filename, sizeof(filename));
    ESP_RETURN_ON_FALSE(length > 0 && (size_t)length < sizeof(filename),
                        ESP_ERR_INVALID_SIZE, TAG, "Track path is too long");

    ESP_RETURN_ON_ERROR(bsp_extra_player_play_file(filename), TAG, "Start track failed");
    file_iterator_set_index(instance, (size_t)index);
    return ESP_OK;
}

esp_err_t bsp_extra_player_play_file(const char *file_path)
{
    ESP_RETURN_ON_FALSE(_is_player_init, ESP_ERR_INVALID_STATE, TAG, "Audio player is not initialized");
    ESP_RETURN_ON_FALSE(file_path, ESP_ERR_INVALID_ARG, TAG, "Track path is NULL");
    ESP_RETURN_ON_FALSE(is_supported_audio_file(file_path), ESP_ERR_NOT_SUPPORTED, TAG,
                        "Only MP3 and WAV files are supported");

    FILE *file = fopen(file_path, "rb");
    ESP_RETURN_ON_FALSE(file, ESP_ERR_NOT_FOUND, TAG, "Open track failed: %s", file_path);

    esp_err_t ret = audio_player_play(file);
    if (ret != ESP_OK) {
        fclose(file);
        return ret;
    }

    snprintf(_audio_file_path, sizeof(_audio_file_path), "%s", file_path);
    ESP_LOGI(TAG, "Playing %s", _audio_file_path);
    return ESP_OK;
}

void bsp_extra_player_register_callback(audio_player_cb_t cb, void *user_data)
{
    _audio_callback = cb;
    _audio_callback_user_data = user_data;
}

bool bsp_extra_player_is_playing_by_path(const char *file_path)
{
    if (!file_path || !_is_player_init || _audio_file_path[0] == '\0') {
        return false;
    }
    const audio_player_state_t state = audio_player_get_state();
    return state != AUDIO_PLAYER_STATE_IDLE && state != AUDIO_PLAYER_STATE_SHUTDOWN &&
           strcmp(_audio_file_path, file_path) == 0;
}

bool bsp_extra_player_is_playing_by_index(file_iterator_instance_t *instance, int index)
{
    if (!instance || index < 0 || (size_t)index >= instance->count) {
        return false;
    }

    char filename[sizeof(_audio_file_path)];
    int length = file_iterator_get_full_path_from_index(instance, (size_t)index, filename, sizeof(filename));
    return length > 0 && (size_t)length < sizeof(filename) &&
           bsp_extra_player_is_playing_by_path(filename);
}
