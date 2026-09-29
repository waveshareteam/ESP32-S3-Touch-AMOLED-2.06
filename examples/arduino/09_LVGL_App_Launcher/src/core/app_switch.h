#pragma once

#include <Arduino.h>
#include "esp_ota_ops.h"

// Several firmwares share this flash, each with its own A/B pair of app slots:
//   Launcher: ota_0 / ota_1      AI Voice (xiaozhi): ota_2 / ota_3
// Switching firmware = pointing otadata at the other pair and restarting.

// Partner slot inside the same A/B pair (ota_0<->ota_1, ota_2<->ota_3); nullptr if missing
const esp_partition_t *app_switch_pair_partner(const esp_partition_t *app);

// Best image of the AI Voice pair (valid description, not rolled back, newest version)
const esp_partition_t *app_switch_find_ai_voice(esp_app_desc_t *desc_out);

// Marks the AI Voice image as boot partition. Call esp_restart() afterwards.
bool app_switch_select_ai_voice(void);
