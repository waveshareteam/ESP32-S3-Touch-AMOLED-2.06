#pragma once

#include <Arduino.h>

enum NetState : uint8_t {
  NET_IDLE,
  NET_CONNECTING,
  NET_CONNECTED,
  NET_FAILED,
};

struct NetScanEntry {
  char ssid[33];
  int32_t rssi;
  bool secure;
};

static const int NET_SCAN_MAX = 12;
static const int NET_SCAN_RUNNING = -1;
static const int NET_SCAN_FAILED = -2;

void net_begin(void);  // auto-connect with the saved credentials, if any
void net_poll(void);   // call from loop(): connection timeout + NTP -> RTC

void net_scan_start(void);
// Number of unique networks (sorted by RSSI), or NET_SCAN_RUNNING / NET_SCAN_FAILED
int net_scan_result(void);
const NetScanEntry *net_scan_entry(int index);

void net_connect(const char *ssid, const char *password);
void net_disconnect(void);
NetState net_state(void);
String net_ip(void);
String net_ssid(void);
