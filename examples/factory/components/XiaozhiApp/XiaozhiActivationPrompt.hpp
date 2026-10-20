/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <atomic>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace esp_brookesia::apps {

class XiaozhiActivationPrompt {
public:
    XiaozhiActivationPrompt();
    ~XiaozhiActivationPrompt();

    XiaozhiActivationPrompt(const XiaozhiActivationPrompt &) = delete;
    XiaozhiActivationPrompt &operator=(const XiaozhiActivationPrompt &) = delete;

    bool start(const char *activation_code, bool force);
    bool playSuccess();
    void cancel();
    bool waitUntilIdle(uint32_t timeout_ms) const;
    bool stopAndWait(uint32_t timeout_ms);
    bool isRunning() const;
    bool lastPlaybackCompleted() const;

private:
    static constexpr size_t CODE_CAPACITY = 64;
    static constexpr int TASK_STACK_SIZE = 24 * 1024;

    SemaphoreHandle_t _mutex = nullptr;
    TaskHandle_t _task = nullptr;
    std::atomic<bool> _running{false};
    std::atomic<bool> _cancel_requested{false};
    std::atomic<bool> _last_playback_completed{false};
    std::atomic<bool> _audio_session_acquired{false};
    std::atomic<bool> _codec_claimed{false};
    char _pending_code[CODE_CAPACITY] = {};
    char _announced_code[CODE_CAPACITY] = {};
    bool _pending_success = false;

    bool shouldContinue() const;
    bool releaseAudioSession();
    void run();

    static void taskEntry(void *arg);
};

} // namespace esp_brookesia::apps
