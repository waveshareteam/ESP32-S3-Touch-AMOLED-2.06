#include <Preferences.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "mbedtls/sha256.h"
#include "../protocol/https_client.h"
#include "../protocol/net.h"
#include "../server/ota_server.h"
#include "app_switch.h"
#include "modules.h"
#include "ota.h"

#define OTA_TASK_STACK 10240
#define OTA_CONFIRM_AFTER_MS 15000
#define OTA_SECTOR_SIZE 4096

static const char *NVS_NAMESPACE = "ota_ver";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static OtaStatus s_status = { OTA_IDLE, 0, "", "" };
static OtaManifest s_manifest;  // written only by the background task
static bool s_manifest_valid = false;
static volatile bool s_busy = false;
static bool s_pending_verify = false;
static uint32_t s_boot_ms = 0;

// The Arduino core marks every image valid at boot unless this returns true.
// Returning true lets ota_confirm_tick() decide, so a broken update rolls back.
extern "C" bool verifyRollbackLater() {
  return true;
}

/* ---------------- Status helpers ---------------- */

static void set_status(OtaStage stage, int progress, const char *fmt, ...) {
  char msg[sizeof(s_status.message)];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);

  portENTER_CRITICAL(&s_lock);
  s_status.stage = stage;
  s_status.progress = progress;
  memcpy(s_status.message, msg, sizeof(msg));
  portEXIT_CRITICAL(&s_lock);
}

static void set_available(const char *text) {
  portENTER_CRITICAL(&s_lock);
  strlcpy(s_status.available, text, sizeof(s_status.available));
  portEXIT_CRITICAL(&s_lock);
}

OtaStatus ota_get_status(void) {
  OtaStatus copy;
  portENTER_CRITICAL(&s_lock);
  copy = s_status;
  portEXIT_CRITICAL(&s_lock);
  return copy;
}

/* ---------------- Versions ---------------- */

String ota_data_version(const char *label) {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true);
  String v = prefs.getString(label, "");
  prefs.end();
  return v;
}

static void store_data_version(const char *label, const char *version) {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putString(label, version);
  prefs.end();
}

static bool firmware_needs_update(void) {
  return s_manifest.has_firmware && version_compare(s_manifest.firmware.version, FIRMWARE_VERSION) > 0;
}

static bool data_needs_update(const OtaItem &item) {
  return ota_data_version(item.name) != item.version;
}

/* ---------------- Boot / rollback ---------------- */

void ota_begin(void) {
  s_boot_ms = millis();
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (running != nullptr && esp_ota_get_state_partition(running, &state) == ESP_OK) {
    s_pending_verify = (state == ESP_OTA_IMG_PENDING_VERIFY);
  }
}

void ota_confirm_tick(void) {
  // Reaching this point repeatedly means the UI loop is alive; confirm after a grace period
  if (s_pending_verify && millis() - s_boot_ms > OTA_CONFIRM_AFTER_MS) {
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
      s_pending_verify = false;
    }
  }
}

bool ota_is_pending_verify(void) {
  return s_pending_verify;
}

const char *ota_running_partition(void) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  return running ? running->label : "?";
}

// Rollback stays inside the Launcher pair; the generic IDF rollback could pick the AI Voice slot
bool ota_can_rollback(void) {
  const esp_partition_t *partner = app_switch_pair_partner(esp_ota_get_running_partition());
  esp_app_desc_t desc;
  return partner != nullptr && esp_ota_get_partition_description(partner, &desc) == ESP_OK;
}

void ota_rollback_and_reboot(void) {
  const esp_partition_t *partner = app_switch_pair_partner(esp_ota_get_running_partition());
  if (partner != nullptr && esp_ota_set_boot_partition(partner) == ESP_OK) {
    esp_restart();
  }
}

/* ---------------- Download + write ---------------- */

struct WriteCtx {
  const OtaItem *item;
  const esp_partition_t *partition;
  esp_ota_handle_t ota_handle;  // app images only
  size_t offset;
  mbedtls_sha256_context sha;
  bool failed;
};

static bool write_chunk(const uint8_t *data, size_t len, void *arg) {
  WriteCtx *ctx = (WriteCtx *)arg;
  if (ctx->offset + len > ctx->item->size) {
    ctx->failed = true;
    return false;
  }
  esp_err_t err = ctx->item->is_app ? esp_ota_write(ctx->ota_handle, data, len)
                                    : esp_partition_write(ctx->partition, ctx->offset, data, len);
  if (err != ESP_OK) {
    ctx->failed = true;
    return false;
  }
  mbedtls_sha256_update(&ctx->sha, data, len);
  ctx->offset += len;
  set_status(OTA_INSTALLING, (int)(ctx->offset * 100 / ctx->item->size), "Writing %s %d%%", ctx->item->name,
             (int)(ctx->offset * 100 / ctx->item->size));
  return true;
}

