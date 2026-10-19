/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "MusicPlayer.hpp"

#include "audio_player.h"
#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "esp_brookesia.hpp"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "gui_music/lv_demo_music.h"
#include "gui_music/lv_demo_music_main.h"
#include "lvgl.h"

#ifdef ESP_UTILS_LOG_TAG
#undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:MusicPlayer"
#include "esp_lib_utils.h"

namespace {

const char *TAG = "MusicPlayer";
constexpr char SD_MUSIC_DIR[] = STORAGE_SERVICE_MOUNT_POINT "/music";
constexpr char SD_MUSIC_DIR_COMPAT[] = STORAGE_SERVICE_MOUNT_POINT "/Music";
constexpr char SPIFFS_MUSIC_DIR[] = BSP_SPIFFS_MOUNT_POINT "/music";

size_t scan_music_directory(const char *path, const char *source,
                            file_iterator_instance_t **iterator)
{
    bsp_extra_file_instance_deinit(iterator);

    const esp_err_t result = bsp_extra_file_instance_init(path, iterator);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "%s music directory unavailable (%s): %s",
                 source, path, esp_err_to_name(result));
        bsp_extra_file_instance_deinit(iterator);
        return 0;
    }

    const size_t count = file_iterator_get_count(*iterator);
    if (count == 0) {
        ESP_LOGI(TAG, "%s music directory is empty: %s", source, path);
        bsp_extra_file_instance_deinit(iterator);
        return 0;
    }

    ESP_LOGI(TAG, "Using %s music library: %s (%u track(s))",
             source, path, static_cast<unsigned>(count));
    return count;
}

void log_memory(const char *stage, size_t track_count)
{
    ESP_LOGI(
        TAG,
        "%s: tracks=%u, internal=%u/%u, psram=%u/%u, stack_hwm=%u",
        stage,
        static_cast<unsigned>(track_count),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr))
    );
}

} // namespace

LV_IMG_DECLARE(img_app_musicplayer);

