/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <atomic>
#include <new>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "storage_service.h"

namespace brookesia::storage_ui {

// All UI methods run under the caller's LVGL lock. The mount worker owns only
// a reference-counted probe, so leaving the app never waits for SD timeouts.
class SdCardGate {
public:
    using Callback = void (*)(void *);

    ~SdCardGate() { close(); }
    SdCardGate() = default;
    SdCardGate(const SdCardGate &) = delete;
    SdCardGate &operator=(const SdCardGate &) = delete;

    bool ready() const { return _ready; }

    bool open(lv_obj_t *screen, Callback on_ready, Callback on_back, void *context)
    {
        if (_panel || _probe) {
            return true;
        }
        _screen = screen;
        _on_ready = on_ready;
        _on_back = on_back;
        _context = context;
        // A valid card goes straight to the app; only failures create a prompt.
        _probe = new (std::nothrow) Probe();
        if (!_probe) {
            showFailure(ESP_ERR_NO_MEM);
            return true;
        }
        _timer = lv_timer_create(pollTimer, 50, this);
        if (!_timer) {
            showFailure(ESP_ERR_NO_MEM);
            release(_probe);
            _probe = nullptr;
            return true;
        }
        // Publish the worker's reference before creation: it may finish on the
        // other core before xTaskCreate() returns.
        _probe->references.fetch_add(1);
        if (xTaskCreate(probeTask, "sd_app_check", 4096, _probe, 4, nullptr) != pdPASS) {
            release(_probe);
            _probe->result = ESP_ERR_NO_MEM;
            _probe->done.store(true);
        }
        return true;
    }

    void pause()
    {
        if (_timer) lv_timer_pause(_timer);
    }

    void resume()
    {
        if (_timer) lv_timer_resume(_timer);
    }

    void close()
    {
        lv_async_call_cancel(backAsync, this);
        removeUi();
        if (_probe) {
            release(_probe);
            _probe = nullptr;
        }
        _ready = false;
        _screen = nullptr;
        _on_ready = nullptr;
        _on_back = nullptr;
        _context = nullptr;
    }

private:
    struct Probe {
        std::atomic<unsigned> references{1};
        std::atomic<bool> done{false};
        esp_err_t result = ESP_OK;
        storage_service_lease_t lease = {};
    };

    static void release(Probe *probe)
    {
        if (probe->references.fetch_sub(1) == 1) {
            storage_service_release(&probe->lease);
            delete probe;
        }
    }

    static void probeTask(void *arg)
    {
        auto *probe = static_cast<Probe *>(arg);
        probe->result = storage_service_acquire(&probe->lease);
        probe->done.store(true);
        release(probe);
        vTaskDelete(nullptr);
    }

    static void pollTimer(lv_timer_t *timer)
    {
        auto *self = static_cast<SdCardGate *>(lv_timer_get_user_data(timer));
        if (!self->_probe || !self->_probe->done.load()) return;
        const esp_err_t result = self->_probe->result;
        lv_timer_delete(self->_timer);
        self->_timer = nullptr;
        if (result != ESP_OK) {
            self->showFailure(result);
            release(self->_probe);
            self->_probe = nullptr;
            return;
        }
        self->_ready = true;
        self->removeUi();
        // Keep the successful lease until app close. Each app also maintains
        // its own file/recording lease for its existing worker lifecycle.
        if (self->_on_ready) self->_on_ready(self->_context);
    }

    static void backEvent(lv_event_t *event)
    {
        auto *self = static_cast<SdCardGate *>(lv_event_get_user_data(event));
        // Defer navigation until the clicked button's event has returned.
        lv_async_call(backAsync, self);
    }

    static void backAsync(void *arg)
    {
        auto *self = static_cast<SdCardGate *>(arg);
        if (self->_panel && self->_on_back) self->_on_back(self->_context);
    }

    void showFailure(esp_err_t result)
    {
        ESP_LOGW("SdCardGate", "SD card check failed: %s", esp_err_to_name(result));
        if (!_panel) createPrompt();
        lv_label_set_text(_title, result == ESP_ERR_NO_MEM ? "Unable to open app" : "SD card required");
        lv_label_set_text(_message, result == ESP_ERR_NO_MEM
            ? "Not enough system memory.\nReturn and try again."
            : "Please insert an SD card\nto use this app.\nThen return and reopen it.");
    }

    void createPrompt()
    {
        _panel = lv_obj_create(_screen);
        lv_obj_remove_style_all(_panel);
        lv_obj_set_size(_panel, lv_pct(100), lv_pct(100));
        lv_obj_center(_panel);
        lv_obj_set_style_bg_color(_panel, lv_color_hex(0x101018), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_panel, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_remove_flag(_panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(_panel, LV_OBJ_FLAG_GESTURE_BUBBLE);

        lv_obj_t *icon = lv_label_create(_panel);
        lv_label_set_text(icon, LV_SYMBOL_SD_CARD);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_40, LV_PART_MAIN);
        lv_obj_set_style_text_color(icon, lv_color_hex(0x38BDF8), LV_PART_MAIN);
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, -110);

        _title = lv_label_create(_panel);
        lv_obj_set_style_text_font(_title, &lv_font_montserrat_24, LV_PART_MAIN);
        lv_obj_set_style_text_color(_title, lv_color_white(), LV_PART_MAIN);
        lv_obj_align(_title, LV_ALIGN_CENTER, 0, -44);

        _message = lv_label_create(_panel);
        lv_obj_set_width(_message, lv_pct(84));
        lv_obj_set_style_text_font(_message, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(_message, lv_color_hex(0xB7BAC5), LV_PART_MAIN);
        lv_obj_set_style_text_align(_message, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(_message, LV_ALIGN_CENTER, 0, 24);

        lv_obj_t *back = lv_button_create(_panel);
        lv_obj_set_size(back, 180, 56);
        lv_obj_align(back, LV_ALIGN_CENTER, 0, 124);
        lv_obj_set_style_bg_color(back, lv_color_hex(0x303040), LV_PART_MAIN);
        lv_obj_add_event_cb(back, backEvent, LV_EVENT_CLICKED, this);
        lv_obj_t *label = lv_label_create(back);
        lv_label_set_text(label, LV_SYMBOL_LEFT "  Back");
        lv_obj_set_style_text_font(label, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_center(label);
    }

    void removeUi()
    {
        if (_timer) {
            lv_timer_delete(_timer);
            _timer = nullptr;
        }
        if (_panel) {
            lv_obj_delete(_panel);
            _panel = nullptr;
        }
        _title = nullptr;
        _message = nullptr;
    }

    Probe *_probe = nullptr;
    lv_obj_t *_screen = nullptr;
    lv_obj_t *_panel = nullptr;
    lv_obj_t *_title = nullptr;
    lv_obj_t *_message = nullptr;
    lv_timer_t *_timer = nullptr;
    Callback _on_ready = nullptr;
    Callback _on_back = nullptr;
    void *_context = nullptr;
    bool _ready = false;
};

} // namespace brookesia::storage_ui
