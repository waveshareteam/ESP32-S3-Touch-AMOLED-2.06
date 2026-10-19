/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "system_status.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_brookesia.hpp"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

namespace brookesia::system_status {

namespace {

constexpr char TAG[] = "system_status";

constexpr uint16_t AXP2101_I2C_ADDRESS = 0x34;
constexpr uint32_t AXP2101_I2C_SPEED_HZ = 400000;
constexpr int I2C_TIMEOUT_MS = 100;
constexpr uint32_t MONITOR_STOP_TIMEOUT_MS = 4000;
constexpr unsigned BATTERY_TRANSIENT_ERROR_LIMIT = 3;
constexpr unsigned PMU_RETRY_INTERVAL_CYCLES = 5;

constexpr char WIFI_NVS_NAMESPACE[] = "storage";
constexpr char WIFI_NVS_ENABLED_KEY[] = "wifi_en";

using StatusBar = esp_brookesia::systems::phone::StatusBar;

// XPowersLib supports the ESP-IDF master driver through callbacks. Keep the
// PMU on the BSP-owned bus so the touch controller, IMU, RTC, and PMU share
// the same I2C controller without creating a competing bus instance.
i2c_master_dev_handle_t pmu_device = nullptr;
std::atomic<esp_err_t> pmu_i2c_error = ESP_OK;

int pmuRegisterRead(uint8_t, uint8_t reg, uint8_t *data, uint8_t len)
{
    if (pmu_device == nullptr || data == nullptr || len == 0) {
        pmu_i2c_error.store(ESP_ERR_INVALID_STATE);
        return -1;
    }

    const esp_err_t result = i2c_master_transmit_receive(
        pmu_device, &reg, sizeof(reg), data, len, I2C_TIMEOUT_MS
    );
    if (result != ESP_OK) {
        pmu_i2c_error.store(result);
        return -1;
    }
    return 0;
}

int pmuRegisterWrite(uint8_t, uint8_t reg, uint8_t *data, uint8_t len)
{
    if (pmu_device == nullptr || data == nullptr || len == 0) {
        pmu_i2c_error.store(ESP_ERR_INVALID_STATE);
        return -1;
    }

    std::array<uint8_t, UINT8_MAX + 1> transaction = {};
    transaction[0] = reg;
    std::copy_n(data, len, transaction.begin() + 1);
    const esp_err_t result = i2c_master_transmit(
        pmu_device, transaction.data(), static_cast<size_t>(len) + 1, I2C_TIMEOUT_MS
    );
    if (result != ESP_OK) {
        pmu_i2c_error.store(result);
        return -1;
    }
    return 0;
}

class SemaphoreLock {
public:
    explicit SemaphoreLock(SemaphoreHandle_t semaphore)
        : _semaphore(semaphore),
          _locked(semaphore != nullptr && xSemaphoreTake(semaphore, portMAX_DELAY) == pdTRUE)
    {
    }

    ~SemaphoreLock()
    {
        if (_locked) {
            xSemaphoreGive(_semaphore);
        }
    }

    bool isLocked() const
    {
        return _locked;
    }

    SemaphoreLock(const SemaphoreLock &) = delete;
    SemaphoreLock &operator=(const SemaphoreLock &) = delete;

private:
    SemaphoreHandle_t _semaphore;
    bool _locked;
};

class Monitor {
public:
    Monitor()
        : _api_mutex(xSemaphoreCreateMutex()),
          _wifi_mutex(xSemaphoreCreateMutex())
    {
    }

    esp_err_t start(StatusBar *status_bar, uint32_t refresh_period_ms)
    {
        if (status_bar == nullptr || refresh_period_ms < 100) {
            return ESP_ERR_INVALID_ARG;
        }

        if (_api_mutex == nullptr || _wifi_mutex == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        SemaphoreLock api_lock(_api_mutex);
        if (!api_lock.isLocked()) {
            return ESP_FAIL;
        }

        if (_running.load() || _task.load() != nullptr) {
            return ESP_ERR_INVALID_STATE;
        }

        esp_err_t result = ensureWifiStackInitialized();
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Initialize Wi-Fi stack failed: %s", esp_err_to_name(result));
            return result;
        }

        bool wifi_enabled = false;
        result = loadWifiEnabled(wifi_enabled);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Read persisted Wi-Fi state failed: %s", esp_err_to_name(result));
            wifi_enabled = false;
        }

