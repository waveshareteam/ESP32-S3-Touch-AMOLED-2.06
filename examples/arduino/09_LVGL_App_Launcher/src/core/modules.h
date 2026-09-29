#pragma once

#include <Arduino.h>

// Firmware (app image) version. Every code module is linked into this one image, so a
// firmware OTA replaces all modules together; the per-module versions below tell which
// modules actually changed between two firmware releases.
#define FIRMWARE_VERSION "1.3.3"
#define FIRMWARE_BOARD_ID "ESP32-S3-Touch-AMOLED-2.06"

struct ModuleInfo {
  const char *name;           // key used in the OTA manifest "modules" object
  const char *version;
  const char *data_partition; // label of the module's own data partition (nullptr = none)
};

extern const ModuleInfo MODULES[];
extern const size_t MODULE_COUNT;

const ModuleInfo *module_find(const char *name);
const ModuleInfo *module_find_by_partition(const char *label);

// Semantic version compare ("1.2.10" > "1.2.9"). Returns <0, 0, >0.
int version_compare(const char *a, const char *b);
