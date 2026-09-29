#include <string.h>
#include "modules.h"
#include "app_switch.h"

// The AI Voice slots can also hold other images (e.g. a rejected LAN upload or staged assets);
// only boot images that identify as the xiaozhi firmware.
#define AI_VOICE_PROJECT_NAME "xiaozhi"

static bool is_ota_slot(const esp_partition_t *p) {
  return p != nullptr && p->type == ESP_PARTITION_TYPE_APP && p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
         p->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX;
}

const esp_partition_t *app_switch_pair_partner(const esp_partition_t *app) {
  if (!is_ota_slot(app)) {
    return nullptr;
  }
  // Pairs are (ota_0, ota_1), (ota_2, ota_3), ...: flip the lowest bit of the slot index
  int slot = app->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
  auto partner = (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_MIN + (slot ^ 1));
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP, partner, nullptr);
}

// Prefers images that were not rolled back, then the newer version
static const esp_partition_t *pick_newest(esp_partition_subtype_t a, esp_partition_subtype_t b,
                                          const char *project, esp_app_desc_t *desc_out) {
  const esp_partition_t *best = nullptr;
  esp_app_desc_t best_desc = {};
  bool best_healthy = false;

  for (esp_partition_subtype_t sub : { a, b }) {
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, sub, nullptr);
    esp_app_desc_t desc;
    if (p == nullptr || esp_ota_get_partition_description(p, &desc) != ESP_OK) {
      continue;  // missing or empty slot
    }
    if (project != nullptr && strncmp(desc.project_name, project, sizeof(desc.project_name)) != 0) {
      continue;  // some other firmware
    }
    esp_ota_img_states_t state;
    bool healthy = !(esp_ota_get_state_partition(p, &state) == ESP_OK &&
                     (state == ESP_OTA_IMG_INVALID || state == ESP_OTA_IMG_ABORTED));
    bool better = (best == nullptr) || (healthy && !best_healthy) ||
                  (healthy == best_healthy && version_compare(desc.version, best_desc.version) > 0);
    if (better) {
      best = p;
      best_desc = desc;
      best_healthy = healthy;
    }
  }
  if (best != nullptr && desc_out != nullptr) {
    *desc_out = best_desc;
  }
  return best;
}

const esp_partition_t *app_switch_find_ai_voice(esp_app_desc_t *desc_out) {
  return pick_newest(ESP_PARTITION_SUBTYPE_APP_OTA_2, ESP_PARTITION_SUBTYPE_APP_OTA_3, AI_VOICE_PROJECT_NAME, desc_out);
}

bool app_switch_select_ai_voice(void) {
  const esp_partition_t *target = app_switch_find_ai_voice(nullptr);
  // esp_ota_set_boot_partition() verifies the whole image before switching
  return target != nullptr && esp_ota_set_boot_partition(target) == ESP_OK;
}
