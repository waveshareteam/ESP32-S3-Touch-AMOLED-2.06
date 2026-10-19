/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <atomic>
#include <cstdint>

#include "esp_brookesia.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "storage_service.h"

namespace esp_brookesia::apps {

class StoragePage : public systems::phone::App {
public:
    static StoragePage *requestInstance(bool use_status_bar = false,
                                        bool use_navigation_bar = false);

    StoragePage(bool use_status_bar, bool use_navigation_bar);
    ~StoragePage() override;

    bool run() override;
    bool back() override;
    bool close() override;

private:
    enum class WorkerCommand : uint8_t {
        Refresh,
        Benchmark,
        Eject,
        ExportDiagnostics,
        SetChatHistory,
    };

    struct WorkerRequest {
        WorkerCommand command;
        uint32_t generation;
        bool enabled;
    };

    struct WorkerResult {
        StoragePage *page;
        WorkerCommand command;
        uint32_t generation;
        esp_err_t operation_result;
        esp_err_t info_result;
        storage_service_info_t info;
        storage_service_benchmark_t benchmark;
        bool chat_history_enabled;
        char diagnostics_path[160];
    };

    static StoragePage *_instance;

    static void actionEventCallback(lv_event_t *event);
    static void chatHistoryEventCallback(lv_event_t *event);
    static void workerTask(void *argument);
    static void workerResultAsyncCallback(void *argument);

    bool ensureWorker();
    bool enqueue(WorkerCommand command, bool enabled = false);
    void createUi();
    void setBusy(bool busy, const char *message);
    void applyInfo(const storage_service_info_t &info, esp_err_t info_result);
    void applyWorkerResult(const WorkerResult &result);
    void destroyUi();

    std::atomic_bool page_active{false};
    std::atomic<uint32_t> page_generation{0};
    QueueHandle_t worker_queue = nullptr;
    TaskHandle_t worker_task = nullptr;

    lv_obj_t *page_root = nullptr;
    lv_obj_t *list = nullptr;
    lv_obj_t *status_value = nullptr;
    lv_obj_t *card_value = nullptr;
    lv_obj_t *capacity_value = nullptr;
    lv_obj_t *free_value = nullptr;
    lv_obj_t *bus_value = nullptr;
    lv_obj_t *frequency_value = nullptr;
    lv_obj_t *operation_value = nullptr;
    lv_obj_t *write_value = nullptr;
    lv_obj_t *read_value = nullptr;
    lv_obj_t *crc_value = nullptr;
    lv_obj_t *refresh_button = nullptr;
    lv_obj_t *benchmark_button = nullptr;
    lv_obj_t *export_button = nullptr;
    lv_obj_t *eject_button = nullptr;
    lv_obj_t *chat_history_switch = nullptr;

    lv_style_t style_list = {};
    lv_style_t style_row = {};
    lv_style_t style_section = {};
    lv_style_t style_pressed = {};
};

} // namespace esp_brookesia::apps
