/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "StoragePage.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include "../Settings.hpp"
#include "SettingsUI.hpp"
#include "chat_history.h"
#include "bsp/esp-bsp.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "system_status.hpp"

#ifdef ESP_UTILS_LOG_TAG
#undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:StoragePage"
#include "esp_lib_utils.h"

namespace esp_brookesia::apps {

namespace {

constexpr size_t BENCHMARK_BYTES = 4U * 1024U * 1024U;
constexpr char DIAGNOSTICS_DIRECTORY[] = STORAGE_SERVICE_MOUNT_POINT "/Waveshare/Diagnostics";
constexpr char DIAGNOSTICS_PATH[] =
    STORAGE_SERVICE_MOUNT_POINT "/Waveshare/Diagnostics/device-diagnostics.txt";

lv_obj_t *getValueLabel(lv_obj_t *row)
{
    if (row == nullptr) {
        return nullptr;
    }
    const uint32_t child_count = lv_obj_get_child_count(row);
    return child_count > 0 ? lv_obj_get_child(row, static_cast<int32_t>(child_count - 1)) : nullptr;
}

void formatBytes(uint64_t bytes, char *buffer, size_t buffer_size)
{
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        snprintf(buffer, buffer_size, "%.2f GiB",
                 static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024ULL) {
        snprintf(buffer, buffer_size, "%.1f MiB",
                 static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        snprintf(buffer, buffer_size, "%.1f KiB", static_cast<double>(bytes) / 1024.0);
    } else {
        snprintf(buffer, buffer_size, "%llu B", static_cast<unsigned long long>(bytes));
    }
}

lv_obj_t *addActionButton(lv_obj_t *list, const char *text, lv_style_t &row_style,
                          lv_style_t &pressed_style, lv_event_cb_t callback, void *user_data)
{
    lv_obj_t *button = lv_list_add_button(list, nullptr, text);
    lv_obj_add_style(button, &row_style, LV_PART_MAIN);
    lv_obj_add_style(button, &pressed_style, LV_STATE_PRESSED);
    settings_ui::use_ellipsis_for_button_label(button);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    return button;
}

esp_err_t ensureDirectory(const char *path)
{
    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t exportDiagnostics(char *path, size_t path_size)
{
    storage_service_lease_t lease = {};
    esp_err_t result = storage_service_acquire(&lease);
    if (result != ESP_OK) {
        return result;
    }

    result = ensureDirectory(STORAGE_SERVICE_MOUNT_POINT "/Waveshare");
    if (result == ESP_OK) {
        result = ensureDirectory(DIAGNOSTICS_DIRECTORY);
    }

    storage_service_info_t storage = {};
    if (result == ESP_OK) {
        result = storage_service_get_info(&storage);
    }

    FILE *file = nullptr;
    if (result == ESP_OK) {
        file = fopen(DIAGNOSTICS_PATH, "wb");
        if (file == nullptr) {
            result = ESP_FAIL;
        }
    }

    if (file != nullptr) {
        esp_chip_info_t chip = {};
        esp_chip_info(&chip);
        brookesia::system_status::Snapshot status = {};
        const bool status_valid = brookesia::system_status::get_snapshot(status);

        bool ok = fprintf(file,
                          "Waveshare ESP32-S3-Touch-AMOLED-2.06 diagnostics\n"
                          "idf_version=%s\n"
                          "uptime_ms=%lld\n"
                          "reset_reason=%d\n"
                          "chip_cores=%u\n"
                          "chip_revision=%u\n"
                          "heap_free=%u\n"
                          "heap_minimum=%u\n"
                          "psram_free=%u\n"
                          "sd_state=%s\n"
                          "sd_error=%s\n"
                          "sd_card=%s\n"
                          "sd_capacity_bytes=%llu\n"
                          "sd_free_bytes=%llu\n"
                          "sd_bus_width=%lu\n"
                          "sd_frequency_khz=%lu\n"
                          "sd_generation=%lu\n"
                          "sd_active_leases=%lu\n",
                          esp_get_idf_version(),
                          static_cast<long long>(esp_timer_get_time() / 1000),
                          static_cast<int>(esp_reset_reason()),
                          static_cast<unsigned>(chip.cores),
                          static_cast<unsigned>(chip.revision),
                          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                          static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)),
                          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                          storage_service_state_name(storage.state),
                          esp_err_to_name(storage.last_error),
                          storage.card_name[0] ? storage.card_name : "unknown",
                          static_cast<unsigned long long>(storage.capacity_bytes),
                          static_cast<unsigned long long>(storage.free_bytes),
                          static_cast<unsigned long>(storage.bus_width),
                          static_cast<unsigned long>(storage.frequency_khz),
                          static_cast<unsigned long>(storage.generation),
                          static_cast<unsigned long>(storage.active_leases)) >= 0;

        if (status_valid) {
            ok = ok && fprintf(file,
                               "battery_valid=%d\n"
                               "battery_present=%d\n"
                               "battery_percent=%d\n"
                               "battery_charging=%d\n"
                               "wifi_enabled=%d\n"
                               "wifi_connected=%d\n"
                               "wifi_rssi=%d\n",
                               status.battery_valid,
                               status.battery_present,
                               status.battery_percent,
                               status.charging,
                               status.wifi_enabled,
                               status.wifi_connected,
                               status.wifi_rssi) >= 0;
        } else {
            ok = ok && fputs("system_status=unavailable\n", file) != EOF;
        }
        ok = ok && fprintf(file, "ai_chat_history_enabled=%d\n",
                           chat_history_is_enabled()) >= 0;
        ok = ok && fflush(file) == 0;
        ok = ok && fsync(fileno(file)) == 0;
        ok = fclose(file) == 0 && ok;
        file = nullptr;
        if (!ok) {
            result = ESP_FAIL;
        }
    }

    if (file != nullptr) {
        (void)fclose(file);
    }
    storage_service_release(&lease);

    if (result == ESP_OK) {
        strlcpy(path, DIAGNOSTICS_PATH, path_size);
    }
    return result;
}

} // namespace

StoragePage *StoragePage::_instance = nullptr;

StoragePage *StoragePage::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (_instance == nullptr) {
        _instance = new StoragePage(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

StoragePage::StoragePage(bool use_status_bar, bool use_navigation_bar)
    : App("Storage", nullptr, true, use_status_bar, use_navigation_bar)
{
}

StoragePage::~StoragePage()
{
    page_active = false;
}

bool StoragePage::run()
{
    ESP_UTILS_LOGD("StoragePage Run");
    page_active = false;
    const uint32_t generation = page_generation.fetch_add(1) + 1;
    (void)generation;
    destroyUi();
    lv_obj_clean(lv_screen_active());
    createUi();
    page_active = true;

    if (!ensureWorker()) {
        setBusy(false, "Worker unavailable");
        return false;
    }
    setBusy(true, "Checking card...");
    if (!enqueue(WorkerCommand::Refresh)) {
        setBusy(false, "Request queue busy");
    }
    return true;
}

bool StoragePage::back()
{
    ESP_UTILS_LOGD("StoragePage Back");
    Settings::requestInstance()->showRootPage();
    return true;
}

bool StoragePage::close()
{
    ESP_UTILS_LOGD("StoragePage Close");
    page_active = false;
    page_generation.fetch_add(1);
    destroyUi();
    return true;
}

bool StoragePage::ensureWorker()
{
    if (worker_task != nullptr && worker_queue != nullptr) {
        return true;
    }
    if (worker_queue == nullptr) {
        worker_queue = xQueueCreate(6, sizeof(WorkerRequest));
        if (worker_queue == nullptr) {
            return false;
        }
    }
    if (xTaskCreate(workerTask, "settings_sd", 7 * 1024, this, 3, &worker_task) != pdPASS) {
        vQueueDelete(worker_queue);
        worker_queue = nullptr;
        worker_task = nullptr;
        return false;
    }
    return true;
}

bool StoragePage::enqueue(WorkerCommand command, bool enabled)
{
    if (worker_queue == nullptr) {
        return false;
    }
    const WorkerRequest request = {
        .command = command,
        .generation = page_generation.load(),
        .enabled = enabled,
    };
    return xQueueSend(worker_queue, &request, 0) == pdTRUE;
}

void StoragePage::createUi()
{
    page_root = settings_ui::create_page(lv_screen_active());
    settings_ui::create_header(page_root, "Storage & SD", [](lv_event_t *event) {
        (void)event;
        lv_async_call([](void *parameter) {
            (void)parameter;
            Settings::requestInstance()->showRootPage();
        }, nullptr);
    });

    settings_ui::init_list_styles(style_list, style_row, style_section, style_pressed);
    list = settings_ui::create_content_list(page_root);
    lv_obj_add_style(list, &style_list, LV_PART_MAIN);

    settings_ui::add_section(list, "Card", style_section);
    status_value = getValueLabel(settings_ui::add_info_row(list, LV_SYMBOL_SD_CARD,
                                                            "Status", "Checking...", style_row));
    card_value = getValueLabel(settings_ui::add_info_row(list, nullptr, "Card", "--", style_row));
    capacity_value = getValueLabel(
        settings_ui::add_info_row(list, nullptr, "Capacity", "--", style_row));
    free_value = getValueLabel(
        settings_ui::add_info_row(list, nullptr, "Available", "--", style_row));
    bus_value = getValueLabel(
        settings_ui::add_info_row(list, nullptr, "Bus", "SDMMC 1-bit", style_row));
    frequency_value = getValueLabel(
        settings_ui::add_info_row(list, nullptr, "Clock", "--", style_row));
    (void)settings_ui::add_info_row(list, nullptr, "Detection", "Manual rescan", style_row);

    settings_ui::add_section(list, "Performance", style_section);
    write_value = getValueLabel(
        settings_ui::add_info_row(list, nullptr, "Sequential write", "--", style_row));
    read_value = getValueLabel(
        settings_ui::add_info_row(list, nullptr, "Sequential read", "--", style_row));
    crc_value = getValueLabel(
        settings_ui::add_info_row(list, nullptr, "CRC check", "Not tested", style_row));

    settings_ui::add_section(list, "AIChats", style_section);
    lv_obj_t *chat_row = lv_list_add_button(list, nullptr, nullptr);
    lv_obj_add_style(chat_row, &style_row, LV_PART_MAIN);
    lv_obj_clear_flag(chat_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *chat_label = lv_label_create(chat_row);
    lv_label_set_text(chat_label, "Save text history");
    lv_label_set_long_mode(chat_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(chat_label, 1);
    lv_obj_set_style_text_font(chat_label, &lv_font_montserrat_24, LV_PART_MAIN);
    chat_history_switch = lv_switch_create(chat_row);
    lv_obj_set_size(chat_history_switch, 64, 36);
    if (chat_history_is_enabled()) {
        lv_obj_add_state(chat_history_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(chat_history_switch, chatHistoryEventCallback, LV_EVENT_VALUE_CHANGED, this);

    settings_ui::add_section(list, "Actions", style_section);
    refresh_button = addActionButton(list, LV_SYMBOL_REFRESH "  Rescan card", style_row,
                                     style_pressed, actionEventCallback, this);
    benchmark_button = addActionButton(list, LV_SYMBOL_PLAY "  Run 4 MiB test", style_row,
                                       style_pressed, actionEventCallback, this);
    export_button = addActionButton(list, LV_SYMBOL_UPLOAD "  Export diagnostics", style_row,
                                    style_pressed, actionEventCallback, this);
    eject_button = addActionButton(list, LV_SYMBOL_EJECT "  Safe eject", style_row,
                                   style_pressed, actionEventCallback, this);
    operation_value = getValueLabel(
        settings_ui::add_info_row(list, nullptr, "Last operation", "Ready", style_row));
}

void StoragePage::destroyUi()
{
    if (page_root != nullptr) {
        lv_obj_delete(page_root);
        page_root = nullptr;
        list = nullptr;
        status_value = nullptr;
        card_value = nullptr;
        capacity_value = nullptr;
        free_value = nullptr;
        bus_value = nullptr;
        frequency_value = nullptr;
        operation_value = nullptr;
        write_value = nullptr;
        read_value = nullptr;
        crc_value = nullptr;
        refresh_button = nullptr;
        benchmark_button = nullptr;
        export_button = nullptr;
        eject_button = nullptr;
        chat_history_switch = nullptr;
        settings_ui::reset_list_styles(style_list, style_row, style_section, style_pressed);
    }
}

void StoragePage::setBusy(bool busy, const char *message)
{
    lv_obj_t *controls[] = {
        refresh_button, benchmark_button, export_button, eject_button, chat_history_switch,
    };
    for (lv_obj_t *control : controls) {
        if (control != nullptr) {
            if (busy) {
                lv_obj_add_state(control, LV_STATE_DISABLED);
            } else {
                lv_obj_remove_state(control, LV_STATE_DISABLED);
            }
        }
    }
    if (operation_value != nullptr && message != nullptr) {
        lv_label_set_text(operation_value, message);
    }
}

void StoragePage::actionEventCallback(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    auto *page = static_cast<StoragePage *>(lv_event_get_user_data(event));
    if (page == nullptr || !page->page_active) {
        return;
    }

    lv_obj_t *target = static_cast<lv_obj_t *>(lv_event_get_target(event));
    WorkerCommand command = WorkerCommand::Refresh;
    const char *message = "Checking card...";
    if (target == page->benchmark_button) {
        command = WorkerCommand::Benchmark;
        message = "Testing 4 MiB...";
    } else if (target == page->export_button) {
        command = WorkerCommand::ExportDiagnostics;
        message = "Exporting...";
    } else if (target == page->eject_button) {
        command = WorkerCommand::Eject;
        message = "Ejecting...";
    }

    page->setBusy(true, message);
    if (!page->enqueue(command)) {
        page->setBusy(false, "Request queue busy");
    }
}

void StoragePage::chatHistoryEventCallback(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }
    auto *page = static_cast<StoragePage *>(lv_event_get_user_data(event));
    if (page == nullptr || !page->page_active || page->chat_history_switch == nullptr) {
        return;
    }
    const bool enabled = lv_obj_has_state(page->chat_history_switch, LV_STATE_CHECKED);
    page->setBusy(true, enabled ? "Enabling history..." : "Disabling history...");
    if (!page->enqueue(WorkerCommand::SetChatHistory, enabled)) {
        if (enabled) {
            lv_obj_remove_state(page->chat_history_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(page->chat_history_switch, LV_STATE_CHECKED);
        }
        page->setBusy(false, "Request queue busy");
    }
}

void StoragePage::workerTask(void *argument)
{
    auto *page = static_cast<StoragePage *>(argument);
    WorkerRequest request = {};
    while (xQueueReceive(page->worker_queue, &request, portMAX_DELAY) == pdTRUE) {
        auto *result = static_cast<WorkerResult *>(calloc(1, sizeof(WorkerResult)));
        if (result == nullptr) {
            ESP_UTILS_LOGE("Allocate Storage worker result failed");
            continue;
        }
        result->page = page;
        result->command = request.command;
        result->generation = request.generation;
        result->operation_result = ESP_OK;

        switch (request.command) {
        case WorkerCommand::Refresh:
            result->operation_result = storage_service_mount();
            break;
        case WorkerCommand::Benchmark:
            result->operation_result =
                storage_service_run_benchmark(BENCHMARK_BYTES, &result->benchmark);
            break;
        case WorkerCommand::Eject:
            result->operation_result = storage_service_safe_eject();
            break;
        case WorkerCommand::ExportDiagnostics:
            result->operation_result =
                exportDiagnostics(result->diagnostics_path, sizeof(result->diagnostics_path));
            break;
        case WorkerCommand::SetChatHistory:
            result->operation_result = chat_history_set_enabled(request.enabled);
            break;
        }

        result->chat_history_enabled = chat_history_is_enabled();
        result->info_result = storage_service_get_info(&result->info);
        lv_result_t async_result = LV_RESULT_INVALID;
        if (bsp_display_lock(0)) {
            async_result = lv_async_call(workerResultAsyncCallback, result);
            bsp_display_unlock();
        }
        if (async_result != LV_RESULT_OK) {
            free(result);
        }
    }
    page->worker_task = nullptr;
    vTaskDelete(nullptr);
}

void StoragePage::workerResultAsyncCallback(void *argument)
{
    auto *result = static_cast<WorkerResult *>(argument);
    if (result == nullptr) {
        return;
    }
    StoragePage *page = result->page;
    if (page != nullptr && page->page_active &&
            page->page_generation.load() == result->generation && page->page_root != nullptr) {
        page->applyWorkerResult(*result);
    }
    free(result);
}

void StoragePage::applyInfo(const storage_service_info_t &info, esp_err_t info_result)
{
    if (status_value == nullptr) {
        return;
    }

    char text[64] = {};
    if (info.last_error != ESP_OK && info.state == STORAGE_SERVICE_STATE_ERROR) {
        snprintf(text, sizeof(text), "%s: %s", storage_service_state_name(info.state),
                 esp_err_to_name(info.last_error));
    } else {
        strlcpy(text, storage_service_state_name(info.state), sizeof(text));
    }
    lv_label_set_text(status_value, text);

    const bool has_card = info.capacity_bytes != 0;
    lv_label_set_text(card_value, has_card && info.card_name[0] ? info.card_name : "--");
    if (has_card) {
        formatBytes(info.capacity_bytes, text, sizeof(text));
        lv_label_set_text(capacity_value, text);
        formatBytes(info.free_bytes, text, sizeof(text));
        lv_label_set_text(free_value, text);
        lv_label_set_text_fmt(bus_value, "SDMMC %lu-bit",
                              static_cast<unsigned long>(info.bus_width));
        lv_label_set_text_fmt(frequency_value, "%.1f MHz",
                              static_cast<double>(info.frequency_khz) / 1000.0);
    } else {
        lv_label_set_text(capacity_value, "--");
        lv_label_set_text(free_value, "--");
        lv_label_set_text(bus_value, "SDMMC 1-bit");
        lv_label_set_text(frequency_value, "--");
    }

    if (info_result != ESP_OK && info.state != STORAGE_SERVICE_STATE_ERROR) {
        ESP_UTILS_LOGW("Read SD information failed: %s", esp_err_to_name(info_result));
    }
}

void StoragePage::applyWorkerResult(const WorkerResult &result)
{
    applyInfo(result.info, result.info_result);

    if (chat_history_switch != nullptr) {
        if (result.chat_history_enabled) {
            lv_obj_add_state(chat_history_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(chat_history_switch, LV_STATE_CHECKED);
        }
    }

    char message[176] = {};
    if (result.operation_result != ESP_OK) {
        if ((result.command == WorkerCommand::Eject ||
             result.command == WorkerCommand::Refresh) &&
                result.operation_result == ESP_ERR_INVALID_STATE && result.info.active_leases > 0) {
            snprintf(message, sizeof(message), "Busy: close media apps in Recents (%lu user(s))",
                     static_cast<unsigned long>(result.info.active_leases));
        } else {
            snprintf(message, sizeof(message), "Failed: %s",
                     esp_err_to_name(result.operation_result));
        }
    } else {
        switch (result.command) {
        case WorkerCommand::Refresh:
            strlcpy(message, "Card scan complete", sizeof(message));
            break;
        case WorkerCommand::Benchmark:
            lv_label_set_text_fmt(write_value, "%.2f MiB/s", result.benchmark.write_mib_per_s);
            lv_label_set_text_fmt(read_value, "%.2f MiB/s", result.benchmark.read_mib_per_s);
            lv_label_set_text(crc_value,
                              result.benchmark.actual_crc32 == result.benchmark.expected_crc32
                                  ? "Passed"
                                  : "Mismatch");
            snprintf(message, sizeof(message), "4 MiB test passed (%08lx)",
                     static_cast<unsigned long>(result.benchmark.actual_crc32));
            break;
        case WorkerCommand::Eject:
            strlcpy(message, "Safe to remove card", sizeof(message));
            break;
        case WorkerCommand::ExportDiagnostics:
            snprintf(message, sizeof(message), "Saved: %s", result.diagnostics_path);
            break;
        case WorkerCommand::SetChatHistory:
            strlcpy(message,
                    result.chat_history_enabled ? "AIChats history enabled"
                                                : "AIChats history disabled",
                    sizeof(message));
            break;
        }
    }
    setBusy(false, message);
}

} // namespace esp_brookesia::apps
