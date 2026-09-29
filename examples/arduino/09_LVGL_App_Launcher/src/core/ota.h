#pragma once

#include <Arduino.h>

enum OtaStage : uint8_t {
  OTA_IDLE,
  OTA_CHECKING,
  OTA_UP_TO_DATE,
  OTA_AVAILABLE,
  OTA_INSTALLING,
  OTA_REBOOTING,
  OTA_FAILED,
};

struct OtaStatus {
  OtaStage stage;
  int progress;         // 0 - 100 for the item being written
  char message[128];    // current step / error text
  char available[224];  // summary of what the server offers
};

// Boot hook: reads rollback state of the running image
void ota_begin(void);
// Call from loop(): confirms a freshly updated image after it has run healthy for a while
void ota_confirm_tick(void);

// Background jobs (run in their own FreeRTOS task, never touch LVGL)
bool ota_check_async(void);
bool ota_install_async(void);
OtaStatus ota_get_status(void);

const char *ota_running_partition(void);
bool ota_is_pending_verify(void);
bool ota_can_rollback(void);
void ota_rollback_and_reboot(void);
// Installed version of a module data partition ("" = empty / never installed)
String ota_data_version(const char *label);
