/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

namespace esp_brookesia::apps {

enum class XiaozhiUiText : uint8_t {
    NetworkRequired,
    Preparing,
    ActivationRequired,
    Connecting,
    Ready,
    Listening,
    Processing,
    Speaking,
    Error,
    ActivationInstructions,
    Count,
};

/**
 * Return compile-time localized text for Xiaozhi-owned UI elements.
 *
 * Server-provided conversation and activation text must not pass through this
 * function because its language is selected by the Xiaozhi service.
 */
const char *xiaozhiUiText(XiaozhiUiText text);

} // namespace esp_brookesia::apps
