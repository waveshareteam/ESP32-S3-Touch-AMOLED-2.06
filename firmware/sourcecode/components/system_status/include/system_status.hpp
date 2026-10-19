/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>

#include "esp_err.h"
#include "esp_wifi_types_generic.h"

namespace esp_brookesia::systems::phone {
class StatusBar;
}

namespace brookesia::system_status {

enum class BatteryPowerState : uint8_t {
    UNKNOWN = 0,
    STANDBY,
    CHARGING,
    DISCHARGING,
};

struct Snapshot {
    bool battery_valid = false;
    bool battery_present = false;
    int battery_percent = -1;
    bool charging = false;
    BatteryPowerState battery_power_state = BatteryPowerState::UNKNOWN;
    uint8_t charger_status = 0;
    bool wifi_enabled = false;
    bool wifi_connected = false;
    int wifi_rssi = -127;
};

/**
 * Start real board-status monitoring.
 *
 * The caller retains ownership of status_bar, which must remain alive until
 * stop() is called. The AXP2101 is read through the BSP I2C bus and Wi-Fi
 * state is sourced from native ESP-IDF Wi-Fi/IP events plus AP RSSI polling.
 */
esp_err_t start(
    esp_brookesia::systems::phone::StatusBar *status_bar,
    uint32_t refresh_period_ms = 1000
);

esp_err_t stop();

/** Return the most recent hardware snapshot. */
bool get_snapshot(Snapshot &snapshot);

/**
 * Apply and persist the system-wide Wi-Fi switch state.
 *
 * The resident monitor owns STA start/stop and reconnection. UI applications
 * should use this entry point rather than starting or stopping the driver
 * directly.
 */
esp_err_t set_wifi_enabled(bool enabled);

/** Connect using a new STA configuration under the resident reconnect policy. */
esp_err_t connect_wifi(const wifi_config_t &config);

/** Request a connection attempt using the currently stored STA configuration. */
esp_err_t request_wifi_connect();

} // namespace brookesia::system_status
