#pragma once

#include <Arduino.h>

struct TzEntry {
  const char *name;   // IANA name shown to the user
  const char *posix;  // POSIX TZ string used by setenv("TZ")
};

// Built-in table: ESP32 has no tz database, so only these names are accepted.
static const TzEntry TZ_TABLE[] = {
  { "Asia/Bangkok", "<+07>-7" },
  { "Asia/Ho_Chi_Minh", "<+07>-7" },
  { "Asia/Jakarta", "WIB-7" },
  { "Asia/Yangon", "<+0630>-6:30" },
  { "Asia/Kolkata", "IST-5:30" },
  { "Asia/Dubai", "<+04>-4" },
  { "Asia/Singapore", "<+08>-8" },
  { "Asia/Kuala_Lumpur", "<+08>-8" },
  { "Asia/Shanghai", "CST-8" },
  { "Asia/Hong_Kong", "HKT-8" },
  { "Asia/Taipei", "CST-8" },
  { "Asia/Manila", "PST-8" },
  { "Asia/Tokyo", "JST-9" },
  { "Asia/Seoul", "KST-9" },
  { "Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
  { "Europe/London", "GMT0BST,M3.5.0/1,M10.5.0" },
  { "Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3" },
  { "Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3" },
  { "America/New_York", "EST5EDT,M3.2.0,M11.1.0" },
  { "America/Chicago", "CST6CDT,M3.2.0,M11.1.0" },
  { "America/Denver", "MST7MDT,M3.2.0,M11.1.0" },
  { "America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0" },
  { "UTC", "UTC0" },
};

static const size_t TZ_TABLE_COUNT = sizeof(TZ_TABLE) / sizeof(TZ_TABLE[0]);

// Case-insensitive lookup; returns nullptr when the name is unknown.
inline const TzEntry *tz_find(const char *name) {
  if (name == nullptr) {
    return nullptr;
  }
  for (size_t i = 0; i < TZ_TABLE_COUNT; i++) {
    if (strcasecmp(TZ_TABLE[i].name, name) == 0) {
      return &TZ_TABLE[i];
    }
  }
  return nullptr;
}
