#pragma once

#include <Arduino.h>

// Receives each downloaded chunk; return false to abort the transfer.
typedef bool (*HttpsChunkCb)(const uint8_t *data, size_t len, void *ctx);

struct HttpsResult {
  bool ok;
  int status;              // HTTP status of the final response
  int64_t content_length;  // -1 when the server uses chunked encoding
  size_t received;
  char error[96];
};

// Streams an HTTPS GET. Only https:// URLs are accepted; the server certificate is
// verified against the ESP-IDF CA bundle. Follows up to 5 redirects.
// on_start (optional) is called once with the final Content-Length before the first chunk.
HttpsResult https_get(const char *url, size_t max_bytes, HttpsChunkCb on_chunk, void *ctx,
                      void (*on_start)(int64_t content_length, void *ctx) = nullptr);
