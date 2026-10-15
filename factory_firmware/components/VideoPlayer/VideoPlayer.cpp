/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "VideoPlayer.hpp"

#include <dirent.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "bsp/display.h"
#include "bsp_board_extra.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#ifdef ESP_UTILS_LOG_TAG
#undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:VideoPlayer"
#include "esp_lib_utils.h"

LV_IMG_DECLARE(img_app_vedioplayer);

namespace {

constexpr size_t AVI_BUFFER_SIZE = 512 * 1024;
constexpr TickType_t WORKER_JOIN_TIMEOUT = pdMS_TO_TICKS(3000);
constexpr TickType_t AUDIO_CALLBACK_DRAIN_TIMEOUT = pdMS_TO_TICKS(1500);
constexpr int64_t MIN_FRAME_INTERVAL_US = 100000; // Full-screen QSPI path: cap at 10 fps.
constexpr uint32_t CHROME_VISIBLE_MS = 3500;

class AudioCallbackUse {
public:
    explicit AudioCallbackUse(std::atomic<uint32_t> &users)
        : _users(users)
    {
        _users.fetch_add(1);
    }

    ~AudioCallbackUse()
    {
        _users.fetch_sub(1);
    }

    AudioCallbackUse(const AudioCallbackUse &) = delete;
    AudioCallbackUse &operator=(const AudioCallbackUse &) = delete;

private:
    std::atomic<uint32_t> &_users;
};

bool is_avi_file(const char *name)
{
    const char *extension = name ? strrchr(name, '.') : nullptr;
    return extension && strcasecmp(extension, ".avi") == 0;
}

bool is_supported_pcm_rate(uint32_t rate)
{
    switch (rate) {
    case 8000:
    case 12000:
    case 16000:
    case 24000:
    case 32000:
    case 44100:
    case 48000:
        return true;
    default:
        return false;
    }
}

int compare_video_paths(const void *lhs, const void *rhs)
{
    const auto *left = static_cast<char *const *>(lhs);
    const auto *right = static_cast<char *const *>(rhs);
    return strcasecmp(*left, *right);
}

} // namespace

