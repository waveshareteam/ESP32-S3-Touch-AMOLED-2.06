/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "chat_history.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "storage_service.h"

#define CHAT_HISTORY_QUEUE_DEPTH 8
#define CHAT_HISTORY_ROLE_BYTES  16
#define CHAT_HISTORY_TEXT_BYTES  512
#define CHAT_HISTORY_RECORD_BYTES \
    (96U + CHAT_HISTORY_ROLE_BYTES * 6U + CHAT_HISTORY_TEXT_BYTES * 6U)

typedef struct {
    char role[CHAT_HISTORY_ROLE_BYTES];
    char text[CHAT_HISTORY_TEXT_BYTES];
} chat_history_message_t;

static const char *TAG = "ChatHistory";
static QueueHandle_t s_queue;
static bool s_initialized;
static atomic_bool s_enabled;

static void copy_string(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strlcpy(dst, src, dst_size);

    // strlcpy is byte-oriented.  If a long Chinese response is truncated in
    // the middle of a UTF-8 sequence, trim that incomplete final code point so
    // each JSONL record remains valid UTF-8.
    const size_t length = strlen(dst);
    if (length == 0 || src[length] == '\0') {
        return;
    }
    size_t start = length - 1;
    while (start > 0 && (((unsigned char)dst[start] & 0xc0U) == 0x80U)) {
        --start;
    }
    const unsigned char lead = (unsigned char)dst[start];
    size_t expected = 1;
    if ((lead & 0xe0U) == 0xc0U) {
        expected = 2;
    } else if ((lead & 0xf0U) == 0xe0U) {
        expected = 3;
    } else if ((lead & 0xf8U) == 0xf0U) {
        expected = 4;
    }
    if (length - start < expected) {
        dst[start] = '\0';
    }
}

static void load_enabled_preference(void)
{
    nvs_handle_t handle = 0;
    uint8_t value = 0;
    esp_err_t ret = nvs_open("chat_history", NVS_READONLY, &handle);
    if (ret == ESP_OK) {
        if (nvs_get_u8(handle, "enabled", &value) != ESP_OK) {
            value = 0;
        }
        nvs_close(handle);
    }
    atomic_store(&s_enabled, value != 0);
}