        const esp_err_t pmu_result = tryInitializePmu();
        if (pmu_result != ESP_OK) {
            _last_pmu_init_error = pmu_result;
            _pmu_retry_countdown = PMU_RETRY_INTERVAL_CYCLES;
            ESP_LOGW(
                TAG,
                "AXP2101 unavailable; battery will remain unknown while Wi-Fi monitoring continues: %s",
                esp_err_to_name(pmu_result)
            );
        }

        result = esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, eventHandler, this, &_wifi_event_instance
        );
        if (result != ESP_OK) {
            cleanupDevice();
            return result;
        }

        result = esp_event_handler_instance_register(
            IP_EVENT, ESP_EVENT_ANY_ID, eventHandler, this, &_ip_event_instance
        );
        if (result != ESP_OK) {
            esp_event_handler_instance_unregister(
                WIFI_EVENT, ESP_EVENT_ANY_ID, _wifi_event_instance
            );
            _wifi_event_instance = nullptr;
            cleanupDevice();
            return result;
        }

        _status_bar = status_bar;
        _refresh_period_ms = refresh_period_ms;
        _wifi_enabled.store(wifi_enabled);
        _wifi_started.store(false);
        _wifi_event_connected.store(false);
        _wifi_reconnect_requested.store(false);
        _manual_wifi_configuration.store(false);
        _battery_error_streak = 0;
        _last_battery_error = ESP_OK;

        portENTER_CRITICAL(&_snapshot_lock);
        _snapshot = {};
        _snapshot.wifi_enabled = wifi_enabled;
        portEXIT_CRITICAL(&_snapshot_lock);

        _running.store(true);

        result = applyWifiEnabledState(wifi_enabled);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Restore Wi-Fi state failed: %s", esp_err_to_name(result));
        }

        TaskHandle_t task = nullptr;
        if (xTaskCreate(taskEntry, "system_status", 4096, this, 3, &task) != pdPASS) {
            _running.store(false);
            if (wifi_enabled) {
                const esp_err_t rollback_result = applyWifiEnabledState(false);
                if (rollback_result != ESP_OK) {
                    ESP_LOGW(
                        TAG,
                        "Roll back Wi-Fi after task creation failure failed: %s",
                        esp_err_to_name(rollback_result)
                    );
                }
            }
            unregisterEventHandlers();
            cleanupDevice();
            _status_bar = nullptr;
            return ESP_ERR_NO_MEM;
        }
        _task.store(task);

