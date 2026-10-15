/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "XiaozhiLocalization.hpp"

#include <stddef.h>

#include "sdkconfig.h"

namespace esp_brookesia::apps {

namespace {

#if defined(CONFIG_XIAOZHI_APP_UI_LANGUAGE_SIMPLIFIED_CHINESE)
constexpr const char *UI_TEXTS[] = {
    "需要网络连接",
    "正在准备小智",
    "等待设备激活",
    "正在连接",
    "待命",
    "聆听中",
    "思考中",
    "小智正在说话",
    "服务异常",
    "请在小智控制面板添加设备并输入激活码。",
};
#else
constexpr const char *UI_TEXTS[] = {
    "Network connection required",
    "Preparing Xiaozhi",
    "Waiting for device activation",
    "Connecting",
    "Standby",
    "Listening",
    "Thinking",
    "Xiaozhi is speaking",
    "Service unavailable",
    "Add this device in the Xiaozhi control panel and enter the activation code.",
};
#endif

constexpr size_t UI_TEXT_COUNT =
    static_cast<size_t>(XiaozhiUiText::Count);
static_assert(
    sizeof(UI_TEXTS) / sizeof(UI_TEXTS[0]) == UI_TEXT_COUNT,
    "Xiaozhi UI text catalog is incomplete"
);

} // namespace

const char *xiaozhiUiText(XiaozhiUiText text)
{
    size_t index = static_cast<size_t>(text);
    return index < UI_TEXT_COUNT ? UI_TEXTS[index] : "";
}

} // namespace esp_brookesia::apps
