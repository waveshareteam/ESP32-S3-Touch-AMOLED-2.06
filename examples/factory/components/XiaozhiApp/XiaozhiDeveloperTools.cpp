/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "XiaozhiDeveloperTools.hpp"

#include "esp_mcp_engine.h"

namespace esp_brookesia::apps {

esp_err_t registerXiaozhiDeveloperTools(esp_mcp_t *mcp)
{
    if (!mcp) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * TODO: Register finished product tools here.
     *
     * Suggested extension points:
     * - device.display.set_brightness: validate a bounded brightness value,
     *   then call the BSP backlight API.
     * - device.audio.set_volume: validate a bounded volume value, then update
     *   the shared codec/output-volume service.
     * - device.camera.capture: capture a current frame and return it through
     *   the MCP image/vision result path without blocking the protocol task.
     *
     * Use esp_mcp_tool_create() plus esp_mcp_tool_add_property() to describe a
     * real callback, then publish it with esp_mcp_add_tool(). If publication
     * fails, destroy the tool immediately. After successful publication, the
     * MCP engine owns the tool and releases it from esp_mcp_destroy().
     *
     * Keep hardware work out of the MCP callback when it can block. Dispatch
     * to an application task and return a truthful result only after the
     * operation completes. Camera buffers in particular must be copied or
     * retained until the result builder has consumed them.
     */

    return ESP_OK;
}

} // namespace esp_brookesia::apps