        ESP_LOGI(
            TAG,
            "Monitoring AXP2101 battery and ESP-IDF Wi-Fi state (Wi-Fi %s)",
            wifi_enabled ? "enabled" : "disabled"
        );
        return ESP_OK;
    }

    esp_err_t stop()
    {
        if (_api_mutex == nullptr) {
            return ESP_ERR_INVALID_STATE;
        }
        SemaphoreLock api_lock(_api_mutex);
        if (!api_lock.isLocked()) {
            return ESP_FAIL;
        }
        if (!_running.exchange(false)) {
            return ESP_ERR_INVALID_STATE;
        }

        // No new event callback can queue work after this point. The monitor
        // task is deliberately allowed to finish any bounded I2C transaction;
        // deleting it while the shared BSP bus is locked can corrupt the bus.
        unregisterEventHandlers();

        const TickType_t poll_delay = pdMS_TO_TICKS(10);
        const unsigned poll_count = MONITOR_STOP_TIMEOUT_MS / 10;
        for (unsigned i = 0; i < poll_count && _task.load() != nullptr; ++i) {
            vTaskDelay(poll_delay);
        }
        if (_task.load() != nullptr) {
            ESP_LOGE(TAG, "Monitor task did not stop safely within %" PRIu32 " ms", MONITOR_STOP_TIMEOUT_MS);
            return ESP_ERR_TIMEOUT;
        }

        cleanupDevice();
        _status_bar = nullptr;
        _wifi_event_connected.store(false);
        _wifi_reconnect_requested.store(false);
        return ESP_OK;
    }

    bool getSnapshot(Snapshot &snapshot)
    {
        portENTER_CRITICAL(&_snapshot_lock);
        snapshot = _snapshot;
        portEXIT_CRITICAL(&_snapshot_lock);
        return _running.load();
    }

    esp_err_t setWifiEnabled(bool enabled)
    {
        SemaphoreLock api_lock(_api_mutex);
        if (!api_lock.isLocked()) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!_running.load()) {
            return ESP_ERR_INVALID_STATE;
        }
        SemaphoreLock wifi_lock(_wifi_mutex);
        if (!wifi_lock.isLocked()) {
            return ESP_FAIL;
        }

        esp_err_t result = persistWifiEnabled(enabled);
        if (result != ESP_OK) {
            return result;
        }

        const bool was_enabled = _wifi_enabled.exchange(enabled);
        if (!enabled || !was_enabled) {
            _wifi_event_connected.store(false);
        }
        if (!enabled) {
            _wifi_reconnect_requested.store(false);
        }

        portENTER_CRITICAL(&_snapshot_lock);
        _snapshot.wifi_enabled = enabled;
        if (!enabled) {
            _snapshot.wifi_connected = false;
            _snapshot.wifi_rssi = -127;
        }
        portEXIT_CRITICAL(&_snapshot_lock);

        if (was_enabled == enabled && _wifi_started.load() == enabled) {
            return ESP_OK;
        }

        result = applyWifiEnabledState(enabled);
        return result;
    }

    esp_err_t connectWifi(const wifi_config_t &config)
    {
        SemaphoreLock api_lock(_api_mutex);
        if (!api_lock.isLocked()) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!_running.load() || !_wifi_enabled.load()) {
            return ESP_ERR_INVALID_STATE;
        }
        SemaphoreLock wifi_lock(_wifi_mutex);
        if (!wifi_lock.isLocked()) {
            return ESP_FAIL;
        }

        _manual_wifi_configuration.store(true);
        _wifi_reconnect_requested.store(false);

        const esp_err_t disconnect_result = esp_wifi_disconnect();
        if (disconnect_result != ESP_OK &&
                disconnect_result != ESP_ERR_WIFI_NOT_STARTED &&
                disconnect_result != ESP_ERR_WIFI_NOT_CONNECT) {
            ESP_LOGW(
                TAG, "Disconnect before applying STA configuration failed: %s",
                esp_err_to_name(disconnect_result)
            );
        }

        wifi_config_t mutable_config = config;
        const esp_err_t result = esp_wifi_set_config(WIFI_IF_STA, &mutable_config);
        _manual_wifi_configuration.store(false);
        if (result != ESP_OK) {
            return result;
        }

        _wifi_reconnect_requested.store(true);
        return ESP_OK;
    }

    esp_err_t requestWifiConnect()
    {
        SemaphoreLock api_lock(_api_mutex);
        if (!api_lock.isLocked()) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!_running.load() || !_wifi_enabled.load()) {
            return ESP_ERR_INVALID_STATE;
        }
        SemaphoreLock wifi_lock(_wifi_mutex);
        if (!wifi_lock.isLocked()) {
            return ESP_FAIL;
        }
        _wifi_reconnect_requested.store(true);
        return ESP_OK;
    }

