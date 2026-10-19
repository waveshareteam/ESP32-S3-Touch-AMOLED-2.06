/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

struct esp_mcp_s;
typedef struct esp_mcp_s esp_mcp_t;

namespace esp_brookesia::apps {

/**
 * Register product-specific MCP tools exposed by the Xiaozhi application.
 *
 * This hook runs once for each newly created MCP engine, before the Xiaozhi
 * transport starts. The default implementation intentionally registers no
 * tools. Add only complete, callable tools so tools/list never advertises
 * placeholders to the server.
 */
esp_err_t registerXiaozhiDeveloperTools(esp_mcp_t *mcp);

} // namespace esp_brookesia::apps
