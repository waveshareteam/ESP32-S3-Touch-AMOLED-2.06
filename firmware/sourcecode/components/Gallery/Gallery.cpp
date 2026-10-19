/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Gallery.hpp"

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_brookesia.hpp"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_lib_utils.h"
#include "esp_log.h"

#ifdef ESP_UTILS_LOG_TAG
#undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:Gallery"

LV_IMG_DECLARE(img_app_gallery);

namespace {

constexpr char APP_NAME[] = "Gallery";
constexpr char PHOTO_DIRECTORY[] = STORAGE_SERVICE_MOUNT_POINT "/photos";
constexpr char PHOTO_DIRECTORY_COMPAT[] = STORAGE_SERVICE_MOUNT_POINT "/Photos";
constexpr size_t INTERNAL_FALLBACK_LIMIT = 512 * 1024;
constexpr size_t JPEG_HEADER_SCAN_LIMIT = 512 * 1024;

void *allocatePsramPreferred(size_t bytes, bool aligned)
{
    void *memory = aligned
                       ? heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                       : heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == nullptr && bytes <= INTERNAL_FALLBACK_LIMIT) {
        memory = aligned
                     ? heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
                     : heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return memory;
}

const char *jpegErrorName(jpeg_error_t error)
{
    switch (error) {
    case JPEG_ERR_OK:
        return "OK";
    case JPEG_ERR_NO_MEM:
        return "out of memory";
    case JPEG_ERR_NO_MORE_DATA:
        return "truncated data";
    case JPEG_ERR_INVALID_PARAM:
        return "invalid parameters";
    case JPEG_ERR_BAD_DATA:
        return "damaged data";
    case JPEG_ERR_UNSUPPORT_FMT:
        return "unsupported format";
    case JPEG_ERR_UNSUPPORT_STD:
        return "not baseline JPEG";
    case JPEG_ERR_FAIL:
    default:
        return "decoder failure";
    }
}

bool isStartOfFrameMarker(int marker)
{
    switch (marker) {
    case 0xc0:
    case 0xc1:
    case 0xc2:
    case 0xc3:
    case 0xc5:
    case 0xc6:
    case 0xc7:
    case 0xc9:
    case 0xca:
    case 0xcb:
    case 0xcd:
    case 0xce:
    case 0xcf:
        return true;
    default:
        return false;
    }
}

bool markerHasNoPayload(int marker)
{
    return marker == 0x01 || marker == 0xd8 || marker == 0xd9 ||
           (marker >= 0xd0 && marker <= 0xd7);
}

} // namespace

