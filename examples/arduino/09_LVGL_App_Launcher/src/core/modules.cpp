#include "core_version.h"
#include "../board/board_version.h"
#include "../audio/audio_version.h"
#include "../protocol/protocol_version.h"
#include "../server/server_version.h"
#include "../apps/apps_version.h"
#include "../components/components_version.h"
#include "../launcher/launcher_version.h"
#include "modules.h"

// Data partition labels must match partitions.csv in the sketch folder
const ModuleInfo MODULES[] = {
  { "core", CORE_MODULE_VERSION, nullptr },  // uses nvs + otadata
  { "board", BOARD_MODULE_VERSION, "board" },
  { "audio", AUDIO_MODULE_VERSION, "audio" },
  { "protocol", PROTOCOL_MODULE_VERSION, "protocol" },
  { "server", SERVER_MODULE_VERSION, "server" },
  { "apps", APPS_MODULE_VERSION, "apps" },
  { "components", COMPONENTS_MODULE_VERSION, "components" },
  { "launcher", LAUNCHER_MODULE_VERSION, "launcher" },
};

const size_t MODULE_COUNT = sizeof(MODULES) / sizeof(MODULES[0]);

const ModuleInfo *module_find(const char *name) {
  for (size_t i = 0; i < MODULE_COUNT; i++) {
    if (strcmp(MODULES[i].name, name) == 0) {
      return &MODULES[i];
    }
  }
  return nullptr;
}

const ModuleInfo *module_find_by_partition(const char *label) {
  for (size_t i = 0; i < MODULE_COUNT; i++) {
    if (MODULES[i].data_partition != nullptr && strcmp(MODULES[i].data_partition, label) == 0) {
      return &MODULES[i];
    }
  }
  return nullptr;
}

int version_compare(const char *a, const char *b) {
  while (*a != '\0' || *b != '\0') {
    long na = strtol(a, (char **)&a, 10);
    long nb = strtol(b, (char **)&b, 10);
    if (na != nb) {
      return na < nb ? -1 : 1;
    }
    // Skip the separator ('.') or stop at any non-numeric suffix
    if (*a == '.') a++;
    else if (*a != '\0') break;
    if (*b == '.') b++;
    else if (*b != '\0') break;
  }
  return 0;
}
