/*
 * SPDX-FileCopyrightText: 2015-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <sys/cdefs.h>
#include <stdbool.h>
#include "esp_codec_dev.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "audio_player.h"
#include "file_iterator.h"
#include "bsp/esp-bsp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CODEC_DEFAULT_SAMPLE_RATE           (16000)
#define CODEC_VOICE_SAMPLE_RATE             (24000)
#define CODEC_DEFAULT_BIT_WIDTH             (16)
#define CODEC_DEFAULT_ADC_VOLUME            (24.0)
#define CODEC_DEFAULT_CHANNEL               (2)
#define CODEC_VOICE_INPUT_CHANNELS          (4)
#define CODEC_DEFAULT_VOLUME                (80)

/* ES7210 serializes MIC1, MIC3(reference), MIC2, MIC4 in that order. TDM slot
 * masks and physical microphone masks are deliberately separate namespaces. */
#define BSP_EXTRA_ES7210_TDM_SLOT_MIC1_MASK            (1U << 0)
#define BSP_EXTRA_ES7210_TDM_SLOT_MIC3_ECHO_MASK       (1U << 1)
#define BSP_EXTRA_ES7210_TDM_SLOT_MIC2_MASK            (1U << 2)
#define BSP_EXTRA_ES7210_TDM_SLOT_MIC4_MASK            (1U << 3)
#define BSP_EXTRA_ES7210_TDM_ALL_SLOTS_MASK            \
    (BSP_EXTRA_ES7210_TDM_SLOT_MIC1_MASK |             \
     BSP_EXTRA_ES7210_TDM_SLOT_MIC3_ECHO_MASK |        \
     BSP_EXTRA_ES7210_TDM_SLOT_MIC2_MASK |             \
     BSP_EXTRA_ES7210_TDM_SLOT_MIC4_MASK)
#define BSP_EXTRA_ES7210_PHYSICAL_MIC1_MASK            (1U << 0)
#define BSP_EXTRA_ES7210_PHYSICAL_MIC2_MASK            (1U << 1)
#define BSP_EXTRA_ES7210_PHYSICAL_MIC3_MASK            (1U << 2)
#define BSP_EXTRA_ES7210_PHYSICAL_FRONT_MIC_MASK       \
    (BSP_EXTRA_ES7210_PHYSICAL_MIC1_MASK | BSP_EXTRA_ES7210_PHYSICAL_MIC2_MASK)
#define BSP_EXTRA_ES7210_PHYSICAL_CONNECTED_MIC_MASK   \
    (BSP_EXTRA_ES7210_PHYSICAL_FRONT_MIC_MASK | BSP_EXTRA_ES7210_PHYSICAL_MIC3_MASK)

#define BSP_LCD_BACKLIGHT_BRIGHTNESS_MAX    (95)
#define BSP_LCD_BACKLIGHT_BRIGHTNESS_MIN    (0)
#define LCD_LEDC_CH                         (CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH)

/**
 * @brief Exclusive users of the board's shared ES8311/ES7210 audio bus.
 *
 * The owner token protects the complete codec session, not an individual
 * playback or capture call. Acquire is deliberately non-recursive: every
 * successful acquire must have exactly one matching release.
 */
typedef enum {
    BSP_EXTRA_AUDIO_OWNER_NONE = 0,
    BSP_EXTRA_AUDIO_OWNER_MUSIC,
    BSP_EXTRA_AUDIO_OWNER_VIDEO,
    BSP_EXTRA_AUDIO_OWNER_RECORDER,
    BSP_EXTRA_AUDIO_OWNER_SPEC_ANALYZER,
    BSP_EXTRA_AUDIO_OWNER_XIAOZHI,
    BSP_EXTRA_AUDIO_OWNER_MAX,
} bsp_extra_audio_owner_t;

/**
 * @brief Try to acquire the board audio session without blocking.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for NONE/out-of-range owners,
 *         or ESP_ERR_INVALID_STATE when a session is already owned.
 */
esp_err_t bsp_extra_audio_session_acquire(bsp_extra_audio_owner_t owner);

/**
 * @brief Release a previously acquired board audio session.
 *
 * Only the current owner may release the session.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for NONE/out-of-range owners,
 *         or ESP_ERR_INVALID_STATE when owner does not own the session.
 */
esp_err_t bsp_extra_audio_session_release(bsp_extra_audio_owner_t owner);

/**
 * @brief Return the current audio owner using a cross-core-safe snapshot.
 */
bsp_extra_audio_owner_t bsp_extra_audio_session_get_owner(void);

/**
 * @brief Return a stable diagnostic name for an owner value.
 *
 * Invalid values return "Invalid".
 */
const char *bsp_extra_audio_owner_name(bsp_extra_audio_owner_t owner);

/**************************************************************************************************
 * BSP Extra interface
 * Mainly provided some I2S Codec interfaces.
 **************************************************************************************************/
