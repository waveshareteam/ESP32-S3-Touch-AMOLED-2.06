#include <WiFi.h>
#include "esp_sntp.h"
#include "../board/board.h"
#include "../core/settings_store.h"
#include "../components/timezones.h"
#include "../protocol/net.h"

#define NET_CONNECT_TIMEOUT_MS 15000

static NetState s_state = NET_IDLE;
static uint32_t s_connect_start_ms = 0;
static bool s_scan_active = false;
static int s_scan_count = 0;
static NetScanEntry s_scan[NET_SCAN_MAX];
static volatile bool s_ntp_synced = false;

static void on_time_synced(struct timeval *tv) {
  (void)tv;
  s_ntp_synced = true;  // runs in the SNTP task; the RTC write happens in net_poll()
}

static void start_ntp(void) {
  const TzEntry *tz = tz_find(g_settings.tz_name);
  sntp_set_time_sync_notification_cb(on_time_synced);
  configTzTime(tz ? tz->posix : "UTC0", "pool.ntp.org", "time.google.com");
}

void net_begin(void) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  if (g_settings.wifi_ssid[0] != '\0') {
    net_connect(g_settings.wifi_ssid, g_settings.wifi_pass);
  }
}

void net_poll(void) {
  if (s_state == NET_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      s_state = NET_CONNECTED;
      start_ntp();
    } else if (millis() - s_connect_start_ms > NET_CONNECT_TIMEOUT_MS) {
      WiFi.disconnect();
      s_state = NET_FAILED;
    }
  } else if (s_state == NET_CONNECTED && WiFi.status() != WL_CONNECTED) {
    s_state = NET_CONNECTING;  // auto-reconnect is running
    s_connect_start_ms = millis();
  }

  if (s_ntp_synced) {
    s_ntp_synced = false;
    board_time_save_to_rtc();
  }
}

void net_scan_start(void) {
  WiFi.scanDelete();
  s_scan_count = 0;
  s_scan_active = (WiFi.scanNetworks(true /* async */) == WIFI_SCAN_RUNNING);
}

int net_scan_result(void) {
  if (!s_scan_active) {
    return s_scan_count;
  }
  int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    return NET_SCAN_RUNNING;
  }
  s_scan_active = false;
  if (n < 0) {
    return NET_SCAN_FAILED;
  }

  // Keep the strongest entry per SSID, drop hidden networks, sort by RSSI (insertion sort)
  s_scan_count = 0;
  for (int16_t i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) {
      continue;
    }
    int32_t rssi = WiFi.RSSI(i);
    int existing = -1;
    for (int j = 0; j < s_scan_count; j++) {
      if (ssid.equals(s_scan[j].ssid)) {
        existing = j;
        break;
      }
    }
    if (existing >= 0) {
      if (rssi <= s_scan[existing].rssi) {
        continue;
      }
      // Remove the weaker duplicate before re-inserting
      for (int j = existing; j < s_scan_count - 1; j++) {
        s_scan[j] = s_scan[j + 1];
      }
      s_scan_count--;
    }
    NetScanEntry entry;
    strlcpy(entry.ssid, ssid.c_str(), sizeof(entry.ssid));
    entry.rssi = rssi;
    entry.secure = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);

    int pos = s_scan_count;
    while (pos > 0 && s_scan[pos - 1].rssi < rssi) {
      if (pos < NET_SCAN_MAX) {
        s_scan[pos] = s_scan[pos - 1];
      }
      pos--;
    }
    if (pos < NET_SCAN_MAX) {
      s_scan[pos] = entry;
      if (s_scan_count < NET_SCAN_MAX) {
        s_scan_count++;
      }
    }
  }
  WiFi.scanDelete();
  return s_scan_count;
}

const NetScanEntry *net_scan_entry(int index) {
  if (index < 0 || index >= s_scan_count) {
    return nullptr;
  }
  return &s_scan[index];
}

void net_connect(const char *ssid, const char *password) {
  WiFi.disconnect();
  if (password != nullptr && password[0] != '\0') {
    WiFi.begin(ssid, password);
  } else {
    WiFi.begin(ssid);
  }
  s_state = NET_CONNECTING;
  s_connect_start_ms = millis();
}

void net_disconnect(void) {
  WiFi.disconnect();
  s_state = NET_IDLE;
}

NetState net_state(void) {
  return s_state;
}

String net_ip(void) {
  return WiFi.localIP().toString();
}

String net_ssid(void) {
  return WiFi.SSID();
}