namespace esp_brookesia::apps {

Gallery *Gallery::_instance = nullptr;

Gallery *Gallery::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (_instance == nullptr) {
        _instance = new Gallery(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

Gallery::Gallery(bool use_status_bar, bool use_navigation_bar)
    : App(APP_NAME, &img_app_gallery, true, use_status_bar, use_navigation_bar),
      _photos(nullptr),
      _photo_count(0),
      _skipped_count(0),
      _storage_lease{},
      _stop_requested(false),
      _worker_stopped(true),
      _session_state(SessionState::Idle),
      _request_token(0),
      _pending_frame(nullptr),
      _worker_signal(xSemaphoreCreateBinary()),
      _session_mutex(xSemaphoreCreateMutex()),
      _session_message{},
      _root(nullptr),
      _status_label(nullptr),
      _spinner(nullptr),
      _caption_label(nullptr),
      _image(nullptr),
      _ui_timer(nullptr),
      _image_descriptor{},
      _current_pixels(nullptr),
      _shown_state(SessionState::Idle),
      _next_request_serial(0),
      _last_slide_tick(0),
      _caption_shown_tick(0),
      _selected_index(0),
      _displayed_index(SIZE_MAX),
      _image_max_width(DECODE_TARGET_WIDTH),
      _image_max_height(DECODE_TARGET_HEIGHT),
      _first_request_sent(false),
      _loading(false),
      _slideshow_enabled(false),
      _paused(false),
      _viewer_enabled(false),
      _ignore_next_click(false)
{
}

Gallery::~Gallery()
{
    if (!close()) {
        ESP_UTILS_LOGW("Waiting for Gallery worker before destruction");
        if (stopWorker(portMAX_DELAY)) {
            releaseUi();
            DecodedFrame *pending =
                _pending_frame.exchange(nullptr, std::memory_order_acq_rel);
            freeFrame(pending);
            releasePhotosAndStorage();
        }
    }
    if (_session_mutex != nullptr) {
        vSemaphoreDelete(_session_mutex);
        _session_mutex = nullptr;
    }
    if (_worker_signal != nullptr) {
        vSemaphoreDelete(_worker_signal);
        _worker_signal = nullptr;
    }
    _instance = nullptr;
}

bool Gallery::init()
{
    ESP_UTILS_LOGD("Init");
    return _session_mutex != nullptr && _worker_signal != nullptr;
}

bool Gallery::deinit()
{
    ESP_UTILS_LOGD("Deinit");
    return close();
}

bool Gallery::run()
{
    ESP_UTILS_LOGD("Run");
    if (!_sd_gate.ready()) {
        return _sd_gate.open(lv_screen_active(), [](void *context) {
            auto *app = static_cast<Gallery *>(context);
            if (!app->run()) (void)app->notifyCoreClosed();
        }, [](void *context) {
            (void)static_cast<Gallery *>(context)->back();
        }, this);
    }
    if (!_worker_stopped.load(std::memory_order_acquire) || _root != nullptr) {
        ESP_UTILS_LOGE("Gallery session is already active");
        return false;
    }

    _stop_requested.store(false, std::memory_order_release);
    _request_token.store(0, std::memory_order_release);
    _shown_state = SessionState::Idle;
    _next_request_serial = 0;
    _selected_index = 0;
    _displayed_index = SIZE_MAX;
    _first_request_sent = false;
    _loading = false;
    _slideshow_enabled = false;
    _paused = false;
    _viewer_enabled = false;
    _ignore_next_click = false;
    _last_slide_tick = lv_tick_get();
    _caption_shown_tick = _last_slide_tick;

    createUi();
    setSessionState(SessionState::Mounting, "Mounting SD card...");
    showStatus("Mounting SD card...", true, true);

    if (!startWorker()) {
        setSessionState(
            SessionState::Error,
            "Gallery worker could not start\nNot enough system memory"
        );
    }

    // Keep the app open when the card or worker is unavailable so the user gets
    // an actionable on-screen error instead of being bounced back to launcher.
    return true;
}

bool Gallery::back()
{
    ESP_UTILS_LOGD("Back");
    return notifyCoreClosed();
}

bool Gallery::close()
{
    ESP_UTILS_LOGD("Close");

    // The ordering here is intentional: no task/timer may retain a pointer to a
    // frame or path when the image/list is released, and the mount lease is the
    // final resource returned to storage_service.
    if (!stopWorker(pdMS_TO_TICKS(WORKER_JOIN_TIMEOUT_MS))) {
        ESP_UTILS_LOGE("Gallery worker did not stop in time; resources retained");
        return false;
    }
    releaseUi();

    DecodedFrame *pending = _pending_frame.exchange(nullptr, std::memory_order_acq_rel);
    freeFrame(pending);
    releasePhotosAndStorage();

    _session_state.store(SessionState::Idle, std::memory_order_release);
    _shown_state = SessionState::Idle;
    _request_token.store(0, std::memory_order_release);
    _slideshow_enabled = false;
    _paused = false;
    _sd_gate.close();
    return true;
}

bool Gallery::pause()
{
    ESP_UTILS_LOGD("Pause");
    _sd_gate.pause();
    _paused = true;
    if (_ui_timer != nullptr) {
        lv_timer_pause(_ui_timer);
    }
    return true;
}

bool Gallery::resume()
{
    ESP_UTILS_LOGD("Resume");
    _sd_gate.resume();
    _paused = false;
    _last_slide_tick = lv_tick_get();
    if (_ui_timer != nullptr) {
        lv_timer_reset(_ui_timer);
        lv_timer_resume(_ui_timer);
    }
    return true;
}

bool Gallery::startWorker()
{
    if (_worker_signal == nullptr) {
        return false;
    }
    while (xSemaphoreTake(_worker_signal, 0) == pdTRUE) {
    }
    _worker_stopped.store(false, std::memory_order_release);

    const BaseType_t result = xTaskCreate(
        workerTask,
        "gallery_worker",
        10 * 1024,
        this,
        4,
        nullptr
    );

    if (result != pdPASS) {
        _worker_stopped.store(true, std::memory_order_release);
        ESP_UTILS_LOGE("Create Gallery worker failed");
        return false;
    }
    return true;
}

bool Gallery::stopWorker(TickType_t timeout)
{
    _stop_requested.store(true, std::memory_order_release);
    if (_worker_signal != nullptr) {
        (void)xSemaphoreGive(_worker_signal);
    }

    const TickType_t started = xTaskGetTickCount();
    // Never force-delete while FatFs or the JPEG library owns private state.
    // A timed-out close retains the list, pending frame, and SD lease so a
    // later close can safely finish cleanup.
    while (!_worker_stopped.load(std::memory_order_acquire)) {
        if (_worker_signal != nullptr) {
            (void)xSemaphoreGive(_worker_signal);
        }
        if (timeout != portMAX_DELAY &&
                xTaskGetTickCount() - started >= timeout) {
            return false;
        }
        vTaskDelay(1);
    }
    return true;
}

void Gallery::workerTask(void *arg)
{
    static_cast<Gallery *>(arg)->workerLoop();
}

void Gallery::workerLoop()
{
    const esp_err_t acquire_result = storage_service_acquire(&_storage_lease);
    if (acquire_result != ESP_OK) {
        char message[STATUS_MESSAGE_LENGTH] = {};
        std::snprintf(
            message,
            sizeof(message),
            "No SD card available\nInsert a card and reopen Gallery\n%s",
            esp_err_to_name(acquire_result)
        );
        setSessionState(SessionState::NoCard, message);
        goto worker_exit;
    }

    if (_stop_requested.load(std::memory_order_acquire)) {
        goto worker_exit;
    }

    _photos = static_cast<PhotoInfo *>(heap_caps_calloc(
        MAX_PHOTOS,
        sizeof(PhotoInfo),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    ));
    if (_photos == nullptr) {
        _photos = static_cast<PhotoInfo *>(heap_caps_calloc(
            MAX_PHOTOS,
            sizeof(PhotoInfo),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        ));
    }
    if (_photos == nullptr) {
        setSessionState(SessionState::Error, "Not enough memory to build photo list");
        goto worker_exit;
    }

    setSessionState(SessionState::Scanning, "Scanning /sdcard/photos...");
    {
        bool directory_found = false;
        bool lower_directory_found = false;
        if (!scanPhotoDirectory(PHOTO_DIRECTORY, lower_directory_found)) {
            setSessionState(SessionState::Error, "SD card read error while scanning photos");
            goto worker_exit;
        }
        directory_found = lower_directory_found;

        // FAT normally treats these names as identical. Only try the capitalized
        // compatibility path when the canonical lowercase directory is absent,
        // which also prevents duplicate entries on case-insensitive volumes.
        if (!lower_directory_found) {
            bool compatibility_directory_found = false;
            if (!scanPhotoDirectory(PHOTO_DIRECTORY_COMPAT, compatibility_directory_found)) {
                setSessionState(SessionState::Error, "SD card read error while scanning Photos");
                goto worker_exit;
            }
            directory_found = compatibility_directory_found;
        }

        if (_stop_requested.load(std::memory_order_acquire)) {
            goto worker_exit;
        }

        if (_photo_count == 0) {
            if (!directory_found) {
                setSessionState(
                    SessionState::Empty,
                    "Photo folder not found\nCreate /sdcard/photos and add\nbaseline JPG/JPEG files"
                );
            } else if (_skipped_count != 0) {
                setSessionState(
                    SessionState::Empty,
                    "No compatible photos\nUse baseline JPG/JPEG files\nunder 4 MB each"
                );
            } else {
                setSessionState(
                    SessionState::Empty,
                    "No photos found\nCopy baseline JPG/JPEG files to\n/sdcard/photos"
                );
            }
            goto worker_exit;
        }
    }

    std::qsort(_photos, _photo_count, sizeof(PhotoInfo), comparePhotos);
    {
        char ready_message[STATUS_MESSAGE_LENGTH] = {};
        std::snprintf(
            ready_message,
            sizeof(ready_message),
            "%u photo%s ready",
            static_cast<unsigned>(_photo_count),
            _photo_count == 1 ? "" : "s"
        );
        setSessionState(SessionState::Ready, ready_message);
    }

    {
        uint32_t last_request_token = 0;
        while (!_stop_requested.load(std::memory_order_acquire)) {
            (void)xSemaphoreTake(_worker_signal, portMAX_DELAY);
            if (_stop_requested.load(std::memory_order_acquire)) {
                break;
            }

            const uint32_t request_token = _request_token.load(std::memory_order_acquire);
            if (request_token == 0 || request_token == last_request_token) {
                continue;
            }
            last_request_token = request_token;

            const uint8_t index = static_cast<uint8_t>(request_token & REQUEST_INDEX_MASK);
            DecodedFrame *frame = decodePhoto(index, request_token);

            // A decode that promotes the session to Error is unrecoverable for
            // this worker.  In particular, allocateFrame() cannot manufacture
            // an error frame when even its small decoder state allocation
            // fails.  Do not return to the blocking semaphore wait: reaching
            // worker_exit is what lets processUi() safely release the photo
            // list and the storage lease after _worker_stopped becomes true.
            // Per-photo failures remain recoverable because they return an
            // error frame without changing the Ready session state.
            if (_session_state.load(std::memory_order_acquire) == SessionState::Error) {
                freeFrame(frame);
                _stop_requested.store(true, std::memory_order_release);
                break;
            }
            if (frame != nullptr) {
                publishFrame(frame);
            }
        }
    }

worker_exit:
    _worker_stopped.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}

void Gallery::setSessionState(SessionState state, const char *message)
{
    if (_session_mutex == nullptr ||
            xSemaphoreTake(_session_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    std::snprintf(
        _session_message,
        sizeof(_session_message),
        "%s",
        message != nullptr ? message : ""
    );
    _session_state.store(state, std::memory_order_release);
    xSemaphoreGive(_session_mutex);
}

bool Gallery::scanPhotoDirectory(const char *directory, bool &directory_found)
{
    directory_found = false;
    errno = 0;
    DIR *dir = opendir(directory);
    if (dir == nullptr) {
        return errno == ENOENT;
    }
    directory_found = true;

    bool success = true;
    errno = 0;
    while (!_stop_requested.load(std::memory_order_acquire)) {
        dirent *entry = readdir(dir);
        if (entry == nullptr) {
            if (errno != 0) {
                success = false;
            }
            break;
        }
        if (entry->d_name[0] == '\0' || std::strcmp(entry->d_name, ".") == 0 ||
            std::strcmp(entry->d_name, "..") == 0 || entry->d_type == DT_DIR ||
            !hasJpegExtension(entry->d_name)) {
            continue;
        }

        char path[MAX_PATH_LENGTH];
        const int path_length = std::snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (path_length <= 0 || static_cast<size_t>(path_length) >= sizeof(path)) {
            ++_skipped_count;
            continue;
        }

        struct stat file_status = {};
        if (stat(path, &file_status) != 0 || !S_ISREG(file_status.st_mode) ||
            file_status.st_size <= 0 ||
            static_cast<uint64_t>(file_status.st_size) > MAX_COMPRESSED_BYTES) {
            ++_skipped_count;
            continue;
        }

        uint16_t width = 0;
        uint16_t height = 0;
        if (!readBaselineJpegInfo(path, width, height)) {
            ++_skipped_count;
            continue;
        }

        // esp_new_jpeg can downscale by at most 1/8. Reject a pathological
        // image whose smallest possible RGB565 result still exceeds our cap.
        const uint64_t minimum_width = (static_cast<uint64_t>(width) + 7) / 8;
        const uint64_t minimum_height = (static_cast<uint64_t>(height) + 7) / 8;
        if (minimum_width * minimum_height * 2 > MAX_DECODED_BYTES) {
            ++_skipped_count;
            continue;
        }

        if (!appendPhoto(
                path,
                static_cast<uint32_t>(file_status.st_size),
                width,
                height
            )) {
            ++_skipped_count;
            if (_photo_count >= MAX_PHOTOS) {
                break;
            }
        }
    }

    if (closedir(dir) != 0) {
        success = false;
    }
    return success;
}

bool Gallery::appendPhoto(const char *path, uint32_t file_size, uint16_t width, uint16_t height)
{
    if (_photos == nullptr || _photo_count >= MAX_PHOTOS || path == nullptr) {
        return false;
    }

    PhotoInfo &photo = _photos[_photo_count];
    const int copied = std::snprintf(photo.path, sizeof(photo.path), "%s", path);
    if (copied <= 0 || static_cast<size_t>(copied) >= sizeof(photo.path)) {
        photo.path[0] = '\0';
        return false;
    }
    photo.file_size = file_size;
    photo.width = width;
    photo.height = height;
    ++_photo_count;
    return true;
}

bool Gallery::hasJpegExtension(const char *name)
{
    if (name == nullptr) {
        return false;
    }
    const char *extension = std::strrchr(name, '.');
    return extension != nullptr &&
           (strcasecmp(extension, ".jpg") == 0 || strcasecmp(extension, ".jpeg") == 0);
}

bool Gallery::readBaselineJpegInfo(const char *path, uint16_t &width, uint16_t &height)
{
    width = 0;
    height = 0;

    FILE *file = std::fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }

    bool baseline = false;
    size_t scanned = 0;
    const int first = std::fgetc(file);
    const int second = std::fgetc(file);
    scanned += 2;
    if (first != 0xff || second != 0xd8) {
        std::fclose(file);
        return false;
    }

    while (scanned < JPEG_HEADER_SCAN_LIMIT) {
        int prefix = std::fgetc(file);
        ++scanned;
        while (prefix != EOF && prefix != 0xff && scanned < JPEG_HEADER_SCAN_LIMIT) {
            prefix = std::fgetc(file);
            ++scanned;
        }
        if (prefix == EOF) {
            break;
        }

        int marker = std::fgetc(file);
        ++scanned;
        while (marker == 0xff && scanned < JPEG_HEADER_SCAN_LIMIT) {
            marker = std::fgetc(file);
            ++scanned;
        }
        if (marker == EOF || marker == 0xda || marker == 0xd9) {
            break;
        }
        if (marker == 0x00 || markerHasNoPayload(marker)) {
            continue;
        }

        const int length_high = std::fgetc(file);
        const int length_low = std::fgetc(file);
        scanned += 2;
        if (length_high == EOF || length_low == EOF) {
            break;
        }
        const uint16_t segment_length = static_cast<uint16_t>((length_high << 8) | length_low);
        if (segment_length < 2) {
            break;
        }
        const size_t payload_length = segment_length - 2;

        if (isStartOfFrameMarker(marker)) {
            if (marker != 0xc0 || payload_length < 6) {
                break;
            }
            const int precision = std::fgetc(file);
            const int height_high = std::fgetc(file);
            const int height_low = std::fgetc(file);
            const int width_high = std::fgetc(file);
            const int width_low = std::fgetc(file);
            const int components = std::fgetc(file);
            if (precision != 8 || height_high == EOF || height_low == EOF ||
                width_high == EOF || width_low == EOF || components == EOF || components <= 0) {
                break;
            }
            height = static_cast<uint16_t>((height_high << 8) | height_low);
            width = static_cast<uint16_t>((width_high << 8) | width_low);
            baseline = width != 0 && height != 0;
            break;
        }

        if (payload_length > static_cast<size_t>(LONG_MAX) ||
            std::fseek(file, static_cast<long>(payload_length), SEEK_CUR) != 0) {
            break;
        }
        scanned += payload_length;
    }

    std::fclose(file);
    return baseline;
}

int Gallery::comparePhotos(const void *left, const void *right)
{
    const auto *left_photo = static_cast<const PhotoInfo *>(left);
    const auto *right_photo = static_cast<const PhotoInfo *>(right);
    return strcasecmp(left_photo->path, right_photo->path);
}

Gallery::DecodedFrame *Gallery::allocateFrame(uint8_t index, uint32_t request_token)
{
    auto *frame = static_cast<DecodedFrame *>(heap_caps_calloc(
        1,
        sizeof(DecodedFrame),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    ));
    if (frame == nullptr) {
        frame = static_cast<DecodedFrame *>(heap_caps_calloc(
            1,
            sizeof(DecodedFrame),
            MALLOC_CAP_DEFAULT
        ));
    }
    if (frame != nullptr) {
        frame->index = index;
        frame->request_token = request_token;
    }
    return frame;
}

void Gallery::freeFrame(DecodedFrame *frame)
{
    if (frame == nullptr) {
        return;
    }
    if (frame->pixels != nullptr) {
        heap_caps_free(frame->pixels);
        frame->pixels = nullptr;
    }
    heap_caps_free(frame);
}

Gallery::DecodedFrame *Gallery::decodePhoto(uint8_t index, uint32_t request_token)
{
    DecodedFrame *frame = allocateFrame(index, request_token);
    if (frame == nullptr) {
        setSessionState(SessionState::Error, "Not enough memory for photo decoder state");
        return nullptr;
    }

    if (index >= _photo_count || _photos == nullptr) {
        std::snprintf(frame->message, sizeof(frame->message), "Photo index is no longer valid");
        return frame;
    }

    const PhotoInfo photo = _photos[index];
    FILE *file = std::fopen(photo.path, "rb");
    if (file == nullptr) {
        const char *name = std::strrchr(photo.path, '/');
        name = name != nullptr ? name + 1 : photo.path;
        std::snprintf(
            frame->message,
            sizeof(frame->message),
            "Could not open\n%.128s",
            name
        );
        return frame;
    }

    uint8_t *input = static_cast<uint8_t *>(allocatePsramPreferred(photo.file_size, false));
    if (input == nullptr) {
        std::fclose(file);
        std::snprintf(
            frame->message,
            sizeof(frame->message),
            "Not enough PSRAM to load this photo\nTry a JPG under %u KB",
            static_cast<unsigned>(INTERNAL_FALLBACK_LIMIT / 1024)
        );
        return frame;
    }

    size_t bytes_read = 0;
    while (bytes_read < photo.file_size && !_stop_requested.load(std::memory_order_acquire)) {
        const size_t remaining = photo.file_size - bytes_read;
        const size_t chunk = remaining < 32 * 1024 ? remaining : 32 * 1024;
        const size_t current = std::fread(input + bytes_read, 1, chunk, file);
        bytes_read += current;
        if (current != chunk) {
            break;
        }
    }
    std::fclose(file);

    if (_stop_requested.load(std::memory_order_acquire) ||
        _request_token.load(std::memory_order_acquire) != request_token) {
        heap_caps_free(input);
        freeFrame(frame);
        return nullptr;
    }
    if (bytes_read != photo.file_size) {
        heap_caps_free(input);
        std::snprintf(frame->message, sizeof(frame->message), "Photo read failed or card was removed");
        return frame;
    }

    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;

    double scale = 1.0;
    if (photo.width > DECODE_TARGET_WIDTH || photo.height > DECODE_TARGET_HEIGHT) {
        const double width_scale = static_cast<double>(DECODE_TARGET_WIDTH) / photo.width;
        const double height_scale = static_cast<double>(DECODE_TARGET_HEIGHT) / photo.height;
        scale = width_scale < height_scale ? width_scale : height_scale;
        if (scale < 0.125) {
            scale = 0.125;
        }
    }
    if (scale < 0.999 && photo.width >= 8 && photo.height >= 8) {
        uint32_t scaled_width = static_cast<uint32_t>(std::ceil(photo.width * scale / 8.0)) * 8;
        uint32_t scaled_height = static_cast<uint32_t>(std::ceil(photo.height * scale / 8.0)) * 8;
        if (scaled_width < photo.width && scaled_height < photo.height &&
            scaled_width <= UINT16_MAX && scaled_height <= UINT16_MAX) {
            config.scale.width = static_cast<uint16_t>(scaled_width);
            config.scale.height = static_cast<uint16_t>(scaled_height);
        }
    }

    jpeg_dec_handle_t decoder = nullptr;
    jpeg_error_t jpeg_result = jpeg_dec_open(&config, &decoder);
    if (jpeg_result != JPEG_ERR_OK) {
        heap_caps_free(input);
        std::snprintf(
            frame->message,
            sizeof(frame->message),
            "JPEG decoder setup failed\n%s",
            jpegErrorName(jpeg_result)
        );
        return frame;
    }

    jpeg_dec_io_t io = {};
    io.inbuf = input;
    io.inbuf_len = static_cast<int>(photo.file_size);
    jpeg_dec_header_info_t output_info = {};
    jpeg_result = jpeg_dec_parse_header(decoder, &io, &output_info);

    int output_bytes = 0;
    if (jpeg_result == JPEG_ERR_OK) {
        jpeg_result = jpeg_dec_get_outbuf_len(decoder, &output_bytes);
    }
    if (jpeg_result != JPEG_ERR_OK || output_bytes <= 0 ||
        static_cast<size_t>(output_bytes) > MAX_DECODED_BYTES ||
        output_info.width == 0 || output_info.height == 0) {
        (void)jpeg_dec_close(decoder);
        heap_caps_free(input);
        std::snprintf(
            frame->message,
            sizeof(frame->message),
            "Photo cannot be decoded safely\n%s",
            jpeg_result == JPEG_ERR_OK ? "decoded image is too large" : jpegErrorName(jpeg_result)
        );
        return frame;
    }

    uint8_t *output = static_cast<uint8_t *>(allocatePsramPreferred(
        static_cast<size_t>(output_bytes),
        true
    ));
    if (output == nullptr) {
        (void)jpeg_dec_close(decoder);
        heap_caps_free(input);
        std::snprintf(frame->message, sizeof(frame->message), "Not enough memory for decoded photo");
        return frame;
    }

    io.outbuf = output;
    jpeg_result = jpeg_dec_process(decoder, &io);
    (void)jpeg_dec_close(decoder);
    heap_caps_free(input);

    if (_stop_requested.load(std::memory_order_acquire) ||
        _request_token.load(std::memory_order_acquire) != request_token) {
        heap_caps_free(output);
        freeFrame(frame);
        return nullptr;
    }
    if (jpeg_result != JPEG_ERR_OK) {
        heap_caps_free(output);
        std::snprintf(
            frame->message,
            sizeof(frame->message),
            "JPEG decode failed\n%s",
            jpegErrorName(jpeg_result)
        );
        return frame;
    }

    frame->pixels = output;
    frame->data_size = static_cast<size_t>(output_bytes);
    frame->width = output_info.width;
    frame->height = output_info.height;
    return frame;
}

void Gallery::publishFrame(DecodedFrame *frame)
{
    DecodedFrame *stale = _pending_frame.exchange(frame, std::memory_order_acq_rel);
    freeFrame(stale);
}

void Gallery::createUi()
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    _root = lv_obj_create(screen);
    lv_obj_remove_flag(_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(_root, LV_PCT(100), LV_PCT(100));
    lv_obj_center(_root);
    lv_obj_set_style_bg_color(_root, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(_root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(_root, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(_root, 0, LV_PART_MAIN);

    const lv_area_t visual_area = getVisualArea();
    const int32_t visual_width = lv_area_get_width(&visual_area);
    const int32_t visual_height = lv_area_get_height(&visual_area);
    _image_max_width = visual_width < DECODE_TARGET_WIDTH
                           ? visual_width
                           : DECODE_TARGET_WIDTH;
    _image_max_height = visual_height < DECODE_TARGET_HEIGHT
                            ? visual_height
                            : DECODE_TARGET_HEIGHT;

    // The 2.06-inch reference app is intentionally a photograph-first
    // viewer: one image owns the whole screen and a tap advances it.  Keep
    // that interaction, but leave the SD/JPEG work off the LVGL thread.
    lv_obj_add_flag(_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(_root, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(_root, viewerEventCallback, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(_root, viewerEventCallback, LV_EVENT_GESTURE, this);
    lv_obj_add_event_cb(_root, viewerEventCallback, LV_EVENT_LONG_PRESSED, this);

    _status_label = lv_label_create(_root);
    lv_obj_set_width(_status_label, visual_width * 78 / 100);
    lv_label_set_long_mode(_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(_status_label, lv_color_hex(0xe8eef8), LV_PART_MAIN);
    lv_obj_align(_status_label, LV_ALIGN_CENTER, 0, 24);

    _spinner = lv_spinner_create(_root);
    lv_obj_set_size(_spinner, 50, 50);
    lv_spinner_set_anim_params(_spinner, 900, 80);
    lv_obj_set_style_arc_width(_spinner, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_color(_spinner, lv_color_hex(0x27303a), LV_PART_MAIN);
    lv_obj_set_style_arc_width(_spinner, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(_spinner, lv_color_hex(0x25d9cb), LV_PART_INDICATOR);
    lv_obj_align(_spinner, LV_ALIGN_CENTER, 0, -44);

    _caption_label = lv_label_create(_root);
    lv_obj_set_width(_caption_label, visual_width - 52);
    lv_label_set_long_mode(_caption_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(_caption_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(_caption_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_caption_label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_caption_label, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(_caption_label, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(_caption_label, 8, LV_PART_MAIN);
    lv_obj_set_style_radius(_caption_label, 18, LV_PART_MAIN);
    lv_obj_align(_caption_label, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_add_flag(_caption_label, LV_OBJ_FLAG_HIDDEN);
    setControlsEnabled(false);

    _ui_timer = lv_timer_create(uiTimerCallback, UI_REFRESH_PERIOD_MS, this);
}

void Gallery::releaseUi()
{
    if (_ui_timer != nullptr) {
        lv_timer_delete(_ui_timer);
        _ui_timer = nullptr;
    }

    clearCurrentFrame();

    if (_root != nullptr && lv_obj_is_valid(_root)) {
        lv_obj_delete(_root);
    }
    _root = nullptr;
    _status_label = nullptr;
    _spinner = nullptr;
    _caption_label = nullptr;
    _viewer_enabled = false;
    _ignore_next_click = false;
}

void Gallery::releasePhotosAndStorage()
{
    if (_photos != nullptr) {
        heap_caps_free(_photos);
        _photos = nullptr;
    }
    _photo_count = 0;
    _skipped_count = 0;

    // This must remain after every file/frame/list release. It permits a safe
    // eject immediately after the app has fully relinquished the volume.
    storage_service_release(&_storage_lease);
}

void Gallery::clearCurrentFrame()
{
    if (_image_descriptor.data != nullptr) {
        lv_image_cache_drop(&_image_descriptor);
    }
    if (_image != nullptr && lv_obj_is_valid(_image)) {
        lv_obj_delete(_image);
    }
    _image = nullptr;

    std::memset(&_image_descriptor, 0, sizeof(_image_descriptor));
    if (_current_pixels != nullptr) {
        heap_caps_free(_current_pixels);
        _current_pixels = nullptr;
    }
    _displayed_index = SIZE_MAX;
}

void Gallery::processUi()
{
    SessionState state = SessionState::Idle;
    char session_message[STATUS_MESSAGE_LENGTH] = {};
    if (_session_mutex != nullptr &&
            xSemaphoreTake(_session_mutex, portMAX_DELAY) == pdTRUE) {
        state = _session_state.load(std::memory_order_acquire);
        std::snprintf(
            session_message,
            sizeof(session_message),
            "%s",
            _session_message
        );
        xSemaphoreGive(_session_mutex);
    }
    if (state != _shown_state) {
        _shown_state = state;
        switch (state) {
        case SessionState::Mounting:
        case SessionState::Scanning:
            setControlsEnabled(false);
            showStatus(session_message, true, true);
            break;
        case SessionState::Ready:
            setControlsEnabled(true);
            if (!_first_request_sent && _photo_count != 0) {
                _first_request_sent = true;
                requestPhoto(0, false);
            }
            break;
        case SessionState::NoCard:
        case SessionState::Empty:
        case SessionState::Error:
            setControlsEnabled(false);
            showStatus(session_message, false, true);
            break;
        case SessionState::Idle:
        default:
            break;
        }
    }

    const bool terminal_state =
        state == SessionState::NoCard || state == SessionState::Empty ||
        state == SessionState::Error;
    if (terminal_state &&
            _worker_stopped.load(std::memory_order_acquire) &&
            (_photos != nullptr || _storage_lease.active)) {
        DecodedFrame *pending =
            _pending_frame.exchange(nullptr, std::memory_order_acq_rel);
        freeFrame(pending);
        releasePhotosAndStorage();
    }

    DecodedFrame *frame = _pending_frame.exchange(nullptr, std::memory_order_acq_rel);
    if (frame != nullptr) {
        const uint32_t current_request = _request_token.load(std::memory_order_acquire);
        if (state == SessionState::Ready && frame->request_token == current_request &&
            frame->index == _selected_index) {
            applyFrame(frame);
        } else {
            freeFrame(frame);
        }
    }

    if (!_paused && _slideshow_enabled && !_loading && _photo_count > 1 &&
        _displayed_index != SIZE_MAX &&
        lv_tick_elaps(_last_slide_tick) >= SLIDESHOW_PERIOD_MS) {
        navigate(1);
    }

    if (_caption_label != nullptr &&
        !lv_obj_has_flag(_caption_label, LV_OBJ_FLAG_HIDDEN) &&
        state == SessionState::Ready && !_loading &&
        lv_tick_elaps(_caption_shown_tick) >= CAPTION_VISIBLE_MS) {
        lv_obj_add_flag(_caption_label, LV_OBJ_FLAG_HIDDEN);
    }
}

void Gallery::applyFrame(DecodedFrame *frame)
{
    _loading = false;
    if (frame->pixels == nullptr || frame->width == 0 || frame->height == 0) {
        clearCurrentFrame();
        showStatus(
            frame->message[0] != '\0' ? frame->message : "Photo decode failed",
            false,
            false
        );
        updateCaption(frame->index);
        freeFrame(frame);
        _last_slide_tick = lv_tick_get();
        return;
    }

    clearCurrentFrame();
    _current_pixels = frame->pixels;
    frame->pixels = nullptr;

    _image_descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    _image_descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
    _image_descriptor.header.flags = 0;
    _image_descriptor.header.w = frame->width;
    _image_descriptor.header.h = frame->height;
    _image_descriptor.header.stride = frame->width * 2;
    _image_descriptor.data_size = static_cast<uint32_t>(frame->data_size);
    _image_descriptor.data = _current_pixels;

    _image = lv_image_create(_root);
    lv_image_set_src(_image, &_image_descriptor);
    lv_image_set_antialias(_image, true);

    const uint32_t width_scale =
        static_cast<uint32_t>(_image_max_width) * LV_SCALE_NONE / frame->width;
    const uint32_t height_scale =
        static_cast<uint32_t>(_image_max_height) * LV_SCALE_NONE / frame->height;
    uint32_t image_scale = width_scale < height_scale ? width_scale : height_scale;
    if (image_scale == 0) {
        image_scale = 1;
    } else if (image_scale > LV_SCALE_NONE * 4U) {
        // Avoid pathological scaling for tiny JPEGs while still making normal
        // camera thumbnails useful on the target display.
        image_scale = LV_SCALE_NONE * 4U;
    }
    lv_image_set_scale(_image, image_scale);
    lv_obj_align(_image, LV_ALIGN_CENTER, 0, 0);
    lv_obj_move_to_index(_image, 0);

    lv_obj_add_flag(_status_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_caption_label, LV_OBJ_FLAG_HIDDEN);
    _displayed_index = frame->index;
    _ignore_next_click = false;
    updateCaption(frame->index);
    _caption_shown_tick = lv_tick_get();
    _last_slide_tick = lv_tick_get();
    freeFrame(frame);
}

void Gallery::requestPhoto(size_t index, bool manual_navigation)
{
    if (_photo_count == 0 || index >= _photo_count) {
        return;
    }

    _selected_index = index;
    _next_request_serial = (_next_request_serial + 1) & REQUEST_SERIAL_MASK;
    if (_next_request_serial == 0) {
        _next_request_serial = 1;
    }
    const uint32_t token = (_next_request_serial << 8) | static_cast<uint32_t>(index);
    _request_token.store(token, std::memory_order_release);
    _loading = true;
    _last_slide_tick = lv_tick_get();

    lv_obj_clear_flag(_spinner, LV_OBJ_FLAG_HIDDEN);
    if (_image == nullptr) {
        lv_label_set_text(_status_label, "Loading photo...");
        lv_obj_clear_flag(_status_label, LV_OBJ_FLAG_HIDDEN);
    }
    updateCaption(index);

    if (!_worker_stopped.load(std::memory_order_acquire) &&
            _worker_signal != nullptr) {
        (void)xSemaphoreGive(_worker_signal);
    } else {
        _loading = false;
        showStatus("Gallery worker is not available", false, true);
    }

    if (manual_navigation) {
        _last_slide_tick = lv_tick_get();
    }
}

void Gallery::navigate(int delta)
{
    if (_photo_count < 2) {
        return;
    }

    const int count = static_cast<int>(_photo_count);
    int next_index = static_cast<int>(_selected_index) + delta;
    while (next_index < 0) {
        next_index += count;
    }
    next_index %= count;
    requestPhoto(static_cast<size_t>(next_index), true);
}

void Gallery::setControlsEnabled(bool enabled)
{
    _viewer_enabled = enabled && _photo_count > 1;
}

void Gallery::showStatus(const char *message, bool show_spinner, bool clear_photo)
{
    if (clear_photo) {
        clearCurrentFrame();
    }
    if (_status_label != nullptr) {
        lv_label_set_text(_status_label, message != nullptr ? message : "");
        lv_obj_clear_flag(_status_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (_spinner != nullptr) {
        if (show_spinner) {
            lv_obj_clear_flag(_spinner, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(_spinner, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (_caption_label != nullptr) {
        lv_obj_add_flag(_caption_label, LV_OBJ_FLAG_HIDDEN);
    }
}

void Gallery::updateCaption(size_t index)
{
    if (_caption_label == nullptr || _photos == nullptr || index >= _photo_count) {
        return;
    }

    const char *name = std::strrchr(_photos[index].path, '/');
    name = name != nullptr ? name + 1 : _photos[index].path;
    char abbreviated_name[44];
    if (std::strlen(name) >= sizeof(abbreviated_name)) {
        std::snprintf(abbreviated_name, sizeof(abbreviated_name), "%.38s...", name);
        name = abbreviated_name;
    }
    lv_label_set_text_fmt(
        _caption_label,
        "%u / %u   %s",
        static_cast<unsigned>(index + 1),
        static_cast<unsigned>(_photo_count),
        name
    );
    lv_obj_clear_flag(_caption_label, LV_OBJ_FLAG_HIDDEN);
    _caption_shown_tick = lv_tick_get();
}

void Gallery::updateSlideshowIcon()
{
    if (_caption_label == nullptr) {
        return;
    }

    lv_label_set_text(
        _caption_label,
        _slideshow_enabled ? "Slideshow on" : "Slideshow off"
    );
    lv_obj_clear_flag(_caption_label, LV_OBJ_FLAG_HIDDEN);
    _caption_shown_tick = lv_tick_get();
}

void Gallery::uiTimerCallback(lv_timer_t *timer)
{
    auto *gallery = static_cast<Gallery *>(lv_timer_get_user_data(timer));
    if (gallery != nullptr) {
        gallery->processUi();
    }
}

void Gallery::viewerEventCallback(lv_event_t *event)
{
    auto *gallery = static_cast<Gallery *>(lv_event_get_user_data(event));
    if (gallery == nullptr || !gallery->_viewer_enabled) {
        return;
    }

    const lv_event_code_t code = lv_event_get_code(event);
    if (gallery->_loading) {
        // A gesture/long-press can be followed by a synthetic CLICKED event.
        // Consume that event even though the requested frame is still loading,
        // otherwise the next real tap after the decode would be lost.
        if (code == LV_EVENT_CLICKED && gallery->_ignore_next_click) {
            gallery->_ignore_next_click = false;
        }
        return;
    }

    if (code == LV_EVENT_GESTURE) {
        lv_indev_t *indev = lv_indev_active();
        if (indev == nullptr) {
            return;
        }
        const lv_dir_t direction = lv_indev_get_gesture_dir(indev);
        if (direction == LV_DIR_LEFT) {
            gallery->navigate(1);
            gallery->_ignore_next_click = true;
        } else if (direction == LV_DIR_RIGHT) {
            gallery->navigate(-1);
            gallery->_ignore_next_click = true;
        }
        return;
    }

    if (code == LV_EVENT_LONG_PRESSED) {
        gallery->_slideshow_enabled = !gallery->_slideshow_enabled;
        gallery->_last_slide_tick = lv_tick_get();
        gallery->_ignore_next_click = true;
        gallery->updateSlideshowIcon();
        return;
    }

    if (code == LV_EVENT_CLICKED) {
        if (gallery->_ignore_next_click) {
            gallery->_ignore_next_click = false;
            return;
        }
        // Match the reference Gallery: a simple tap advances to the next
        // image.  Horizontal swipes above additionally support both directions.
        gallery->navigate(1);
    }
}

} // namespace esp_brookesia::apps