namespace esp_brookesia::apps {

VideoPlayer *VideoPlayer::_instance = nullptr;

VideoPlayer *VideoPlayer::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (!_instance) {
        _instance = new VideoPlayer(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

VideoPlayer::VideoPlayer(bool use_status_bar, bool use_navigation_bar)
    : App("VideoPlayer", &img_app_vedioplayer, true, use_status_bar, use_navigation_bar)
{
}

VideoPlayer::~VideoPlayer()
{
    _ui_accept_frames.store(false);
    // LVGL may redraw the canvas until the screen is actually cleared by the
    // core.  Point it at permanent storage before freeing the PSRAM frames.
    detachCanvas();
    if (stopWorker(portMAX_DELAY)) {
        releaseFrameBuffers();
        freeVideos();
    }
    destroyUi();
    _instance = nullptr;
}

bool VideoPlayer::init()
{
    if (!_worker_stopped.load() || _worker_task.load() || !_cleanup_complete.load()) {
        return false;
    }
    _state.store(State::Idle);
    _last_error.store(ESP_OK);
    _command.store(Command::None);
    return true;
}

bool VideoPlayer::run()
{
    if (!_sd_gate.ready()) {
        return _sd_gate.open(lv_screen_active(), [](void *context) {
            auto *app = static_cast<VideoPlayer *>(context);
            if (!app->run()) (void)app->notifyCoreClosed();
        }, [](void *context) {
            (void)static_cast<VideoPlayer *>(context)->back();
        }, this);
    }
    _ui_accept_frames.store(false);
    detachCanvas();
    destroyUi();
    if (_worker_stopped.load() && _cleanup_complete.load()) {
        // A naturally completed/failed playback keeps its last frame alive until
        // the UI is detached.  A fresh run no longer references that frame.
        releaseFrameBuffers();
    }
    _screen = lv_screen_active();
    lv_obj_set_style_bg_color(_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_event_cb_with_user_data(_screen, chromeEvent, this);
    lv_obj_add_event_cb(_screen, chromeEvent, LV_EVENT_CLICKED, this);

    // Keep the reference app's simple, video-first composition. The canvas is
    // created before every overlay so a 320x200/320x240 clip remains the visual
    // focus on the target AMOLED.
    _canvas = lv_canvas_create(_screen);
    lv_canvas_set_buffer(_canvas, &_placeholder_pixel, 1, 1, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(_canvas, 1, 1);
    lv_obj_align(_canvas, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_canvas, chromeEvent, LV_EVENT_CLICKED, this);

    _hero_icon = lv_image_create(_screen);
    lv_image_set_src(_hero_icon, &img_app_vedioplayer);
    lv_obj_align(_hero_icon, LV_ALIGN_CENTER, 0, -42);

    _status_label = lv_label_create(_screen);
    lv_label_set_text(_status_label, "Mounting SD card...");
    lv_obj_set_width(_status_label, 360);
    lv_obj_set_style_text_align(_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(_status_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(_status_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_status_label, lv_color_hex(0x111118), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_status_label, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(_status_label, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(_status_label, 9, LV_PART_MAIN);
    lv_obj_set_style_radius(_status_label, 18, LV_PART_MAIN);
    lv_obj_align(_status_label, LV_ALIGN_CENTER, 0, 48);

    _header = lv_obj_create(_screen);
    lv_obj_remove_style_all(_header);
    lv_obj_set_size(_header, 260, 42);
    lv_obj_set_style_bg_color(_header, lv_color_hex(0x101016), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_header, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(_header, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(_header, lv_color_hex(0x393444), LV_PART_MAIN);
    lv_obj_set_style_border_opa(_header, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_radius(_header, 21, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(_header, 18, LV_PART_MAIN);
    lv_obj_set_flex_flow(_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        _header,
        LV_FLEX_ALIGN_SPACE_BETWEEN,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER
    );
    lv_obj_align(_header, LV_ALIGN_TOP_MID, 0, 22);

    lv_obj_t *title = lv_label_create(_header);
    lv_label_set_text(title, "VIDEO");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xC695FF), LV_PART_MAIN);

    _counter_label = lv_label_create(_header);
    lv_label_set_text(_counter_label, "0 / 0");
    lv_obj_set_style_text_font(_counter_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(_counter_label, lv_color_white(), LV_PART_MAIN);

    _controls = lv_obj_create(_screen);
    lv_obj_remove_style_all(_controls);
    lv_obj_set_size(_controls, 268, 68);
    lv_obj_set_style_bg_color(_controls, lv_color_hex(0x101016), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_controls, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(_controls, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(_controls, lv_color_hex(0x393444), LV_PART_MAIN);
    lv_obj_set_style_border_opa(_controls, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_radius(_controls, 34, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(_controls, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(_controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        _controls,
        LV_FLEX_ALIGN_SPACE_EVENLY,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER
    );
    lv_obj_align(_controls, LV_ALIGN_BOTTOM_MID, 0, -22);

    auto add_control = [this](const char *text, Command command, bool primary) -> lv_obj_t * {
        lv_obj_t *button = lv_button_create(_controls);
        lv_obj_set_size(button, primary ? 56 : 46, primary ? 56 : 46);
        lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(
            button,
            primary ? lv_color_hex(0x8E42FF) : lv_color_hex(0x2B2932),
            LV_PART_MAIN
        );
        lv_obj_set_style_bg_opa(button, primary ? LV_OPA_COVER : LV_OPA_80, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(button, primary ? 18 : 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(button, lv_color_hex(0x8E42FF), LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(button, primary ? LV_OPA_30 : LV_OPA_TRANSP, LV_PART_MAIN);
        const lv_style_selector_t disabled_selector =
            static_cast<lv_style_selector_t>(LV_PART_MAIN) |
            static_cast<lv_style_selector_t>(LV_STATE_DISABLED);
        lv_obj_set_style_opa(button, LV_OPA_30, disabled_selector);
        lv_obj_set_user_data(button, reinterpret_cast<void *>(static_cast<uintptr_t>(command)));
        lv_obj_add_event_cb(button, controlEvent, LV_EVENT_CLICKED, this);
        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, primary ? &lv_font_montserrat_24 : &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
        lv_obj_center(label);
        if (primary) {
            _play_button_label = label;
        }
        return button;
    };
    _previous_button = add_control(LV_SYMBOL_PREV, Command::Previous, false);
    _play_button = add_control(LV_SYMBOL_PAUSE, Command::Pause, true);
    _next_button = add_control(LV_SYMBOL_NEXT, Command::Next, false);

    _ui_timer = lv_timer_create(uiTimer, 50, this);
    _last_ui_state = State::Idle;
    _frame_presented = false;
    _chrome_visible = true;
    _chrome_hide_at_ms = 0;
    updateControlState(State::Mounting, 0);
    const bool started = _ui_timer && startWorker();
    _ui_accept_frames.store(started);
    return started;
}

bool VideoPlayer::back()
{
    return notifyCoreClosed();
}

bool VideoPlayer::close()
{
    _ui_accept_frames.store(false);
    detachCanvas();
    const bool stopped = stopWorker(WORKER_JOIN_TIMEOUT);
    if (stopped) {
        releaseFrameBuffers();
    }
    destroyUi();
    _sd_gate.close();
    return stopped;
}

bool VideoPlayer::deinit()
{
    return close();
}

bool VideoPlayer::pause()
{
    _sd_gate.pause();
    _ui_accept_frames.store(false);
    detachCanvas();
    if (_ui_timer) {
        lv_timer_pause(_ui_timer);
    }
    const bool stopped = stopWorker(WORKER_JOIN_TIMEOUT);
    if (stopped) {
        releaseFrameBuffers();
    }
    return stopped;
}

bool VideoPlayer::resume()
{
    _sd_gate.resume();
    if (!_canvas) {
        return true;
    }
    if (_ui_timer) {
        lv_timer_resume(_ui_timer);
    }
    const bool started = startWorker();
    _ui_accept_frames.store(started);
    return started;
}

void VideoPlayer::detachCanvas()
{
    if (_canvas) {
        lv_canvas_set_buffer(_canvas, &_placeholder_pixel, 1, 1, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_size(_canvas, 1, 1);
        lv_obj_invalidate(_canvas);
    }
    _display_buffer_index = -1;
    _pending_buffer_index = -1;
}

void VideoPlayer::destroyUi()
{
    if (_ui_timer) {
        lv_timer_del(_ui_timer);
        _ui_timer = nullptr;
    }
    if (_screen) {
        lv_obj_remove_event_cb_with_user_data(_screen, chromeEvent, this);
    }
    if (_canvas) {
        lv_obj_remove_event_cb_with_user_data(_canvas, chromeEvent, this);
    }
    _screen = nullptr;
    _canvas = nullptr;
    _header = nullptr;
    _controls = nullptr;
    _hero_icon = nullptr;
    _status_label = nullptr;
    _counter_label = nullptr;
    _previous_button = nullptr;
    _play_button = nullptr;
    _next_button = nullptr;
    _play_button_label = nullptr;
    _frame_presented = false;
    _chrome_visible = true;
    _chrome_hide_at_ms = 0;
}

void VideoPlayer::setChromeVisible(bool visible)
{
    auto set_visible = [visible](lv_obj_t *object) {
        if (!object) {
            return;
        }
        if (visible) {
            lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
        }
    };
    set_visible(_header);
    set_visible(_controls);
    _chrome_visible = visible;
}

void VideoPlayer::showChrome()
{
    setChromeVisible(true);
    _chrome_hide_at_ms = lv_tick_get() + CHROME_VISIBLE_MS;
}

void VideoPlayer::updateControlState(State state, int file_count)
{
    const bool enabled = file_count > 0 &&
        (state == State::Playing || state == State::Paused);
    lv_obj_t *buttons[] = {_previous_button, _play_button, _next_button};
    for (lv_obj_t *button : buttons) {
        if (!button) {
            continue;
        }
        if (enabled) {
            lv_obj_remove_state(button, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(button, LV_STATE_DISABLED);
        }
    }
}

bool VideoPlayer::startWorker()
{
    if (_worker_task.load() || !_worker_stopped.load()) {
        return false;
    }

    // A failed deinit/codec shutdown deliberately retains every callback,
    // FILE, owner token, and SD lease. Retry that cleanup before a new worker
    // is allowed to overwrite any handle or ownership flag.
    const bool retained_resources =
        _avi != nullptr || _jpeg != nullptr || _files != nullptr ||
        _storage_lease.active || _audio_claimed.load() ||
        _audio_session_acquired.load();
    if ((!_cleanup_complete.load() || retained_resources) &&
            !cleanupPlaybackResources()) {
        _state.store(State::Error);
        return false;
    }
    if (_avi != nullptr || _jpeg != nullptr || _files != nullptr ||
            _storage_lease.active || _audio_claimed.load() ||
            _audio_session_acquired.load()) {
        _last_error.store(ESP_ERR_INVALID_STATE);
        _state.store(State::Error);
        return false;
    }

    _stop_requested.store(false);
    _playing.store(false);
    _command.store(Command::None);
    _file_count.store(0);
    _current_index.store(0);
    _decoded_frames.store(0);
    _dropped_frames.store(0);
    _last_queued_us = 0;
    _last_error.store(ESP_OK);
    _audio_ready.store(false);
    _audio_claimed.store(false);
    _audio_session_acquired.store(false);
    _audio_disable_requested.store(false);
    _audio_callback_users.store(0);
    _cleanup_complete.store(false);
    _state.store(State::Mounting);
    _worker_stopped.store(false);

    TaskHandle_t task = nullptr;
    BaseType_t result = xTaskCreatePinnedToCore(
        workerTask,
        "sd_avi_control",
        8 * 1024,
        this,
        4,
        &task,
        PRO_CPU_NUM
    );
    if (result != pdPASS) {
        _worker_stopped.store(true);
        _cleanup_complete.store(true);
        _state.store(State::Error);
        _last_error.store(ESP_ERR_NO_MEM);
        return false;
    }
    _worker_task.store(task);
    // Publish the task handle before allowing the worker to run to completion
    // on the other core; otherwise a fast mount failure can leave a stale
    // handle after the worker has already cleared it.
    xTaskNotifyGive(task);
    return true;
}

bool VideoPlayer::stopWorker(TickType_t timeout)
{
    _stop_requested.store(true);
    _state.store(State::Stopping);
    TaskHandle_t task = _worker_task.load();
    if (task == xTaskGetCurrentTaskHandle()) {
        return false;
    }
    const TickType_t started = xTaskGetTickCount();
    while (!_worker_stopped.load()) {
        if (timeout != portMAX_DELAY && xTaskGetTickCount() - started >= timeout) {
            ESP_UTILS_LOGE("Timed out waiting for AVI worker");
            return false;
        }
        vTaskDelay(1);
    }
    if (!_cleanup_complete.load()) {
        return cleanupPlaybackResources();
    }
    return true;
}

esp_err_t VideoPlayer::scanVideos()
{
    freeVideos();
    const char *directories[] = {
        STORAGE_SERVICE_MOUNT_POINT "/video",
        STORAGE_SERVICE_MOUNT_POINT "/Video",
        STORAGE_SERVICE_MOUNT_POINT "/videos",
        STORAGE_SERVICE_MOUNT_POINT "/Videos",
        STORAGE_SERVICE_MOUNT_POINT "/avi",
        STORAGE_SERVICE_MOUNT_POINT "/AVI",
        STORAGE_SERVICE_MOUNT_POINT "/movies",
        STORAGE_SERVICE_MOUNT_POINT "/Movies",
        STORAGE_SERVICE_MOUNT_POINT,
    };

    for (const char *directory_path : directories) {
        DIR *directory = opendir(directory_path);
        if (!directory) {
            continue;
        }
        dirent *entry = nullptr;
        while ((entry = readdir(directory)) != nullptr) {
            if (!is_avi_file(entry->d_name)) {
                continue;
            }
            char **grown = static_cast<char **>(
                realloc(_files, sizeof(char *) * static_cast<size_t>(_file_count.load() + 1))
            );
            if (!grown) {
                closedir(directory);
                freeVideos();
                return ESP_ERR_NO_MEM;
            }
            _files = grown;
            const size_t length = strlen(directory_path) + strlen(entry->d_name) + 2;
            char *path = static_cast<char *>(malloc(length));
            if (!path) {
                closedir(directory);
                freeVideos();
                return ESP_ERR_NO_MEM;
            }
            snprintf(path, length, "%s/%s", directory_path, entry->d_name);
            struct stat file_info = {};
            if (stat(path, &file_info) != 0 || !S_ISREG(file_info.st_mode)) {
                free(path);
                continue;
            }
            const int index = _file_count.load();
            _files[index] = path;
            _file_count.store(index + 1);
        }
        closedir(directory);
        if (_file_count.load() > 0) {
            break;
        }
    }
    if (_file_count.load() > 1) {
        qsort(
            _files,
            static_cast<size_t>(_file_count.load()),
            sizeof(_files[0]),
            compare_video_paths
        );
    }
    return _file_count.load() > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void VideoPlayer::freeVideos()
{
    const int count = _file_count.exchange(0);
    if (_files) {
        for (int i = 0; i < count; ++i) {
            free(_files[i]);
        }
        free(_files);
        _files = nullptr;
    }
}

esp_err_t VideoPlayer::initJpeg()
{
    if (_jpeg) {
        return ESP_OK;
    }
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    return jpeg_dec_open(&config, &_jpeg) == JPEG_ERR_OK ? ESP_OK : ESP_FAIL;
}

void VideoPlayer::deinitJpeg()
{
    if (_jpeg) {
        jpeg_dec_close(_jpeg);
        _jpeg = nullptr;
    }
}

esp_err_t VideoPlayer::ensureFrameBuffers(uint16_t width, uint16_t height, size_t bytes)
{
    if (width == 0 || height == 0 || width > BSP_LCD_H_RES || height > BSP_LCD_V_RES ||
            bytes == 0 || bytes > BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (_frame_buffers[0]) {
        return (_video_width == width && _video_height == height &&
                _frame_buffer_size >= bytes) ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
    }
    const size_t aligned_bytes = (bytes + 15U) & ~static_cast<size_t>(15U);
    for (auto &buffer : _frame_buffers) {
        buffer = static_cast<uint8_t *>(heap_caps_aligned_calloc(
            16,
            1,
            aligned_bytes,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        ));
        if (!buffer) {
            releaseFrameBuffers();
            return ESP_ERR_NO_MEM;
        }
    }
    _frame_buffer_size = aligned_bytes;
    _video_width = width;
    _video_height = height;
    return ESP_OK;
}

void VideoPlayer::releaseFrameBuffers()
{
    for (auto &buffer : _frame_buffers) {
        heap_caps_free(buffer);
        buffer = nullptr;
    }
    _frame_buffer_size = 0;
    _video_width = 0;
    _video_height = 0;
    _display_buffer_index = -1;
    _pending_buffer_index = -1;
}

bool VideoPlayer::cleanupPlaybackResources()
{
    esp_err_t cleanup_result = ESP_OK;
    if (_avi) {
        if (_playing.load()) {
            const esp_err_t stop_result = avi_player_play_stop(_avi);
            if (stop_result != ESP_OK && stop_result != ESP_ERR_INVALID_STATE) {
                cleanup_result = stop_result;
            }
        }

        const esp_err_t deinit_result = avi_player_deinit(_avi);
        if (deinit_result != ESP_OK) {
            _last_error.store(deinit_result);
            _cleanup_complete.store(false);
            ESP_UTILS_LOGE(
                "AVI deinit failed; retaining SD lease and callback resources: %s",
                esp_err_to_name(deinit_result)
            );
            return false;
        }
        _avi = nullptr;
    }

    _playing.store(false);
    const esp_err_t audio_result =
        processAudioDisableRequest(AUDIO_CALLBACK_DRAIN_TIMEOUT, true);
    if (audio_result != ESP_OK && cleanup_result == ESP_OK) {
        cleanup_result = audio_result;
    }
    deinitJpeg();
    freeVideos();
    storage_service_release(&_storage_lease);

    _cleanup_complete.store(cleanup_result == ESP_OK);
    if (cleanup_result != ESP_OK) {
        _last_error.store(cleanup_result);
        ESP_UTILS_LOGE("AVI cleanup failed: %s", esp_err_to_name(cleanup_result));
    }
    return cleanup_result == ESP_OK;
}

esp_err_t VideoPlayer::prepareAudioForPlayback()
{
    // Finish a previous file's failed shutdown before reopening the callback
    // gate or acquiring another session. A timeout must retain ownership.
    if (_audio_disable_requested.load() ||
            (!_audio_ready.load() &&
             (_audio_claimed.load() || _audio_session_acquired.load()))) {
        const esp_err_t ret = processAudioDisableRequest(AUDIO_CALLBACK_DRAIN_TIMEOUT, true);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    if (_audio_ready.load()) {
        return ESP_OK;
    }

    const esp_err_t owner_result =
        bsp_extra_audio_session_acquire(BSP_EXTRA_AUDIO_OWNER_VIDEO);
    if (owner_result != ESP_OK) {
        ESP_UTILS_LOGW(
            "Audio is owned by %s; playing this AVI without sound",
            bsp_extra_audio_owner_name(bsp_extra_audio_session_get_owner())
        );
        return owner_result;
    }
    _audio_session_acquired.store(true);
    const esp_err_t codec_result = bsp_extra_codec_init();
    if (codec_result != ESP_OK) {
        _audio_disable_requested.store(true);
        ESP_UTILS_LOGW(
            "AVI codec init failed; releasing audio session: %s",
            esp_err_to_name(codec_result)
        );
        (void)processAudioDisableRequest(AUDIO_CALLBACK_DRAIN_TIMEOUT, false);
        return codec_result;
    }

    _audio_claimed.store(true);
    _audio_ready.store(true);
    return ESP_OK;
}

esp_err_t VideoPlayer::processAudioDisableRequest(TickType_t callback_timeout, bool force)
{
    if (!force && !_audio_disable_requested.load()) {
        return ESP_OK;
    }

    // Close the callback gate before checking the user count. A callback that
    // starts after this store can still touch the atomics, but it cannot enter
    // a codec operation.
    _audio_ready.store(false);
    const TickType_t started = xTaskGetTickCount();
    while (_audio_callback_users.load() != 0) {
        if (callback_timeout != portMAX_DELAY &&
                xTaskGetTickCount() - started >= callback_timeout) {
            _audio_disable_requested.store(true);
            _last_error.store(ESP_ERR_TIMEOUT);
            ESP_UTILS_LOGE(
                "Timed out waiting for %lu AVI audio callback user(s)",
                static_cast<unsigned long>(_audio_callback_users.load())
            );
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }

    if (_audio_claimed.load()) {
        const esp_err_t stop_result = bsp_extra_codec_dev_stop();
        if (stop_result != ESP_OK) {
            _audio_disable_requested.store(true);
            _last_error.store(stop_result);
            ESP_UTILS_LOGE(
                "AVI codec stop failed; retaining audio session: %s",
                esp_err_to_name(stop_result)
            );
            return stop_result;
        }
        _audio_claimed.store(false);
    }

    if (_audio_session_acquired.load()) {
        const esp_err_t release_result =
            bsp_extra_audio_session_release(BSP_EXTRA_AUDIO_OWNER_VIDEO);
        if (release_result != ESP_OK) {
            _audio_disable_requested.store(true);
            _last_error.store(release_result);
            ESP_UTILS_LOGE(
                "AVI audio session release failed; retaining ownership state: %s",
                esp_err_to_name(release_result)
            );
            return release_result;
        }
        _audio_session_acquired.store(false);
    }

    _audio_disable_requested.store(false);
    return ESP_OK;
}

void VideoPlayer::videoCallback(frame_data_t *data, void *arg)
{
    auto *self = static_cast<VideoPlayer *>(arg);
    if (!self || !data || !data->data || data->data_bytes == 0 ||
            self->_stop_requested.load() || !self->_ui_accept_frames.load()) {
        return;
    }
    const int64_t now_us = esp_timer_get_time();
    if (self->_last_queued_us != 0 &&
            now_us - self->_last_queued_us < MIN_FRAME_INTERVAL_US) {
        self->_dropped_frames.fetch_add(1);
        return;
    }
    // The LVGL timer owns canvas changes. Keep at most one decoded frame
    // pending so the AVI callback never blocks on the LVGL mutex and closing
    // the app cannot deadlock behind a frame presentation.
    if (self->_pending_buffer_index.load() >= 0) {
        self->_dropped_frames.fetch_add(1);
        return;
    }
    if (self->initJpeg() != ESP_OK) {
        self->_dropped_frames.fetch_add(1);
        return;
    }

    jpeg_dec_io_t io = {
        .inbuf = data->data,
        .inbuf_len = static_cast<int>(data->data_bytes),
        .outbuf = nullptr,
    };
    jpeg_dec_header_info_t info = {};
    if (jpeg_dec_parse_header(self->_jpeg, &io, &info) != JPEG_ERR_OK) {
        self->_dropped_frames.fetch_add(1);
        return;
    }
    int output_bytes = 0;
    if (jpeg_dec_get_outbuf_len(self->_jpeg, &output_bytes) != JPEG_ERR_OK ||
            self->ensureFrameBuffers(
                static_cast<uint16_t>(info.width),
                static_cast<uint16_t>(info.height),
                static_cast<size_t>(output_bytes)
            ) != ESP_OK) {
        self->_last_error.store(ESP_ERR_INVALID_SIZE);
        self->_dropped_frames.fetch_add(1);
        return;
    }

    const int decode_index = self->_display_buffer_index.load() == 0 ? 1 : 0;
    io.outbuf = self->_frame_buffers[decode_index];
    if (jpeg_dec_process(self->_jpeg, &io) != JPEG_ERR_OK) {
        self->_dropped_frames.fetch_add(1);
        return;
    }
    if (!self->_ui_accept_frames.load() || self->_stop_requested.load() ||
            self->_pending_buffer_index.load() >= 0) {
        return;
    }
    self->_last_queued_us = now_us;
    self->_pending_buffer_index.store(decode_index);
}

void VideoPlayer::audioCallback(frame_data_t *data, void *arg)
{
    auto *self = static_cast<VideoPlayer *>(arg);
    if (!self) {
        return;
    }
    AudioCallbackUse callback_use(self->_audio_callback_users);
    if (!self->_audio_ready.load() || self->_audio_disable_requested.load() ||
            self->_stop_requested.load() ||
            !data || !data->data || data->data_bytes == 0) {
        return;
    }
    if (data->audio_info.format != FORMAT_PCM ||
            data->audio_info.bits_per_sample != 16 ||
            data->audio_info.channel == 0 || data->audio_info.channel > 2 ||
            !is_supported_pcm_rate(data->audio_info.sample_rate)) {
        if (!self->_audio_disable_requested.exchange(true)) {
            ESP_UTILS_LOGW(
                "Unsupported AVI audio: format=%d, %lu Hz/%u-bit/%uch; playing this file without sound",
                static_cast<int>(data->audio_info.format),
                static_cast<unsigned long>(data->audio_info.sample_rate),
                static_cast<unsigned>(data->audio_info.bits_per_sample),
                static_cast<unsigned>(data->audio_info.channel)
            );
        }
        return;
    }
    size_t written = 0;
    esp_err_t ret = bsp_extra_i2s_write(data->data, data->data_bytes, &written, 1000);
    if (ret != ESP_OK || written != data->data_bytes) {
        if (!self->_audio_disable_requested.exchange(true)) {
            ESP_UTILS_LOGW(
                "AVI PCM write failed; requesting video-only playback: %s (%u/%u)",
                esp_err_to_name(ret),
                static_cast<unsigned>(written),
                static_cast<unsigned>(data->data_bytes)
            );
        }
    }
}

void VideoPlayer::audioClockCallback(uint32_t rate, uint32_t bits, uint32_t channels, void *arg)
{
    auto *self = static_cast<VideoPlayer *>(arg);
    if (!self) {
        return;
    }
    AudioCallbackUse callback_use(self->_audio_callback_users);
    if (!self->_audio_ready.load() || self->_audio_disable_requested.load() ||
            self->_stop_requested.load()) {
        return;
    }
    ESP_UTILS_LOGI(
        "AVI audio: %lu Hz/%lu-bit/%luch, volume=%d",
        static_cast<unsigned long>(rate),
        static_cast<unsigned long>(bits),
        static_cast<unsigned long>(channels),
        bsp_extra_codec_volume_get()
    );
    if (bits != 16 || channels == 0 || channels > 2 || !is_supported_pcm_rate(rate)) {
        if (!self->_audio_disable_requested.exchange(true)) {
            ESP_UTILS_LOGW(
                "AVI audio disabled: PCM must be 16-bit mono/stereo at a supported sample rate"
            );
        }
        return;
    }
    const i2s_slot_mode_t mode = channels <= 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;
    esp_err_t ret = bsp_extra_codec_set_fs(
        rate,
        bits,
        mode
    );
    if (ret != ESP_OK) {
        if (!self->_audio_disable_requested.exchange(true)) {
            ESP_UTILS_LOGW(
                "AVI audio clock failed; requesting video-only playback: %s",
                esp_err_to_name(ret)
            );
        }
    }
}

void VideoPlayer::playEndCallback(void *arg)
{
    auto *self = static_cast<VideoPlayer *>(arg);
    if (self) {
        self->_playing.store(false);
    }
}

void VideoPlayer::workerTask(void *arg)
{
    auto *self = static_cast<VideoPlayer *>(arg);
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    esp_err_t result = storage_service_acquire(&self->_storage_lease);
    if (result == ESP_OK) {
        self->_state.store(State::Scanning);
        result = self->scanVideos();
    }
    if (result == ESP_ERR_NOT_FOUND) {
        self->_state.store(State::Empty);
    }
    if (result == ESP_OK) {
        self->_state.store(State::Preparing);
        result = self->initJpeg();
    }

    if (result == ESP_OK) {
        avi_player_config_t config = {
            .buffer_size = AVI_BUFFER_SIZE,
            .video_cb = videoCallback,
            // Keep callbacks installed so a later file can recover audio after
            // an unsupported track or another app releases the codec.
            .audio_cb = audioCallback,
            .audio_set_clock_cb = audioClockCallback,
            .avi_play_end_cb = playEndCallback,
            .priority = 6,
            .coreID = APP_CPU_NUM,
            .user_data = self,
            .stack_size = 16 * 1024,
            .stack_in_psram = true,
        };
        result = avi_player_init(config, &self->_avi);
    }

    int index = 0;
    bool paused = false;
    while (result == ESP_OK && !self->_stop_requested.load()) {
        if (self->_audio_disable_requested.load()) {
            (void)self->processAudioDisableRequest(AUDIO_CALLBACK_DRAIN_TIMEOUT, false);
        }
        const int count = self->_file_count.load();
        if (count <= 0) {
            result = ESP_ERR_NOT_FOUND;
            break;
        }
        index = (index % count + count) % count;
        self->_current_index.store(index);

        if (paused) {
            self->_state.store(State::Paused);
            Command command = self->_command.exchange(Command::None);
            if (command == Command::Play) {
                paused = false;
            } else if (command == Command::Next) {
                index = (index + 1) % count;
                paused = false;
            } else if (command == Command::Previous) {
                index = (index + count - 1) % count;
                paused = false;
            } else {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            continue;
        }

        ESP_UTILS_LOGI("Playing AVI: %s", self->_files[index]);
        // Audio failure applies to one file. Retry at each playback boundary;
        // the clock callback configures the codec for the new track.
        (void)self->prepareAudioForPlayback();
        self->_state.store(State::Playing);
        self->_playing.store(true);
        result = avi_player_play_from_file(self->_avi, self->_files[index]);
        if (result != ESP_OK) {
            self->_playing.store(false);
            break;
        }

        int advance = 1;
        while (self->_playing.load() && !self->_stop_requested.load()) {
            if (self->_audio_disable_requested.load()) {
                (void)self->processAudioDisableRequest(AUDIO_CALLBACK_DRAIN_TIMEOUT, false);
            }
            Command command = self->_command.exchange(Command::None);
            if (command == Command::Pause) {
                paused = true;
                advance = 0;
                (void)avi_player_play_stop(self->_avi);
            } else if (command == Command::Next) {
                advance = 1;
                (void)avi_player_play_stop(self->_avi);
            } else if (command == Command::Previous) {
                advance = -1;
                (void)avi_player_play_stop(self->_avi);
            }
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        index = (index + advance + count) % count;
    }

    const bool cleanup_ok = self->cleanupPlaybackResources();
    // Keep the final canvas frame alive. close()/pause()/the next run detach
    // the canvas before freeing these buffers, so a natural EOF/error cannot
    // leave LVGL pointing at freed PSRAM.

    if (!cleanup_ok) {
        self->_state.store(State::Error);
    } else if (!self->_stop_requested.load() && self->_state.load() != State::Empty) {
        self->_last_error.store(result);
        self->_state.store(State::Error);
    } else if (self->_stop_requested.load()) {
        self->_state.store(State::Idle);
    }
    self->_worker_task.store(nullptr);
    self->_worker_stopped.store(true);
    vTaskDelete(nullptr);
}

void VideoPlayer::controlEvent(lv_event_t *event)
{
    auto *self = static_cast<VideoPlayer *>(lv_event_get_user_data(event));
    if (!self || lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    self->showChrome();
    auto *target = static_cast<lv_obj_t *>(lv_event_get_target(event));
    Command command = static_cast<Command>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(target)));
    if (command == Command::Pause && self->_state.load() == State::Paused) {
        command = Command::Play;
    }
    self->_command.store(command);
}

void VideoPlayer::chromeEvent(lv_event_t *event)
{
    auto *self = static_cast<VideoPlayer *>(lv_event_get_user_data(event));
    if (!self || lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (self->_state.load() != State::Playing) {
        self->showChrome();
        return;
    }
    if (self->_chrome_visible) {
        self->setChromeVisible(false);
        self->_chrome_hide_at_ms = 0;
    } else {
        self->showChrome();
    }
}

void VideoPlayer::uiTimer(lv_timer_t *timer)
{
    auto *self = static_cast<VideoPlayer *>(lv_timer_get_user_data(timer));
    if (self) {
        self->updateUi();
    }
}

void VideoPlayer::updateUi()
{
    const int pending_index = _pending_buffer_index.load();
    if (pending_index >= 0 && pending_index < 2 && _canvas && _ui_accept_frames.load()) {
        // Publish the new display owner before reopening the other buffer to
        // the decoder. This ordering prevents the callback from decoding into
        // the frame that the canvas is about to display.
        _display_buffer_index.store(pending_index);
        lv_canvas_set_buffer(
            _canvas,
            _frame_buffers[pending_index],
            _video_width,
            _video_height,
            LV_COLOR_FORMAT_RGB565
        );
        lv_obj_set_size(_canvas, _video_width, _video_height);
        lv_obj_align(_canvas, LV_ALIGN_CENTER, 0, 0);
        lv_obj_invalidate(_canvas);
        _pending_buffer_index.store(-1);
        _decoded_frames.fetch_add(1);
        _frame_presented = true;
        if (_hero_icon) {
            lv_obj_add_flag(_hero_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }

    const State state = _state.load();
    const int count = _file_count.load();
    const int index = _current_index.load();
    if (state != _last_ui_state) {
        if (state == State::Playing) {
            showChrome();
        } else {
            setChromeVisible(true);
            _chrome_hide_at_ms = 0;
        }
        _last_ui_state = state;
    }
    if (state == State::Playing && _chrome_visible && _chrome_hide_at_ms != 0 &&
            static_cast<int32_t>(lv_tick_get() - _chrome_hide_at_ms) >= 0) {
        setChromeVisible(false);
        _chrome_hide_at_ms = 0;
    }
    updateControlState(state, count);

    if (_counter_label) {
        lv_label_set_text_fmt(
            _counter_label,
            "%d / %d",
            count ? index + 1 : 0,
            count
        );
    }
    if (_play_button_label) {
        lv_label_set_text(_play_button_label, state == State::Paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    }
    if (_status_label) {
        const char *text = nullptr;
        switch (state) {
        case State::Mounting: text = "Mounting SD card..."; break;
        case State::Scanning: text = "Scanning AVI videos..."; break;
        case State::Preparing: text = "Preparing video..."; break;
        case State::Paused: text = "Paused"; break;
        case State::Empty:
            text = "No AVI videos\nCopy MJPEG AVI to /sdcard/video";
            break;
        case State::Error:
            text = "Cannot play this AVI\nUse MJPEG video + PCM audio";
            break;
        case State::Stopping: text = "Closing..."; break;
        case State::Idle: text = "Stopped"; break;
        case State::Playing: default: break;
        }
        if (text) {
            lv_label_set_text(_status_label, text);
            lv_obj_remove_flag(_status_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(_status_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

} // namespace esp_brookesia::apps
