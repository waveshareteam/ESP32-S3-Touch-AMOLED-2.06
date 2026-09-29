#include <Preferences.h>
#include "cJSON.h"
#include "../core/modules.h"
#include "../protocol/https_client.h"
#include "ota_server.h"

#define MANIFEST_MAX_BYTES 8192

static const char *NVS_NAMESPACE = "server";

String server_manifest_url(void) {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true);
  String url = prefs.getString("manifest", OTA_DEFAULT_MANIFEST_URL);
  prefs.end();
  return url;
}

void server_set_manifest_url(const char *url) {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putString("manifest", url);
  prefs.end();
}

struct Buffer {
  char *data;
  size_t len;
};

static bool collect_chunk(const uint8_t *data, size_t len, void *ctx) {
  Buffer *buf = (Buffer *)ctx;
  memcpy(buf->data + buf->len, data, len);
  buf->len += len;
  return true;
}

static bool parse_sha256(const char *hex, uint8_t out[32]) {
  if (hex == nullptr || strlen(hex) != 64) {
    return false;
  }
  for (int i = 0; i < 32; i++) {
    char byte[3] = { hex[i * 2], hex[i * 2 + 1], '\0' };
    char *end = nullptr;
    out[i] = (uint8_t)strtoul(byte, &end, 16);
    if (end != byte + 2) {
      return false;
    }
  }
  return true;
}

static bool parse_item(const cJSON *obj, const char *name, bool is_app, OtaItem *item, char *error, size_t error_len) {
  const cJSON *version = cJSON_GetObjectItemCaseSensitive(obj, "version");
  const cJSON *url = cJSON_GetObjectItemCaseSensitive(obj, "url");
  const cJSON *size = cJSON_GetObjectItemCaseSensitive(obj, "size");
  const cJSON *sha = cJSON_GetObjectItemCaseSensitive(obj, "sha256");

  if (!cJSON_IsString(version) || !cJSON_IsString(url) || !cJSON_IsNumber(size) || !cJSON_IsString(sha)) {
    snprintf(error, error_len, "%s: missing version/url/size/sha256", name);
    return false;
  }
  if (strncmp(url->valuestring, "https://", 8) != 0 || strlen(url->valuestring) >= sizeof(item->url)) {
    snprintf(error, error_len, "%s: url must be https:// (<256 chars)", name);
    return false;
  }
  if (size->valuedouble <= 0 || size->valuedouble > 16 * 1024 * 1024) {
    snprintf(error, error_len, "%s: invalid size", name);
    return false;
  }
  if (!parse_sha256(sha->valuestring, item->sha256)) {
    snprintf(error, error_len, "%s: sha256 must be 64 hex chars", name);
    return false;
  }
  strlcpy(item->name, name, sizeof(item->name));
  strlcpy(item->version, version->valuestring, sizeof(item->version));
  strlcpy(item->url, url->valuestring, sizeof(item->url));
  item->size = (uint32_t)size->valuedouble;
  item->is_app = is_app;
  return true;
}

static void describe_module_changes(const cJSON *modules, OtaManifest *out) {
  out->modules_changed[0] = '\0';
  const cJSON *entry = nullptr;
  cJSON_ArrayForEach(entry, modules) {
    if (!cJSON_IsString(entry) || entry->string == nullptr) {
      continue;
    }
    const ModuleInfo *local = module_find(entry->string);
    const char *from = local ? local->version : "new";
    if (local != nullptr && strcmp(local->version, entry->valuestring) == 0) {
      continue;
    }
    char part[48];
    snprintf(part, sizeof(part), "%s%s %s>%s", out->modules_changed[0] ? ", " : "", entry->string, from,
             entry->valuestring);
    strlcat(out->modules_changed, part, sizeof(out->modules_changed));
  }
}

bool server_fetch_manifest(OtaManifest *out, char *error, size_t error_len) {
  memset(out, 0, sizeof(*out));
  String url = server_manifest_url();
  if (url.length() == 0) {
    snprintf(error, error_len, "No update server configured");
    return false;
  }

  Buffer buf = { (char *)malloc(MANIFEST_MAX_BYTES + 1), 0 };
  if (buf.data == nullptr) {
    snprintf(error, error_len, "Out of memory");
    return false;
  }
  HttpsResult res = https_get(url.c_str(), MANIFEST_MAX_BYTES, collect_chunk, &buf);
  if (!res.ok) {
    snprintf(error, error_len, "Manifest: %s", res.error);
    free(buf.data);
    return false;
  }
  buf.data[buf.len] = '\0';

  cJSON *root = cJSON_Parse(buf.data);
  free(buf.data);
  if (root == nullptr) {
    snprintf(error, error_len, "Manifest is not valid JSON");
    return false;
  }

  bool ok = false;
  const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
  const cJSON *board = cJSON_GetObjectItemCaseSensitive(root, "board");
  const cJSON *firmware = cJSON_GetObjectItemCaseSensitive(root, "firmware");
  const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
  const cJSON *item = nullptr;

  if (!cJSON_IsNumber(schema) || schema->valueint != OTA_MANIFEST_SCHEMA) {
    snprintf(error, error_len, "Unsupported manifest schema");
    goto done;
  }
  if (!cJSON_IsString(board) || strcmp(board->valuestring, FIRMWARE_BOARD_ID) != 0) {
    snprintf(error, error_len, "Manifest is for another board");
    goto done;
  }

  if (cJSON_IsObject(firmware)) {
    if (!parse_item(firmware, "firmware", true, &out->firmware, error, error_len)) {
      goto done;
    }
    out->has_firmware = true;
    describe_module_changes(cJSON_GetObjectItemCaseSensitive(firmware, "modules"), out);
  }

  cJSON_ArrayForEach(item, data) {
    const cJSON *partition = cJSON_GetObjectItemCaseSensitive(item, "partition");
    if (!cJSON_IsString(partition) || module_find_by_partition(partition->valuestring) == nullptr) {
      snprintf(error, error_len, "Unknown data partition in manifest");
      goto done;
    }
    if (out->data_count >= OTA_MAX_DATA_ITEMS) {
      break;
    }
    if (!parse_item(item, partition->valuestring, false, &out->data[out->data_count], error, error_len)) {
      goto done;
    }
    out->data_count++;
  }
  ok = true;

done:
  cJSON_Delete(root);
  return ok;
}