/**
 * @brief Player set mute.
 *
 * @param enable: true or false
 *
 * @return
 *    - ESP_OK: Success
 *    - Others: Fail
 */
esp_err_t bsp_extra_codec_mute_set(bool enable);

/**
 * @brief Player set volume.
 *
 * @param volume: volume set
 * @param volume_set: volume set response
 *
 * @return
 *    - ESP_OK: Success
 *    - Others: Fail
 */
esp_err_t bsp_extra_codec_volume_set(int volume, int *volume_set);

/**
 * @brief Player get volume.
 *
 * @return
 *   - volume: volume get
 */
int bsp_extra_codec_volume_get(void);

/**
 * @brief Stop I2S function.
 *
 * @return
 *    - ESP_OK: Success
 *    - Others: Fail
 */
esp_err_t bsp_extra_codec_dev_stop(void);

/**
 * @brief Resume I2S function.
 *
 * @return
 *    - ESP_OK: Success
 *    - Others: Fail
 */
esp_err_t bsp_extra_codec_dev_resume(void);

/**
 * @brief Set I2S format to codec.
 *
 * @param rate: Sample rate of sample
 * @param bits_cfg: Bit lengths of one channel data
 * @param ch: Channels of sample
 *
 * @return
 *    - ESP_OK: Success
 *    - Others: Fail
 */
esp_err_t bsp_extra_codec_set_fs(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch);

/**
 * @brief Configure ES8311 STD playback and ES7210 four-slot TDM capture.
 *
 * record_channels is the physical TDM slot count. record_tdm_slot_mask selects
 * serialized slots returned by reads, while record_mic_gain_mask selects
 * physical ES7210 inputs for gain control. The two masks are not interchangeable.
 */
esp_err_t bsp_extra_codec_set_voice_fs(uint32_t rate, uint32_t bits_cfg,
                                       uint8_t record_channels,
                                       uint16_t record_tdm_slot_mask,
                                       uint16_t record_mic_gain_mask);

/**
 * @brief Return whether the active-high ES8311 PA GPIO is currently asserted.
 */
bool bsp_extra_codec_pa_is_enabled(void);

/**
 * @brief Read data from recoder.
 *
 * @param audio_buffer: The pointer of receiving data buffer
 * @param len: Max data buffer length
 * @param bytes_read: Byte number that actually be read, can be NULL if not needed
 * @param timeout_ms: Max block time
 *
 * @return
 *    - ESP_OK: Success
 *    - Others: Fail
 */
esp_err_t bsp_extra_i2s_read(void *audio_buffer, size_t len, size_t *bytes_read, uint32_t timeout_ms);

/**
 * @brief Write data to player.
 *
 * @param audio_buffer: The pointer of sent data buffer
 * @param len: Max data buffer length
 * @param bytes_written: Byte number that actually be sent, can be NULL if not needed
 * @param timeout_ms: Max block time
 *
 * @return
 *    - ESP_OK: Success
 *    - Others: Fail
 */
esp_err_t bsp_extra_i2s_write(void *audio_buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms);


/**
 * @brief Initialize codec play and record handle.
 *
 * @return
 *      - ESP_OK: Success
 *      - Others: Fail
 */
esp_err_t bsp_extra_codec_init(void);

/**
 * @brief Initialize the MP3/WAV player task using the board codec.
 */
esp_err_t bsp_extra_player_init(void);

/**
 * @brief Stop and delete the audio player task.
 */
esp_err_t bsp_extra_player_del(void);

/**
 * @brief Return whether the audio player task still owns playback resources.
 *
 * This lets storage-backed callers retain their mount lease if player
 * shutdown fails before the active FILE has been released.
 */
bool bsp_extra_player_is_initialized(void);

/**
 * @brief Scan a directory and create an iterator containing MP3/WAV files.
 *
 * Unlike the upstream iterator constructor, this wrapper validates the
 * directory first, filters unsupported entries, and safely supports an empty
 * directory.
 */
esp_err_t bsp_extra_file_instance_init(const char *path, file_iterator_instance_t **ret_instance);

/**
 * @brief Free an iterator created by bsp_extra_file_instance_init().
 */
void bsp_extra_file_instance_deinit(file_iterator_instance_t **instance);

/**
 * @brief Play the audio file at the requested iterator index.
 */
esp_err_t bsp_extra_player_play_index(file_iterator_instance_t *instance, int index);

/**
 * @brief Play an MP3/WAV file by absolute VFS path.
 */
esp_err_t bsp_extra_player_play_file(const char *file_path);

/**
 * @brief Register an application callback for audio-player events.
 */
void bsp_extra_player_register_callback(audio_player_cb_t cb, void *user_data);

/**
 * @brief Return true when file_path is the active or paused track.
 */
bool bsp_extra_player_is_playing_by_path(const char *file_path);

/**
 * @brief Return true when the iterator index is the active or paused track.
 */
bool bsp_extra_player_is_playing_by_index(file_iterator_instance_t *instance, int index);

#ifdef __cplusplus
}
#endif
