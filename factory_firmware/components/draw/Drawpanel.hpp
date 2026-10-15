/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <list>

#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

/**
 * @brief Drawpanel application for touch-based drawing on the device screen
 *
 */
class Drawpanel: public systems::phone::App {
public:
    /**
     * @brief Get the singleton instance of Drawpanel
     *
     * @param use_status_bar Show status bar
     * @param use_navigation_bar Show navigation bar
     * @return Drawpanel* Singleton instance pointer
     */
    static Drawpanel *requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);

    /**
     * @brief Destroy the Drawpanel object
     *
     */
    ~Drawpanel();

    using systems::phone::App::startRecordResource;
    using systems::phone::App::endRecordResource;

protected:
    /**
     * @brief Construct a new Drawpanel object (private to enforce singleton)
     *
     * @param use_status_bar Show status bar
     * @param use_navigation_bar Show navigation bar
     */
    Drawpanel(bool use_status_bar, bool use_navigation_bar);

    bool run(void) override;
    bool back(void) override;

    bool close(void) override;
    bool init(void) override;
    bool deinit(void) override;
    bool pause(void) override;
    bool resume(void) override;
    // bool cleanResource(void) override;

private:
    static Drawpanel *_instance;

    lv_obj_t *_panel_obj;
    struct StrokeSegment {
        lv_obj_t *object = nullptr;
        // LVGL keeps this pointer, so the points must outlive the line object.
        lv_point_precise_t points[2] = {};
    };
    std::list<StrokeSegment> _segments;
    const int _max_segments;
    void clearDrawing();
    bool drawSegment(const lv_point_t &from, const lv_point_t &to);
    /**
     * @brief Touch event callback for drawing continuous strokes on screen
     * @param e LVGL event data
     */
    static void touch_event_cb(lv_event_t *e);

    lv_point_t _prev_point = {};
    bool _has_prev_point = false;
};

} // namespace esp_brookesia::apps