private:
    static void taskEntry(void *arg)
    {
        static_cast<Monitor *>(arg)->taskLoop();
    }

    static void eventHandler(
        void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data
    )
    {
        (void)event_data;
        auto *monitor = static_cast<Monitor *>(arg);
        if (monitor == nullptr || !monitor->_running.load()) {
            return;
        }

        if (event_base == WIFI_EVENT) {
            switch (event_id) {
            case WIFI_EVENT_STA_START:
                monitor->_wifi_started.store(true);
                if (monitor->_wifi_enabled.load()) {
                    monitor->_wifi_reconnect_requested.store(true);
                }
                break;
            case WIFI_EVENT_STA_CONNECTED:
                monitor->_wifi_event_connected.store(true);
                monitor->_wifi_reconnect_requested.store(false);
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                monitor->_wifi_event_connected.store(false);
                if (monitor->_wifi_enabled.load() &&
                        !monitor->_manual_wifi_configuration.load()) {
                    monitor->_wifi_reconnect_requested.store(true);
                }
                break;
            case WIFI_EVENT_STA_STOP:
                monitor->_wifi_started.store(false);
                monitor->_wifi_event_connected.store(false);
                monitor->_wifi_reconnect_requested.store(false);
                break;
            default:
                break;
            }
        } else if (event_base == IP_EVENT) {
            if (event_id == IP_EVENT_STA_GOT_IP) {
                monitor->_wifi_event_connected.store(true);
                monitor->_wifi_reconnect_requested.store(false);
            } else if (event_id == IP_EVENT_STA_LOST_IP) {
                monitor->_wifi_event_connected.store(false);
            }
        }

    }

    void taskLoop()
    {
        while (_running.load()) {
            Snapshot next = {};
            next.battery_percent = -1;
            next.wifi_enabled = _wifi_enabled.load();
            next.wifi_rssi = -127;

            if (pmu_device == nullptr) {
                if (_pmu_retry_countdown > 0) {
                    --_pmu_retry_countdown;
                } else {
                    const esp_err_t pmu_result = tryInitializePmu();
                    if (pmu_result != ESP_OK) {
                        _pmu_retry_countdown = PMU_RETRY_INTERVAL_CYCLES;
                        if (_last_pmu_init_error != pmu_result) {
                            ESP_LOGW(TAG, "AXP2101 retry failed: %s", esp_err_to_name(pmu_result));
                        }
                        _last_pmu_init_error = pmu_result;
                    } else {
                        _last_pmu_init_error = ESP_OK;
                        _battery_error_streak = 0;
                    }
                }
            }

            const esp_err_t battery_result = readBattery(next);
            if (battery_result != ESP_OK) {
                ++_battery_error_streak;
                if (_last_battery_error != battery_result) {
                    ESP_LOGW(TAG, "Read AXP2101 failed: %s", esp_err_to_name(battery_result));
                    _last_battery_error = battery_result;
                }

                // Preserve every validity field with the last real sample for a
                // short I2C disturbance. Persistent failure becomes UNKNOWN and
                // causes the status bar to hide the battery instead of lying.
                portENTER_CRITICAL(&_snapshot_lock);
                if (_snapshot.battery_valid &&
                        _battery_error_streak <= BATTERY_TRANSIENT_ERROR_LIMIT) {
                    next.battery_valid = _snapshot.battery_valid;
                    next.battery_present = _snapshot.battery_present;
                    next.battery_percent = _snapshot.battery_percent;
                    next.charging = _snapshot.charging;
                    next.battery_power_state = _snapshot.battery_power_state;
                    next.charger_status = _snapshot.charger_status;
                }
                portEXIT_CRITICAL(&_snapshot_lock);

                if (pmu_device != nullptr &&
                        _battery_error_streak >= BATTERY_TRANSIENT_ERROR_LIMIT) {
                    cleanupDevice();
                    _pmu_retry_countdown = PMU_RETRY_INTERVAL_CYCLES;
                }
            } else {
                _battery_error_streak = 0;
                _last_battery_error = ESP_OK;
            }

            if (!_running.load()) {
                break;
            }

            {
                SemaphoreLock wifi_lock(_wifi_mutex);
                if (!wifi_lock.isLocked() || !_running.load()) {
                    break;
                }

                // Re-read the desired state while holding the same lock used
                // by the UI APIs. This prevents a stale monitor iteration from
                // overwriting a freshly toggled switch or new STA config.
                next.wifi_enabled = _wifi_enabled.load();
                if (next.wifi_enabled && !_wifi_started.load()) {
                    const esp_err_t wifi_start_result = applyWifiEnabledState(true);
                    if (wifi_start_result != ESP_OK && _last_wifi_start_error != wifi_start_result) {
                        ESP_LOGW(
                            TAG,
                            "Restore Wi-Fi enabled state failed: %s",
                            esp_err_to_name(wifi_start_result)
                        );
                    }
                    _last_wifi_start_error = wifi_start_result;
                } else if (!next.wifi_enabled && _wifi_started.load()) {
                    const esp_err_t wifi_stop_result = applyWifiEnabledState(false);
                    if (wifi_stop_result != ESP_OK && _last_wifi_start_error != wifi_stop_result) {
                        ESP_LOGW(
                            TAG,
                            "Retry Wi-Fi disabled state failed: %s",
                            esp_err_to_name(wifi_stop_result)
                        );
                    }
                    _last_wifi_start_error = wifi_stop_result;
                } else {
                    _last_wifi_start_error = ESP_OK;
                }

                serviceWifiReconnect();

                wifi_ap_record_t access_point = {};
                if (next.wifi_enabled && esp_wifi_sta_get_ap_info(&access_point) == ESP_OK) {
                    next.wifi_connected = true;
                    next.wifi_rssi = access_point.rssi;
                    _wifi_event_connected.store(true);
                } else if (next.wifi_enabled) {
                    next.wifi_connected = _wifi_event_connected.load();
                }

                portENTER_CRITICAL(&_snapshot_lock);
                _snapshot = next;
                portEXIT_CRITICAL(&_snapshot_lock);
            }

            updateStatusBar(next);
            if (_running.load()) {
                vTaskDelay(pdMS_TO_TICKS(_refresh_period_ms));
            }
        }

        cleanupDevice();
        _status_bar = nullptr;
        _task.store(nullptr);
        vTaskDelete(nullptr);
    }

    esp_err_t ensureWifiStackInitialized() const
    {
        esp_err_t result = esp_netif_init();
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
            return result;
        }

        result = esp_event_loop_create_default();
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
            return result;
        }

        if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == nullptr &&
                esp_netif_create_default_wifi_sta() == nullptr) {
            return ESP_ERR_NO_MEM;
        }

        wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
        result = esp_wifi_init(&wifi_config);
        if (result != ESP_OK && result != ESP_ERR_WIFI_INIT_STATE) {
            return result;
        }
        return ESP_OK;
    }

    esp_err_t tryInitializePmu()
    {
        if (pmu_device != nullptr) {
            return ESP_OK;
        }

        const i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
        if (bus == nullptr) {
            return ESP_ERR_INVALID_STATE;
        }

        const i2c_device_config_t device_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = AXP2101_I2C_ADDRESS,
            .scl_speed_hz = AXP2101_I2C_SPEED_HZ,
            .scl_wait_us = 0,
            .flags = {},
        };
        esp_err_t result = i2c_master_bus_add_device(bus, &device_config, &pmu_device);
        if (result != ESP_OK) {
            pmu_device = nullptr;
            return result;
        }

        pmu_i2c_error.store(ESP_OK);
        if (!_pmu.begin(AXP2101_I2C_ADDRESS, pmuRegisterRead, pmuRegisterWrite)) {
            result = pmu_i2c_error.load();
            cleanupDevice();
            return result == ESP_OK ? ESP_ERR_NOT_FOUND : result;
        }

        // Keep the board's existing charging policy, but delegate the register
        // details to XPowersLib instead of duplicating AXP2101 fields locally.
        _pmu.disableTSPinMeasure();
        _pmu.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_50MA);
        const bool constant_current_set = _pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_400MA);
        _pmu.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_25MA);
        const bool target_voltage_set = _pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
        result = pmu_i2c_error.load();
        if (result == ESP_OK && (!constant_current_set || !target_voltage_set)) {
            result = ESP_FAIL;
        }
        if (result != ESP_OK) {
            cleanupDevice();
            return result;
        }

        ESP_LOGI(TAG, "AXP2101 is online");
        return ESP_OK;
    }

    esp_err_t readBattery(Snapshot &snapshot)
    {
        if (pmu_device == nullptr) {
            return ESP_ERR_INVALID_STATE;
        }

        pmu_i2c_error.store(ESP_OK);
        snapshot.battery_present = _pmu.isBatteryConnect();
        snapshot.charger_status = static_cast<uint8_t>(_pmu.getChargerStatus());
        if (_pmu.isCharging()) {
            snapshot.battery_power_state = BatteryPowerState::CHARGING;
        } else if (_pmu.isDischarge()) {
            snapshot.battery_power_state = BatteryPowerState::DISCHARGING;
        } else if (_pmu.isStandby()) {
            snapshot.battery_power_state = BatteryPowerState::STANDBY;
        } else {
            snapshot.battery_power_state = BatteryPowerState::UNKNOWN;
        }
        esp_err_t result = pmu_i2c_error.load();
        if (result != ESP_OK) {
            return result;
        }

        snapshot.battery_valid = true;
        snapshot.charging = snapshot.battery_power_state == BatteryPowerState::CHARGING;

        if (!snapshot.battery_present) {
            snapshot.battery_percent = -1;
            return ESP_OK;
        }

        const int percent = _pmu.getBatteryPercent();
        result = pmu_i2c_error.load();
        if (result != ESP_OK) {
            snapshot.battery_valid = false;
            return result;
        }
        if (percent < 0 || percent > 100) {
            snapshot.battery_valid = false;
            return ESP_ERR_INVALID_RESPONSE;
        }

        snapshot.battery_percent = percent;
        return ESP_OK;
    }

    esp_err_t loadWifiEnabled(bool &enabled) const
    {
        enabled = false;
        nvs_handle_t handle = 0;
        esp_err_t result = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
        if (result == ESP_ERR_NVS_NOT_FOUND) {
            return ESP_OK;
        }
        if (result != ESP_OK) {
            return result;
        }

        int32_t value = 0;
        result = nvs_get_i32(handle, WIFI_NVS_ENABLED_KEY, &value);
        nvs_close(handle);
        if (result == ESP_ERR_NVS_NOT_FOUND) {
            return ESP_OK;
        }
        if (result != ESP_OK) {
            return result;
        }
        enabled = value != 0;
        return ESP_OK;
    }

    esp_err_t persistWifiEnabled(bool enabled) const
    {
        nvs_handle_t handle = 0;
        esp_err_t result = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
        if (result != ESP_OK) {
            return result;
        }
        result = nvs_set_i32(handle, WIFI_NVS_ENABLED_KEY, enabled ? 1 : 0);
        if (result == ESP_OK) {
            result = nvs_commit(handle);
        }
        nvs_close(handle);
        return result;
    }

    esp_err_t applyWifiEnabledState(bool enabled)
    {
        if (enabled) {
            if (!_wifi_started.load()) {
                esp_err_t result = esp_wifi_set_mode(WIFI_MODE_STA);
                if (result != ESP_OK) {
                    return result;
                }
                result = esp_wifi_start();
                if (result != ESP_OK) {
                    return result;
                }
                _wifi_started.store(true);
            }
            _wifi_reconnect_requested.store(true);
            return ESP_OK;
        }

        _wifi_reconnect_requested.store(false);
        _wifi_event_connected.store(false);
        (void)esp_wifi_scan_stop();
        (void)esp_wifi_disconnect();
        if (!_wifi_started.load()) {
            return ESP_OK;
        }

        const esp_err_t result = esp_wifi_stop();
        if (result == ESP_OK || result == ESP_ERR_WIFI_NOT_STARTED) {
            _wifi_started.store(false);
            return ESP_OK;
        }
        return result;
    }

    void serviceWifiReconnect()
    {
        if (!_wifi_enabled.load() || !_wifi_started.load() ||
                _manual_wifi_configuration.load() ||
                !_wifi_reconnect_requested.exchange(false)) {
            return;
        }

        const esp_err_t result = esp_wifi_connect();
        if (result == ESP_OK) {
            _last_wifi_connect_error = ESP_OK;
            return;
        }

        if (_last_wifi_connect_error != result) {
            ESP_LOGW(TAG, "Wi-Fi reconnect request failed: %s", esp_err_to_name(result));
            _last_wifi_connect_error = result;
        }
        if (result != ESP_ERR_WIFI_SSID) {
            _wifi_reconnect_requested.store(true);
        }
    }

    void updateStatusBar(const Snapshot &snapshot) const
    {
        if (_status_bar == nullptr || !_running.load()) {
            return;
        }

        // This monitor task does not own the LVGL lock (or the Wi-Fi mutex) on
        // entry.  Wait until the LVGL worker completes its current frame rather
        // than emitting a timeout every refresh cycle during an expensive draw.
        // stop() is not called from an LVGL callback in this firmware.
        auto &gui_lock = esp_brookesia::gui::LvLock::getInstance();
        if (!gui_lock.lock(-1)) {
            return;
        }
        if (!_running.load()) {
            gui_lock.unlock();
            return;
        }

        if (!snapshot.battery_valid || !snapshot.battery_present) {
            _status_bar->hideBatteryIcon();
        } else {
            // The compact status bar has an icon but no percentage label.
            // setBatteryPercent updates the icon and checks the label flag.
            _status_bar->setBatteryPercent(snapshot.charging, snapshot.battery_percent);
            _status_bar->showBatteryIcon();
        }

        StatusBar::WifiState wifi_state = StatusBar::WifiState::DISCONNECTED;
        if (snapshot.wifi_connected) {
            if (snapshot.wifi_rssi >= -55) {
                wifi_state = StatusBar::WifiState::SIGNAL_3;
            } else if (snapshot.wifi_rssi >= -70) {
                wifi_state = StatusBar::WifiState::SIGNAL_2;
            } else {
                wifi_state = StatusBar::WifiState::SIGNAL_1;
            }
        }
        _status_bar->setWifiIconState(wifi_state);
        gui_lock.unlock();
    }

    void unregisterEventHandlers()
    {
        if (_wifi_event_instance != nullptr) {
            esp_event_handler_instance_unregister(
                WIFI_EVENT, ESP_EVENT_ANY_ID, _wifi_event_instance
            );
            _wifi_event_instance = nullptr;
        }
        if (_ip_event_instance != nullptr) {
            esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, _ip_event_instance);
            _ip_event_instance = nullptr;
        }
    }

    void cleanupDevice()
    {
        if (pmu_device != nullptr) {
            i2c_master_bus_rm_device(pmu_device);
            pmu_device = nullptr;
        }
        pmu_i2c_error.store(ESP_OK);
    }

    StatusBar *_status_bar = nullptr;
    XPowersPMU _pmu;
    esp_event_handler_instance_t _wifi_event_instance = nullptr;
    esp_event_handler_instance_t _ip_event_instance = nullptr;
    SemaphoreHandle_t _api_mutex = nullptr;
    SemaphoreHandle_t _wifi_mutex = nullptr;
    std::atomic<TaskHandle_t> _task = nullptr;
    std::atomic<bool> _running = false;
    std::atomic<bool> _wifi_enabled = false;
    std::atomic<bool> _wifi_started = false;
    std::atomic<bool> _wifi_event_connected = false;
    std::atomic<bool> _wifi_reconnect_requested = false;
    std::atomic<bool> _manual_wifi_configuration = false;
    uint32_t _refresh_period_ms = 1000;
    esp_err_t _last_battery_error = ESP_OK;
    esp_err_t _last_pmu_init_error = ESP_OK;
    esp_err_t _last_wifi_start_error = ESP_OK;
    esp_err_t _last_wifi_connect_error = ESP_OK;
    unsigned _battery_error_streak = 0;
    unsigned _pmu_retry_countdown = 0;

    portMUX_TYPE _snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
    Snapshot _snapshot = {};
};

Monitor &monitor()
{
    static Monitor instance;
    return instance;
}

} // namespace

esp_err_t start(StatusBar *status_bar, uint32_t refresh_period_ms)
{
    return monitor().start(status_bar, refresh_period_ms);
}

esp_err_t stop()
{
    return monitor().stop();
}

bool get_snapshot(Snapshot &snapshot)
{
    return monitor().getSnapshot(snapshot);
}

esp_err_t set_wifi_enabled(bool enabled)
{
    return monitor().setWifiEnabled(enabled);
}

esp_err_t connect_wifi(const wifi_config_t &config)
{
    return monitor().connectWifi(config);
}

esp_err_t request_wifi_connect()
{
    return monitor().requestWifiConnect();
}

} // namespace brookesia::system_status
