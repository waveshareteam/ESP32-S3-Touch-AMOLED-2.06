/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STORAGE_SERVICE_MOUNT_POINT "/sdcard"

typedef enum {
    STORAGE_SERVICE_STATE_UNINITIALIZED = 0,
    STORAGE_SERVICE_STATE_UNMOUNTED,
    STORAGE_SERVICE_STATE_MOUNTING,
    STORAGE_SERVICE_STATE_MOUNTED,
    STORAGE_SERVICE_STATE_EJECTING,
    STORAGE_SERVICE_STATE_ERROR,
} storage_service_state_t;

typedef struct {
    storage_service_state_t state;
    esp_err_t last_error;
    uint32_t generation;
    uint32_t active_leases;
    char card_name[20];
    uint64_t capacity_bytes;
    uint64_t free_bytes;
    uint32_t bus_width;
    uint32_t frequency_khz;
} storage_service_info_t;

/*
 * A lease pins the mounted volume while an application owns open files or
 * iterators.  Callers should keep one lease for the entire lifetime of those
 * objects and release it only after every FILE handle has been closed.
 */
typedef struct {
    uint32_t generation;
    bool active;
} storage_service_lease_t;

typedef struct {
    size_t requested_bytes;
    size_t written_bytes;
    size_t read_bytes;
    uint32_t expected_crc32;
    uint32_t actual_crc32;
    uint32_t write_time_ms;
    uint32_t read_time_ms;
    float write_mib_per_s;
    float read_mib_per_s;
    esp_err_t result;
} storage_service_benchmark_t;

/** Initialize the singleton service without requiring a card to be present. */
esp_err_t storage_service_init(void);

/** Mount or re-check the volume. Safe to call repeatedly. */
esp_err_t storage_service_mount(void);

/** Acquire an inactive, zero-initialized lease, mounting the card on demand. */
esp_err_t storage_service_acquire(storage_service_lease_t *lease);

/** Release a previously acquired lease. Safe to call on an inactive lease. */
void storage_service_release(storage_service_lease_t *lease);

/** Unmount the card only when no application holds a lease. */
esp_err_t storage_service_safe_eject(void);

/** Copy a coherent snapshot of the current card/service state. */
esp_err_t storage_service_get_info(storage_service_info_t *info);

/** Run a write/read/CRC benchmark using a newly-created private temporary file. */
esp_err_t storage_service_run_benchmark(size_t bytes, storage_service_benchmark_t *result);

const char *storage_service_state_name(storage_service_state_t state);

#ifdef __cplusplus
}
#endif
