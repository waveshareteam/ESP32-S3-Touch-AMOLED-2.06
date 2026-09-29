// Arduino.h (via https_client.h) must precede lwip headers pulled in by esp_http_client.h
#include "https_client.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#define HTTPS_MAX_REDIRECTS 5
#define HTTPS_BUFFER_SIZE 4096
#define HTTPS_TIMEOUT_MS 15000

static void set_error(HttpsResult &res, const char *fmt, int value = 0) {
  snprintf(res.error, sizeof(res.error), fmt, value);
  res.ok = false;
}

HttpsResult https_get(const char *url, size_t max_bytes, HttpsChunkCb on_chunk, void *ctx,
                      void (*on_start)(int64_t content_length, void *ctx)) {
  HttpsResult res = {};
  res.content_length = -1;

  if (url == nullptr || strncmp(url, "https://", 8) != 0) {
    set_error(res, "Only https:// URLs are allowed");
    return res;
  }

  esp_http_client_config_t config = {};
  config.url = url;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.timeout_ms = HTTPS_TIMEOUT_MS;
  config.buffer_size = HTTPS_BUFFER_SIZE;
  config.keep_alive_enable = true;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    set_error(res, "HTTP client init failed");
    return res;
  }

  uint8_t *buf = (uint8_t *)malloc(HTTPS_BUFFER_SIZE);
  if (buf == nullptr) {
    esp_http_client_cleanup(client);
    set_error(res, "Out of memory");
    return res;
  }

  // Open, and follow redirects manually (streaming mode does not follow them itself)
  int redirects = 0;
  while (true) {
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      set_error(res, "Connect/TLS failed (0x%x)", err);
      goto done;
    }
    res.content_length = esp_http_client_fetch_headers(client);
    res.status = esp_http_client_get_status_code(client);
    if (res.status == 301 || res.status == 302 || res.status == 303 || res.status == 307 || res.status == 308) {
      if (++redirects > HTTPS_MAX_REDIRECTS) {
        set_error(res, "Too many redirects");
        goto done;
      }
      esp_http_client_set_redirection(client);
      esp_http_client_close(client);
      // A redirect must not downgrade to plain HTTP
      char next_url[512] = {};
      if (esp_http_client_get_url(client, next_url, sizeof(next_url)) != ESP_OK ||
          strncmp(next_url, "https://", 8) != 0) {
        set_error(res, "Redirect to non-HTTPS URL refused");
        goto done;
      }
      continue;
    }
    break;
  }

  if (res.status != 200) {
    set_error(res, "HTTP status %d", res.status);
    goto done;
  }
  if (res.content_length > 0 && (size_t)res.content_length > max_bytes) {
    set_error(res, "File too large (%d bytes)", (int)res.content_length);
    goto done;
  }
  if (on_start != nullptr) {
    on_start(res.content_length, ctx);
  }

  while (true) {
    int n = esp_http_client_read(client, (char *)buf, HTTPS_BUFFER_SIZE);
    if (n < 0) {
      set_error(res, "Read error (%d)", n);
      goto done;
    }
    if (n == 0) {
      if (esp_http_client_is_complete_data_received(client)) {
        break;
      }
      if (res.content_length < 0) {
        break;  // chunked: server closed after the last chunk
      }
      set_error(res, "Connection closed early");
      goto done;
    }
    if (res.received + n > max_bytes) {
      set_error(res, "Download exceeds %d bytes", (int)max_bytes);
      goto done;
    }
    res.received += n;
    if (!on_chunk(buf, n, ctx)) {
      set_error(res, "Aborted by receiver");
      goto done;
    }
  }

  if (res.content_length > 0 && res.received != (size_t)res.content_length) {
    set_error(res, "Size mismatch");
    goto done;
  }
  res.ok = true;

done:
  free(buf);
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return res;
}