static esp_err_t persist_enabled_preference(bool enabled)
{
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open("chat_history", NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_u8(handle, "enabled", enabled ? 1 : 0);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

static esp_err_t ensure_history_directories(void)
{
    const char *paths[] = {
        STORAGE_SERVICE_MOUNT_POINT "/Waveshare",
        STORAGE_SERVICE_MOUNT_POINT "/Waveshare/AIChats",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        if (mkdir(paths[i], 0775) != 0 && errno != EEXIST) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static bool append_record_bytes(
    char *record,
    size_t record_capacity,
    size_t *record_length,
    const char *bytes,
    size_t bytes_length
)
{
    if (!record || !record_length || !bytes || *record_length >= record_capacity ||
        bytes_length > record_capacity - *record_length - 1) {
        return false;
    }
    memcpy(record + *record_length, bytes, bytes_length);
    *record_length += bytes_length;
    record[*record_length] = '\0';
    return true;
}

static bool append_json_string(
    char *record,
    size_t record_capacity,
    size_t *record_length,
    const char *text
)
{
    static const char hex_digits[] = "0123456789abcdef";
    if (!append_record_bytes(record, record_capacity, record_length, "\"", 1)) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; ++p) {
        switch (*p) {
        case '"':
            if (!append_record_bytes(record, record_capacity, record_length, "\\\"", 2)) return false;
            break;
        case '\\':
            if (!append_record_bytes(record, record_capacity, record_length, "\\\\", 2)) return false;
            break;
        case '\b':
            if (!append_record_bytes(record, record_capacity, record_length, "\\b", 2)) return false;
            break;
        case '\f':
            if (!append_record_bytes(record, record_capacity, record_length, "\\f", 2)) return false;
            break;
        case '\n':
            if (!append_record_bytes(record, record_capacity, record_length, "\\n", 2)) return false;
            break;
        case '\r':
            if (!append_record_bytes(record, record_capacity, record_length, "\\r", 2)) return false;
            break;
        case '\t':
            if (!append_record_bytes(record, record_capacity, record_length, "\\t", 2)) return false;
            break;
        default:
            if (*p < 0x20) {
                const char escaped[] = {
                    '\\', 'u', '0', '0', hex_digits[*p >> 4], hex_digits[*p & 0x0fU],
                };
                if (!append_record_bytes(
                        record, record_capacity, record_length, escaped, sizeof(escaped))) {
                    return false;
                }
            } else {
                const char value = (char)*p;
                if (!append_record_bytes(record, record_capacity, record_length, &value, 1)) {
                    return false;
                }
            }
            break;
        }
    }
    return append_record_bytes(record, record_capacity, record_length, "\"", 1);
}

static esp_err_t build_record(
    const chat_history_message_t *message,
    char **record_out,
    size_t *record_length_out
)
{
    if (!message || !record_out || !record_length_out) {
        return ESP_ERR_INVALID_ARG;
    }

    char *record = malloc(CHAT_HISTORY_RECORD_BYTES);
    if (!record) {
        return ESP_ERR_NO_MEM;
    }

    const long long timestamp_ms = (long long)time(NULL) * 1000LL;
    const int prefix_length = snprintf(
        record,
        CHAT_HISTORY_RECORD_BYTES,
        "{\"timestamp_ms\":%lld,\"role\":",
        timestamp_ms
    );
    if (prefix_length < 0 || (size_t)prefix_length >= CHAT_HISTORY_RECORD_BYTES) {
        free(record);
        return ESP_FAIL;
    }

    size_t record_length = (size_t)prefix_length;
    bool ok = append_json_string(
        record, CHAT_HISTORY_RECORD_BYTES, &record_length, message->role);
    ok = ok && append_record_bytes(
        record,
        CHAT_HISTORY_RECORD_BYTES,
        &record_length,
        ",\"text\":",
        sizeof(",\"text\":") - 1
    );
    ok = ok && append_json_string(
        record, CHAT_HISTORY_RECORD_BYTES, &record_length, message->text);
    ok = ok && append_record_bytes(
        record, CHAT_HISTORY_RECORD_BYTES, &record_length, "}\n", 2);
    if (!ok) {
        free(record);
        return ESP_ERR_INVALID_SIZE;
    }

    *record_out = record;
    *record_length_out = record_length;
    return ESP_OK;
}

static void log_append_rollback(
    const char *path,
    off_t append_start,
    const char *failed_step,
    int operation_errno
)
{
    if (truncate(path, append_start) == 0) {
        ESP_LOGW(
            TAG,
            "History %s failed (%s); rolled back to %lld bytes",
            failed_step,
            strerror(operation_errno),
            (long long)append_start
        );
        return;
    }

    const int rollback_errno = errno;
    ESP_LOGE(
        TAG,
        "History %s failed (%s); rollback to %lld bytes failed (%s)",
        failed_step,
        strerror(operation_errno),
        (long long)append_start,
        strerror(rollback_errno)
    );
}

static void make_session_path(char *path, size_t path_size, uint32_t generation)
{
    time_t now = time(NULL);
    struct tm local = {0};
    localtime_r(&now, &local);
    if (local.tm_year + 1900 >= 2024) {
        snprintf(
            path,
            path_size,
            STORAGE_SERVICE_MOUNT_POINT
            "/Waveshare/AIChats/session-%04d%02d%02d-%02d%02d%02d-g%lu.jsonl",
            local.tm_year + 1900,
            local.tm_mon + 1,
            local.tm_mday,
            local.tm_hour,
            local.tm_min,
            local.tm_sec,
            (unsigned long)generation
        );
    } else {
        snprintf(
            path,
            path_size,
            STORAGE_SERVICE_MOUNT_POINT "/Waveshare/AIChats/session-boot-%lld-g%lu.jsonl",
            (long long)(esp_timer_get_time() / 1000),
            (unsigned long)generation
        );
    }
}

static esp_err_t write_message(const chat_history_message_t *message)
{
    char *record = NULL;
    size_t record_length = 0;
    esp_err_t ret = build_record(message, &record, &record_length);
    if (ret != ESP_OK) {
        return ret;
    }

    storage_service_lease_t lease = {0};
    ret = storage_service_acquire(&lease);
    if (ret != ESP_OK) {
        free(record);
        return ret;
    }

    storage_service_info_t info = {0};
    ret = storage_service_get_info(&info);
    if (ret != ESP_OK || ensure_history_directories() != ESP_OK) {
        storage_service_release(&lease);
        free(record);
        return ret == ESP_OK ? ESP_FAIL : ret;
    }

    static uint32_t session_generation;
    static char session_path[192];
    if (session_path[0] == '\0' || session_generation != info.generation) {
        make_session_path(session_path, sizeof(session_path), info.generation);
        session_generation = info.generation;
    }

    FILE *file = fopen(session_path, "ab");
    if (!file) {
        storage_service_release(&lease);
        free(record);
        return ESP_FAIL;
    }

    const int file_descriptor = fileno(file);
    struct stat file_status = {0};
    if (file_descriptor < 0 || fstat(file_descriptor, &file_status) != 0 || file_status.st_size < 0) {
        const int status_errno = errno ? errno : EIO;
        if (fclose(file) != 0) {
            ESP_LOGW(TAG, "History close after stat failure also failed: %s", strerror(errno));
        }
        storage_service_release(&lease);
        free(record);
        ESP_LOGW(TAG, "History append position unavailable: %s", strerror(status_errno));
        return ESP_FAIL;
    }

    const off_t append_start = file_status.st_size;
    const char *failed_step = NULL;
    int operation_errno = 0;

    errno = 0;
    if (fwrite(record, record_length, 1, file) != 1) {
        failed_step = "fwrite";
        operation_errno = errno ? errno : EIO;
    } else if (fflush(file) != 0) {
        failed_step = "fflush";
        operation_errno = errno ? errno : EIO;
    } else if (fsync(file_descriptor) != 0) {
        failed_step = "fsync";
        operation_errno = errno ? errno : EIO;
    }

    errno = 0;
    const int close_result = fclose(file);
    const int close_errno = errno ? errno : EIO;
    if (close_result != 0 && !failed_step) {
        failed_step = "fclose";
        operation_errno = close_errno;
    }

    if (failed_step) {
        // fclose is deliberately completed before truncating.  Otherwise a
        // pending stdio buffer could append bytes again after an early
        // ftruncate.  ESP-IDF's FatFs VFS implements truncate() for SD cards.
        log_append_rollback(session_path, append_start, failed_step, operation_errno);
    }

    storage_service_release(&lease);
    free(record);
    return failed_step ? ESP_FAIL : ESP_OK;
}

static void writer_task(void *arg)
{
    (void)arg;
    chat_history_message_t message;
    while (true) {
        if (xQueueReceive(s_queue, &message, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!atomic_load(&s_enabled)) {
            continue;
        }
        esp_err_t ret = write_message(&message);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "History message not stored: %s", esp_err_to_name(ret));
        }
    }
}

esp_err_t chat_history_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    load_enabled_preference();
    s_queue = xQueueCreate(CHAT_HISTORY_QUEUE_DEPTH, sizeof(chat_history_message_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(writer_task, "chat_history", 4096, NULL, 1, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "SD text history %s", atomic_load(&s_enabled) ? "enabled" : "disabled");
    return ESP_OK;
}

esp_err_t chat_history_set_enabled(bool enabled)
{
    esp_err_t ret = persist_enabled_preference(enabled);
    if (ret == ESP_OK) {
        atomic_store(&s_enabled, enabled);
    }
    return ret;
}

bool chat_history_is_enabled(void)
{
    return atomic_load(&s_enabled);
}

esp_err_t chat_history_append(const char *role, const char *text)
{
    if (!s_initialized || !s_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!atomic_load(&s_enabled) || !text || text[0] == '\0') {
        return ESP_OK;
    }
    chat_history_message_t message = {0};
    copy_string(message.role, sizeof(message.role), role ? role : "unknown");
    copy_string(message.text, sizeof(message.text), text);
    return xQueueSend(s_queue, &message, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}
