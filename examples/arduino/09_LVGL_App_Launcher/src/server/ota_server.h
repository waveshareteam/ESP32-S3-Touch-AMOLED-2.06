#pragma once

#include <Arduino.h>

// Default manifest location baked into the firmware (can be changed in Settings > Update).
// Must be an https:// URL, e.g. a GitHub Release asset:
//   https://github.com/<owner>/<repo>/releases/latest/download/manifest.json
#ifndef OTA_DEFAULT_MANIFEST_URL
#define OTA_DEFAULT_MANIFEST_URL ""
#endif

#define OTA_MANIFEST_SCHEMA 1
#define OTA_MAX_DATA_ITEMS 8

struct OtaItem {
  char name[17];         // "firmware" or the data partition label
  char version[24];
  char url[256];
  uint32_t size;
  uint8_t sha256[32];
  bool is_app;
};

struct OtaManifest {
  bool has_firmware;
  OtaItem firmware;
  int data_count;
  OtaItem data[OTA_MAX_DATA_ITEMS];
  char modules_changed[192];  // e.g. "launcher 1.0.0>1.1.0, apps 1.0.0>1.0.1"
};

String server_manifest_url(void);
void server_set_manifest_url(const char *url);

// Downloads and validates manifest.json (schema, board id, URLs, sizes, SHA-256 format).
bool server_fetch_manifest(OtaManifest *out, char *error, size_t error_len);