static bool install_item(const OtaItem &item) {
  WriteCtx ctx = {};
  ctx.item = &item;

  if (item.is_app) {
    // Stay inside our own A/B pair: the "next" OTA slot could belong to another firmware
    ctx.partition = app_switch_pair_partner(esp_ota_get_running_partition());
  } else {
    ctx.partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, item.name);
  }
  if (ctx.partition == nullptr) {
    set_status(OTA_FAILED, 0, "%s: partition not found", item.name);
    return false;
  }
  if (item.size > ctx.partition->size) {
    set_status(OTA_FAILED, 0, "%s: image larger than partition", item.name);
    return false;
  }

  set_status(OTA_INSTALLING, 0, "Preparing %s", item.name);
  if (item.is_app) {
    if (esp_ota_begin(ctx.partition, item.size, &ctx.ota_handle) != ESP_OK) {
      set_status(OTA_FAILED, 0, "firmware: esp_ota_begin failed");
      return false;
    }
  } else {
    // Forget the old version first so an interrupted write is never reported as installed
    store_data_version(item.name, "");
    size_t erase = (item.size + OTA_SECTOR_SIZE - 1) / OTA_SECTOR_SIZE * OTA_SECTOR_SIZE;
    if (esp_partition_erase_range(ctx.partition, 0, erase) != ESP_OK) {
      set_status(OTA_FAILED, 0, "%s: erase failed", item.name);
      return false;
    }
  }

  mbedtls_sha256_init(&ctx.sha);
  mbedtls_sha256_starts(&ctx.sha, 0);
  HttpsResult res = https_get(item.url, item.size, write_chunk, &ctx);
  uint8_t digest[32];
  mbedtls_sha256_finish(&ctx.sha, digest);
  mbedtls_sha256_free(&ctx.sha);

  bool ok = res.ok && !ctx.failed && ctx.offset == item.size;
  if (!ok) {
    set_status(OTA_FAILED, 0, "%s: %s", item.name, res.ok ? "write/size error" : res.error);
  } else if (memcmp(digest, item.sha256, sizeof(digest)) != 0) {
    set_status(OTA_FAILED, 0, "%s: SHA-256 mismatch, rejected", item.name);
    ok = false;
  }

  if (item.is_app) {
    if (!ok) {
      esp_ota_abort(ctx.ota_handle);
      return false;
    }
    // esp_ota_end() also validates the image header and its own checksum
    if (esp_ota_end(ctx.ota_handle) != ESP_OK || esp_ota_set_boot_partition(ctx.partition) != ESP_OK) {
      set_status(OTA_FAILED, 0, "firmware: image validation failed");
      return false;
    }
  } else {
    if (!ok) {
      esp_partition_erase_range(ctx.partition, 0, OTA_SECTOR_SIZE);  // leave no half-valid data
      return false;
    }
    store_data_version(item.name, item.version);
  }
  return true;
}

/* ---------------- Background tasks ---------------- */

static void summarize_manifest(void) {
  char text[sizeof(s_status.available)] = "";
  if (firmware_needs_update()) {
    snprintf(text, sizeof(text), "Firmware %s > %s", FIRMWARE_VERSION, s_manifest.firmware.version);
    if (s_manifest.modules_changed[0] != '\0') {
      strlcat(text, "\nModules: ", sizeof(text));
      strlcat(text, s_manifest.modules_changed, sizeof(text));
    }
  }
  for (int i = 0; i < s_manifest.data_count; i++) {
    const OtaItem &item = s_manifest.data[i];
    if (data_needs_update(item)) {
      char line[64];
      snprintf(line, sizeof(line), "%sData %s > %s", text[0] ? "\n" : "", item.name, item.version);
      strlcat(text, line, sizeof(text));
    }
  }
  set_available(text);
}

static int pending_item_count(void) {
  int count = firmware_needs_update() ? 1 : 0;
  for (int i = 0; i < s_manifest.data_count; i++) {
    if (data_needs_update(s_manifest.data[i])) {
      count++;
    }
  }
  return count;
}

static void check_task(void *arg) {
  (void)arg;
  char error[96] = "";
  s_manifest_valid = server_fetch_manifest(&s_manifest, error, sizeof(error));
  if (!s_manifest_valid) {
    set_available("");
    set_status(OTA_FAILED, 0, "%s", error);
  } else {
    summarize_manifest();
    if (pending_item_count() == 0) {
      set_status(OTA_UP_TO_DATE, 0, "Up to date (%s)", FIRMWARE_VERSION);
    } else {
      set_status(OTA_AVAILABLE, 0, "%d update(s) available", pending_item_count());
    }
  }
  s_busy = false;
  vTaskDelete(nullptr);
}

static void install_task(void *arg) {
  (void)arg;
  bool ok = true;
  // Data partitions first, so the new firmware boots with the data it expects
  for (int i = 0; ok && i < s_manifest.data_count; i++) {
    if (data_needs_update(s_manifest.data[i])) {
      ok = install_item(s_manifest.data[i]);
    }
  }
  bool reboot = false;
  if (ok && firmware_needs_update()) {
    ok = install_item(s_manifest.firmware);
    reboot = ok;
  }

  if (ok && reboot) {
    set_status(OTA_REBOOTING, 100, "Installed. Restarting...");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
  } else if (ok) {
    summarize_manifest();
    set_status(OTA_UP_TO_DATE, 100, "Data updated");
  }
  s_busy = false;
  vTaskDelete(nullptr);
}

static bool start_task(TaskFunction_t fn, const char *name) {
  if (s_busy) {
    return false;
  }
  if (net_state() != NET_CONNECTED) {
    set_status(OTA_FAILED, 0, "Wi-Fi is not connected");
    return false;
  }
  s_busy = true;
  if (xTaskCreate(fn, name, OTA_TASK_STACK, nullptr, 3, nullptr) != pdPASS) {
    s_busy = false;
    set_status(OTA_FAILED, 0, "Cannot start task");
    return false;
  }
  return true;
}

bool ota_check_async(void) {
  set_status(OTA_CHECKING, 0, "Checking server...");
  return start_task(check_task, "ota_check");
}

bool ota_install_async(void) {
  if (!s_manifest_valid || pending_item_count() == 0) {
    return false;
  }
  set_status(OTA_INSTALLING, 0, "Starting download...");
  return start_task(install_task, "ota_install");
}
