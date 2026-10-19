/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "storage_service.h"
#include "storage_service_ui.hpp"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class Gallery : public systems::phone::App {
public:
    static Gallery *requestInstance(
        bool use_status_bar = false,
        bool use_navigation_bar = false
    );
    ~Gallery() override;

protected:
    Gallery(bool use_status_bar, bool use_navigation_bar);

    bool run() override;
    bool back() override;
    bool close() override;
    bool init() override;
    bool deinit() override;
    bool pause() override;
    bool resume() override;

private:
    static constexpr size_t MAX_PHOTOS = 128;
    static constexpr size_t MAX_PATH_LENGTH = 320;
    static constexpr size_t STATUS_MESSAGE_LENGTH = 160;
    static constexpr size_t MAX_COMPRESSED_BYTES = 4 * 1024 * 1024;
    static constexpr size_t MAX_DECODED_BYTES = 2 * 1024 * 1024;
    // Decode for the target display. esp_new_jpeg still performs the
    // downscale in the worker, so the LVGL thread only needs to present one
    // RGB565 frame instead of scaling a small 320 x 250 thumbnail.
    static constexpr uint16_t DECODE_TARGET_WIDTH = BSP_LCD_H_RES;
    static constexpr uint16_t DECODE_TARGET_HEIGHT = BSP_LCD_V_RES;
    static constexpr uint32_t UI_REFRESH_PERIOD_MS = 50;
    static constexpr uint32_t SLIDESHOW_PERIOD_MS = 4000;
    static constexpr uint32_t CAPTION_VISIBLE_MS = 1800;
    static constexpr uint32_t WORKER_JOIN_TIMEOUT_MS = 3000;
    static constexpr uint32_t REQUEST_INDEX_MASK = 0xff;
    static constexpr uint32_t REQUEST_SERIAL_MASK = 0x00ffffff;

    enum class SessionState : uint8_t {
        Idle,
        Mounting,
        Scanning,
        Ready,
        NoCard,
        Empty,
        Error,
    };

    struct PhotoInfo {
        char path[MAX_PATH_LENGTH];
        uint32_t file_size;
        uint16_t width;
        uint16_t height;
    };

    struct DecodedFrame {
        uint8_t *pixels;
        size_t data_size;
        uint32_t request_token;
        uint16_t width;
        uint16_t height;
        uint8_t index;
        char message[STATUS_MESSAGE_LENGTH];
    };

    static Gallery *_instance;

    PhotoInfo *_photos;
    size_t _photo_count;
    size_t _skipped_count;
    storage_service_lease_t _storage_lease;
    brookesia::storage_ui::SdCardGate _sd_gate;

    std::atomic<bool> _stop_requested;
    std::atomic<bool> _worker_stopped;
    std::atomic<SessionState> _session_state;
    std::atomic<uint32_t> _request_token;
    std::atomic<DecodedFrame *> _pending_frame;
    SemaphoreHandle_t _worker_signal;
    SemaphoreHandle_t _session_mutex;
    char _session_message[STATUS_MESSAGE_LENGTH];

    lv_obj_t *_root;
    lv_obj_t *_status_label;
    lv_obj_t *_spinner;
    lv_obj_t *_caption_label;
    lv_obj_t *_image;
    lv_timer_t *_ui_timer;
    lv_image_dsc_t _image_descriptor;
    uint8_t *_current_pixels;

    SessionState _shown_state;
    uint32_t _next_request_serial;
    uint32_t _last_slide_tick;
    uint32_t _caption_shown_tick;
    size_t _selected_index;
    size_t _displayed_index;
    int32_t _image_max_width;
    int32_t _image_max_height;
    bool _first_request_sent;
    bool _loading;
    bool _slideshow_enabled;
    bool _paused;
    bool _viewer_enabled;
    bool _ignore_next_click;

    bool startWorker();
    bool stopWorker(TickType_t timeout);
    static void workerTask(void *arg);
    void workerLoop();

    void setSessionState(SessionState state, const char *message);
    bool scanPhotoDirectory(const char *directory, bool &directory_found);
    bool appendPhoto(const char *path, uint32_t file_size, uint16_t width, uint16_t height);
    static bool hasJpegExtension(const char *name);
    static bool readBaselineJpegInfo(const char *path, uint16_t &width, uint16_t &height);
    static int comparePhotos(const void *left, const void *right);

    DecodedFrame *decodePhoto(uint8_t index, uint32_t request_token);
    static DecodedFrame *allocateFrame(uint8_t index, uint32_t request_token);
    static void freeFrame(DecodedFrame *frame);
    void publishFrame(DecodedFrame *frame);

    void createUi();
    void releaseUi();
    void releasePhotosAndStorage();
    void clearCurrentFrame();
    void processUi();
    void applyFrame(DecodedFrame *frame);
    void requestPhoto(size_t index, bool manual_navigation);
    void navigate(int delta);
    void setControlsEnabled(bool enabled);
    void showStatus(const char *message, bool show_spinner, bool clear_photo);
    void updateCaption(size_t index);
    void updateSlideshowIcon();

    static void uiTimerCallback(lv_timer_t *timer);
    static void viewerEventCallback(lv_event_t *event);
};

} // namespace esp_brookesia::apps
