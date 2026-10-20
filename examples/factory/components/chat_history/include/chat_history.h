/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the low-priority, non-blocking SD history writer. */
esp_err_t chat_history_init(void);

/** Enable or disable SD text history and persist the preference in NVS. */
esp_err_t chat_history_set_enabled(bool enabled);

bool chat_history_is_enabled(void);

/**
 * Queue one chat message without blocking the protocol/control task.
 * Messages are best-effort and are discarded when disabled or unavailable.
 */
esp_err_t chat_history_append(const char *role, const char *text);

#ifdef __cplusplus
}
#endif