namespace esp_brookesia::apps {

MusicPlayer *MusicPlayer::_instance = nullptr;

MusicPlayer *MusicPlayer::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (_instance == nullptr) {
        _instance = new MusicPlayer(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

MusicPlayer::MusicPlayer(bool use_status_bar, bool use_navigation_bar):
    App("MusicPlayer", &img_app_musicplayer, true, use_status_bar, use_navigation_bar),
    _file_iterator(nullptr),
    _storage_lease{},
    _player_initialized(false),
    _audio_session_acquired(false),
    _demo_created(false)
{
}

MusicPlayer::~MusicPlayer()
{
}

bool MusicPlayer::startAudioSession(void)
{
    const bsp_extra_audio_owner_t current_owner = bsp_extra_audio_session_get_owner();

    if (_player_initialized) {
        if (!_audio_session_acquired || current_owner != BSP_EXTRA_AUDIO_OWNER_MUSIC) {
            ESP_LOGE(TAG, "Player is initialized without the MusicPlayer audio session");
            return false;
        }
        return true;
    }

    if (_audio_session_acquired) {
        if (current_owner != BSP_EXTRA_AUDIO_OWNER_MUSIC) {
            ESP_LOGE(TAG, "MusicPlayer lost its audio session to %s",
                     bsp_extra_audio_owner_name(current_owner));
            return false;
        }
    } else {
        esp_err_t result = bsp_extra_audio_session_acquire(BSP_EXTRA_AUDIO_OWNER_MUSIC);
        if (result != ESP_OK) {
            const bsp_extra_audio_owner_t owner = bsp_extra_audio_session_get_owner();
            ESP_LOGW(TAG, "Audio session unavailable (%s): %s",
                     bsp_extra_audio_owner_name(owner), esp_err_to_name(result));
            return false;
        }
        _audio_session_acquired = true;
    }

    const esp_err_t result = bsp_extra_player_init();
    _player_initialized = bsp_extra_player_is_initialized();
    if (result == ESP_OK && _player_initialized) {
        ESP_LOGI(TAG, "MusicPlayer audio session is ready");
        return true;
    }

    ESP_LOGE(TAG, "Audio player initialization failed: %s", esp_err_to_name(result));
    if (!stopAudioSession(false)) {
        ESP_LOGE(TAG, "Retaining MusicPlayer audio session after initialization cleanup failed");
    }
    return false;
}

bool MusicPlayer::stopAudioSession(bool request_pause)
{
    if (_player_initialized && !_audio_session_acquired) {
        ESP_LOGE(TAG, "Refusing to stop an audio player without a tracked MusicPlayer session");
        return false;
    }

    if (_audio_session_acquired) {
        const bsp_extra_audio_owner_t owner = bsp_extra_audio_session_get_owner();
        if (owner != BSP_EXTRA_AUDIO_OWNER_MUSIC) {
            ESP_LOGE(TAG, "Refusing MusicPlayer cleanup because the current owner is %s",
                     bsp_extra_audio_owner_name(owner));
            return false;
        }
    }

    bool codec_stopped = false;
    if (_player_initialized) {
        if (request_pause) {
            const esp_err_t pause_result = audio_player_pause();
            if (pause_result != ESP_OK) {
                /* Deletion still issues a stop request and waits for the task.
                 * Continue cleanup, but preserve this diagnostic. */
                ESP_LOGW(TAG, "audio_player_pause failed before shutdown: %s",
                         esp_err_to_name(pause_result));
            }
        }

        const esp_err_t delete_result = bsp_extra_player_del();
        _player_initialized = bsp_extra_player_is_initialized();
        codec_stopped = delete_result == ESP_OK && !_player_initialized;
        if (delete_result != ESP_OK) {
            ESP_LOGE(TAG, "Audio player shutdown failed: %s", esp_err_to_name(delete_result));
        }
        if (_player_initialized) {
            ESP_LOGE(TAG, "MusicPlayer audio task is still active; retaining its session");
            return false;
        }
    }

    if (!_audio_session_acquired) {
        return true;
    }

    /* bsp_extra_player_del() normally closes the codec. Retry explicitly when
     * there was no player or when its final codec stop reported an error. */
    if (!codec_stopped) {
        const esp_err_t stop_result = bsp_extra_codec_dev_stop();
        if (stop_result != ESP_OK) {
            ESP_LOGE(TAG, "Codec shutdown failed; retaining MusicPlayer session: %s",
                     esp_err_to_name(stop_result));
            return false;
        }
    }

    const esp_err_t release_result =
        bsp_extra_audio_session_release(BSP_EXTRA_AUDIO_OWNER_MUSIC);
    if (release_result != ESP_OK) {
        ESP_LOGE(TAG, "MusicPlayer audio session release failed: %s",
                 esp_err_to_name(release_result));
        return false;
    }

    _audio_session_acquired = false;
    return true;
}

bool MusicPlayer::run(void)
{
    ESP_UTILS_LOGD("Run");
    if (!_sd_gate.ready()) {
        return _sd_gate.open(lv_screen_active(), [](void *context) {
            auto *app = static_cast<MusicPlayer *>(context);
            if (!app->run()) (void)app->notifyCoreClosed();
        }, [](void *context) {
            (void)static_cast<MusicPlayer *>(context)->back();
        }, this);
    }

    if (_demo_created) {
        ESP_LOGW(TAG, "Music UI is already running");
        return true;
    }
    if (_player_initialized || _audio_session_acquired ||
            _file_iterator != nullptr || _storage_lease.active) {
        ESP_LOGE(TAG, "Cannot restart while previous playback resources are still active");
        return false;
    }

    size_t track_count = 0;
    const char *music_path = nullptr;

    /* Hold the lease for the complete lifetime of the iterator and the audio
     * player's FILE handle. A lease prevents Settings from logically
     * unmounting the card while playback is active. Physical removal can still
     * happen on this board (there is no card-detect GPIO), so every open/play
     * failure remains a recoverable UI error. */
    esp_err_t result = storage_service_acquire(&_storage_lease);
    if (result == ESP_OK) {
        track_count = scan_music_directory(SD_MUSIC_DIR, "SD", &_file_iterator);
        music_path = (track_count > 0) ? SD_MUSIC_DIR : nullptr;

        if (track_count == 0) {
            track_count = scan_music_directory(SD_MUSIC_DIR_COMPAT, "SD compatibility", &_file_iterator);
            music_path = (track_count > 0) ? SD_MUSIC_DIR_COMPAT : nullptr;
        }

        if (track_count == 0) {
            /* No SD-backed object survived the scans, so release before
             * falling back to the always-mounted firmware filesystem. */
            storage_service_release(&_storage_lease);
        }
    } else {
        ESP_LOGW(TAG, "SD music unavailable, falling back to SPIFFS: %s",
                 esp_err_to_name(result));
        storage_service_release(&_storage_lease);
    }

    if (track_count == 0) {
        track_count = scan_music_directory(SPIFFS_MUSIC_DIR, "SPIFFS", &_file_iterator);
        music_path = (track_count > 0) ? SPIFFS_MUSIC_DIR : nullptr;
    }

    if (track_count == 0) {
        ESP_LOGW(TAG, "No MP3/WAV tracks found on SD or SPIFFS");
        /* Keep the complete reference player visible even with an empty
         * firmware image. Passing NULL makes the already-guarded demo render
         * its cover/control/spectrum framework with playback disabled. */
        bsp_extra_file_instance_deinit(&_file_iterator);
        log_memory("Before empty music UI", 0);
        lv_demo_music(lv_screen_active(), nullptr);
        _demo_created = true;
        log_memory("After empty music UI", 0);
        return true;
    }

    ESP_LOGI(TAG, "Music source selected: %s", music_path);

    /* Claim the shared ES8311/ES7210 session only after a playable library is
     * available. Acquire is non-blocking so another audio app remains intact. */
    if (!startAudioSession()) {
        if (!_player_initialized && !_audio_session_acquired) {
            bsp_extra_file_instance_deinit(&_file_iterator);
            storage_service_release(&_storage_lease);
        } else {
            ESP_LOGE(TAG, "Retaining music library because audio cleanup is incomplete");
        }
        return false;
    }

    log_memory("Before music UI", track_count);
    lv_demo_music(lv_screen_active(), _file_iterator);
    _demo_created = true;
    log_memory("After music UI", track_count);
    return true;
}

bool MusicPlayer::back(void)
{
    ESP_UTILS_LOGD("Back");
    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "Notify core closed failed");
    return true;
}

bool MusicPlayer::close(void)
{
    ESP_UTILS_LOGD("Close");

    /* Pause the visible demo before deleting its objects. The direct pause in
     * stopAudioSession() intentionally remains as a shutdown guard if the UI
     * was already gone or its logical playing flag was stale. */
    if (_demo_created) {
        if (_player_initialized) {
            lv_demo_music_pause();
        }
        lv_demo_music_exit_pause();
    }

    const bool audio_stopped = stopAudioSession(true);

    /* Delete this demo's own timers/objects before releasing the iterator.
     * Brookesia's subsequent resource cleanup is live-object aware, so this
     * targeted cleanup is idempotent and avoids the reference demo's global
     * lv_anim_del(NULL, NULL) side effect. */
    if (_demo_created) {
        lv_demo_music_close();
        _demo_created = false;
    }

    /* The iterator owns path/name strings and the audio player may own an open
     * FILE from that path. Retain both until the task, codec, and owner token
     * have all been released. */
    if (!_player_initialized && !_audio_session_acquired) {
        bsp_extra_file_instance_deinit(&_file_iterator);
        storage_service_release(&_storage_lease);
    } else {
        ESP_LOGE(TAG, "Retaining music iterator and SD lease because audio cleanup is incomplete");
    }
    _sd_gate.close();
    return audio_stopped;
}

bool MusicPlayer::init(void)
{
    ESP_UTILS_LOGD("Init");
    return true;
}

bool MusicPlayer::deinit(void)
{
    ESP_UTILS_LOGD("Deinit");
    return close();
}

bool MusicPlayer::pause(void)
{
    ESP_UTILS_LOGD("Pause");
    _sd_gate.pause();
    if (_demo_created) {
        if (_player_initialized) {
            lv_demo_music_pause();
        }
        lv_demo_music_exit_pause();
    }
    /* Preserve the UI, iterator, and SD lease, but fully hand off the shared
     * codec so Video/Recorder/SpecAnalyzer/Xiaozhi may acquire it. */
    return stopAudioSession(true);
}

bool MusicPlayer::resume(void)
{
    ESP_UTILS_LOGD("Resume");
    _sd_gate.resume();
    if (_file_iterator == nullptr) {
        /* Empty-library UI never claimed audio. */
        return true;
    }
    return startAudioSession();
}

} // namespace esp_brookesia::apps
