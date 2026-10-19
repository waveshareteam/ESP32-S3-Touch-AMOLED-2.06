/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "XiaozhiActivationClient.hpp"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_hmac.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_flash.h"
#include "esp_chip_info.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

namespace esp_brookesia::apps {

namespace {

constexpr char TAG[] = "XiaozhiActivation";
constexpr char LANGUAGE[] = "zh-CN";

struct HttpContext {
    char *response = nullptr;
    size_t response_length = 0;
    size_t response_capacity = 0;
    esp_err_t result = ESP_OK;
};

esp_err_t httpEventHandler(esp_http_client_event_t *event)
{
    auto *context = static_cast<HttpContext *>(event->user_data);
    if (!context) {
        return ESP_OK;
    }

    switch (event->event_id) {
    case HTTP_EVENT_ON_DATA: {
        if (context->result != ESP_OK || !event->data || event->data_len <= 0) {
            return context->result;
        }
        size_t data_length = static_cast<size_t>(event->data_len);
        if (context->response_length + data_length >=
                context->response_capacity) {
            context->result = ESP_ERR_INVALID_SIZE;
            return context->result;
        }
        memcpy(
            context->response + context->response_length,
            event->data,
            data_length
        );
        context->response_length += data_length;
        context->response[context->response_length] = '\0';
        return ESP_OK;
    }
    case HTTP_EVENT_REDIRECT:
        return esp_http_client_set_redirection(event->client);
    default:
        return ESP_OK;
    }
}

uint32_t responseCapacity()
{
    return CONFIG_XIAOZHI_INFO_MAX_RESPONSE_SIZE > 0 ?
           CONFIG_XIAOZHI_INFO_MAX_RESPONSE_SIZE : 8192;
}

} // namespace

XiaozhiActivationClient::XiaozhiActivationClient()
{
    _mutex = xSemaphoreCreateMutex();
}

XiaozhiActivationClient::~XiaozhiActivationClient()
{
    cancel();
    if (_mutex) {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
}

esp_err_t XiaozhiActivationClient::loadIdentity(Identity &identity) const
{
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open("board", NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t uuid_length = sizeof(identity.uuid);
    ret = nvs_get_str(handle, "uuid", identity.uuid, &uuid_length);
    if (ret == ESP_ERR_NVS_NOT_FOUND ||
            (ret == ESP_OK && identity.uuid[0] == '\0')) {
        uint8_t uuid[16] = {};
        esp_fill_random(uuid, sizeof(uuid));
        uuid[6] = (uuid[6] & 0x0f) | 0x40;
        uuid[8] = (uuid[8] & 0x3f) | 0x80;
        snprintf(
            identity.uuid,
            sizeof(identity.uuid),
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
            "%02x%02x%02x%02x%02x%02x",
            uuid[0], uuid[1], uuid[2], uuid[3],
            uuid[4], uuid[5], uuid[6], uuid[7],
            uuid[8], uuid[9], uuid[10], uuid[11],
            uuid[12], uuid[13], uuid[14], uuid[15]
        );
        ret = nvs_set_str(handle, "uuid", identity.uuid);
        if (ret == ESP_OK) {
            ret = nvs_commit(handle);
        }
    }
    nvs_close(handle);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t mac[6] = {};
    ret = esp_read_mac(mac, ESP_MAC_BASE);
    if (ret != ESP_OK) {
        return ret;
    }
    snprintf(
        identity.mac_address,
        sizeof(identity.mac_address),
        "%02x:%02x:%02x:%02x:%02x:%02x",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
    );

#ifdef ESP_EFUSE_BLOCK_USR_DATA
    uint8_t serial_number[33] = {};
    ret = esp_efuse_read_field_blob(
              ESP_EFUSE_USER_DATA, serial_number, 32 * 8
          );
    if (ret != ESP_OK) {
        return ret;
    }
    if (serial_number[0] != 0) {
        memcpy(identity.serial_number, serial_number, 32);
        identity.serial_number[32] = '\0';
        identity.factory = true;
    }
#endif
    return ESP_OK;
}

esp_err_t XiaozhiActivationClient::buildBoardJson(
    const Identity &identity,
    std::string &json
) const
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    uint32_t flash_size = 0;
    (void)esp_flash_get_size(nullptr, &flash_size);
    cJSON_AddNumberToObject(root, "version", 2);
    cJSON_AddStringToObject(root, "language", LANGUAGE);
    cJSON_AddNumberToObject(root, "flash_size", flash_size);
    cJSON_AddNumberToObject(
        root, "minimum_free_heap_size", esp_get_minimum_free_heap_size()
    );
    cJSON_AddStringToObject(root, "mac_address", identity.mac_address);
    cJSON_AddStringToObject(root, "uuid", identity.uuid);
    cJSON_AddStringToObject(root, "chip_model_name", CONFIG_IDF_TARGET);

    cJSON *chip = cJSON_CreateObject();
    if (chip) {
        esp_chip_info_t info = {};
        esp_chip_info(&info);
        cJSON_AddNumberToObject(chip, "model", info.model);
        cJSON_AddNumberToObject(chip, "cores", info.cores);
        cJSON_AddNumberToObject(chip, "revision", info.revision);
        cJSON_AddNumberToObject(chip, "features", info.features);
        cJSON_AddItemToObject(root, "chip_info", chip);
    }

    cJSON *application = cJSON_CreateObject();
    if (application) {
        const esp_app_desc_t *description = esp_app_get_description();
        char compile_time[64] = {};
        char sha256[65] = {};
        if (description) {
            snprintf(
                compile_time,
                sizeof(compile_time),
                "%sT%sZ",
                description->date,
                description->time
            );
            for (size_t i = 0; i < sizeof(description->app_elf_sha256); ++i) {
                snprintf(
                    sha256 + i * 2,
                    sizeof(sha256) - i * 2,
                    "%02x",
                    description->app_elf_sha256[i]
                );
            }
            cJSON_AddStringToObject(
                application, "name", description->project_name
            );
            cJSON_AddStringToObject(
                application, "version", description->version
            );
            cJSON_AddStringToObject(
                application, "compile_time", compile_time
            );
            cJSON_AddStringToObject(
                application, "idf_version", description->idf_ver
            );
            cJSON_AddStringToObject(application, "elf_sha256", sha256);
        }
        cJSON_AddItemToObject(root, "application", application);
    }

    cJSON *partitions = cJSON_CreateArray();
    if (partitions) {
        esp_partition_iterator_t iterator = esp_partition_find(
            ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr
        );
        while (iterator) {
            const esp_partition_t *partition = esp_partition_get(iterator);
            cJSON *item = cJSON_CreateObject();
            if (partition && item) {
                cJSON_AddStringToObject(item, "label", partition->label);
                cJSON_AddNumberToObject(item, "type", partition->type);
                cJSON_AddNumberToObject(item, "subtype", partition->subtype);
                cJSON_AddNumberToObject(item, "address", partition->address);
                cJSON_AddNumberToObject(item, "size", partition->size);
                cJSON_AddItemToArray(partitions, item);
            } else {
                cJSON_Delete(item);
            }
            iterator = esp_partition_next(iterator);
        }
        esp_partition_iterator_release(iterator);
        cJSON_AddItemToObject(root, "partition_table", partitions);
    }

    cJSON *ota = cJSON_CreateObject();
    if (ota) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        cJSON_AddStringToObject(
            ota, "label", running ? running->label : "Unknown"
        );
        cJSON_AddItemToObject(root, "ota", ota);
    }

    cJSON *board = cJSON_CreateObject();
    if (board) {
        cJSON_AddStringToObject(board, "type", "generic");
        cJSON_AddStringToObject(board, "name", CONFIG_IDF_TARGET);
        cJSON_AddStringToObject(board, "version", "1.0");
        cJSON_AddItemToObject(root, "board", board);
    }

    char *serialized = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!serialized) {
        return ESP_ERR_NO_MEM;
    }
    json.assign(serialized);
    cJSON_free(serialized);
    return ESP_OK;
}

esp_err_t XiaozhiActivationClient::performPost(
    const char *url,
    const Identity &identity,
    const char *body,
    std::string *response,
    int &http_status
)
{
    if (!url || !url[0] || !body || !_mutex) {
        return ESP_ERR_INVALID_ARG;
    }

    http_status = 0;
    const size_t capacity = responseCapacity();
    auto *response_buffer = static_cast<char *>(calloc(1, capacity + 1));
    if (!response_buffer) {
        return ESP_ERR_NO_MEM;
    }
    HttpContext context = {};
    context.response = response_buffer;
    context.response_capacity = capacity + 1;

    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = httpEventHandler;
    config.user_data = &context;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = CONFIG_XIAOZHI_INFO_TIMEOUT_MS;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(response_buffer);
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(_mutex, portMAX_DELAY) != pdTRUE) {
        esp_http_client_cleanup(client);
        free(response_buffer);
        return ESP_ERR_INVALID_STATE;
    }
    if (_active_client) {
        xSemaphoreGive(_mutex);
        esp_http_client_cleanup(client);
        free(response_buffer);
        return ESP_ERR_INVALID_STATE;
    }
    _active_client = client;
    xSemaphoreGive(_mutex);

    esp_err_t ret = ESP_OK;
    char activation_version[2] = {
        static_cast<char>(identity.factory ? '2' : '1'), '\0'
    };
    char user_agent[96] = {};
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(
        user_agent,
        sizeof(user_agent),
        "%s/%s",
        CONFIG_IDF_TARGET,
        app ? app->version : "unknown"
    );

    do {
        ret = esp_http_client_set_header(
                  client, "Activation-Version", activation_version
              );
        if (ret != ESP_OK) {
            break;
        }
        ret = esp_http_client_set_header(
                  client, "Device-Id", identity.mac_address
              );
        if (ret != ESP_OK) {
            break;
        }
        ret = esp_http_client_set_header(client, "Client-Id", identity.uuid);
        if (ret != ESP_OK) {
            break;
        }
        if (identity.factory) {
            ret = esp_http_client_set_header(
                      client, "Serial-Number", identity.serial_number
                  );
            if (ret != ESP_OK) {
                break;
            }
        }
        ret = esp_http_client_set_header(client, "User-Agent", user_agent);
        if (ret != ESP_OK) {
            break;
        }
        ret = esp_http_client_set_header(client, "Accept-Language", LANGUAGE);
        if (ret != ESP_OK) {
            break;
        }
        ret = esp_http_client_set_header(
                  client, "Content-Type", "application/json"
              );
        if (ret != ESP_OK) {
            break;
        }
        ret = esp_http_client_set_post_field(
                  client, body, static_cast<int>(strlen(body))
              );
        if (ret != ESP_OK) {
            break;
        }

        do {
            ret = esp_http_client_perform(client);
        } while (ret == ESP_ERR_HTTP_EAGAIN);
        if (ret != ESP_OK) {
            break;
        }

        http_status = esp_http_client_get_status_code(client);
        if (context.result != ESP_OK) {
            ret = context.result;
            break;
        }
        if (response) {
            response->assign(
                context.response ? context.response : "",
                context.response_length
            );
        }
    } while (false);

    if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        if (_active_client == client) {
            _active_client = nullptr;
        }
        xSemaphoreGive(_mutex);
    }
    esp_http_client_cleanup(client);
    free(response_buffer);
    return ret;
}

esp_err_t XiaozhiActivationClient::fetchInfo(Info &out)
{
    out = {};
    Identity identity = {};
    esp_err_t ret = loadIdentity(identity);
    if (ret != ESP_OK) {
        return ret;
    }

    out.device_id = identity.mac_address;
    out.client_id = identity.uuid;

    std::string body;
    ret = buildBoardJson(identity, body);
    if (ret != ESP_OK) {
        return ret;
    }

    std::string response;
    ret = performPost(
              CONFIG_XIAOZHI_OTA_URL,
              identity,
              body.c_str(),
              &response,
              out.http_status
          );
    if (ret != ESP_OK) {
        return ret;
    }
    if (out.http_status != 200 || response.empty()) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *root = cJSON_ParseWithLength(response.data(), response.size());
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    out.factory_identity = identity.factory;
    cJSON *activation = cJSON_GetObjectItem(root, "activation");
    if (cJSON_IsObject(activation)) {
        cJSON *message = cJSON_GetObjectItem(activation, "message");
        cJSON *code = cJSON_GetObjectItem(activation, "code");
        cJSON *challenge = cJSON_GetObjectItem(activation, "challenge");
        cJSON *timeout_ms = cJSON_GetObjectItem(activation, "timeout_ms");
        if (cJSON_IsString(message)) {
            out.message = message->valuestring;
        }
        if (cJSON_IsString(code)) {
            out.code = code->valuestring;
        }
        if (cJSON_IsString(challenge)) {
            out.challenge = challenge->valuestring;
        }
        if (cJSON_IsNumber(timeout_ms)) {
            out.timeout_ms = timeout_ms->valueint;
        }
        if (!out.code.empty() || !out.challenge.empty()) {
            out.state = BindingState::ActivationRequired;
        }
    }

    cJSON *websocket = cJSON_GetObjectItem(root, "websocket");
    if (cJSON_IsObject(websocket)) {
        cJSON *url = cJSON_GetObjectItem(websocket, "url");
        cJSON *token = cJSON_GetObjectItem(websocket, "token");
        cJSON *version = cJSON_GetObjectItem(websocket, "version");
        if (cJSON_IsString(url) && url->valuestring[0] != '\0') {
            out.websocket_url = url->valuestring;
            out.has_websocket_config = true;
        }
        if (cJSON_IsString(token)) {
            out.websocket_token = token->valuestring;
        }
        if (cJSON_IsNumber(version) && version->valueint > 0) {
            out.websocket_version = version->valueint;
        } else if (cJSON_IsString(version)) {
            int parsed_version = atoi(version->valuestring);
            if (parsed_version > 0) {
                out.websocket_version = parsed_version;
            }
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t XiaozhiActivationClient::activate(
    const Info &info,
    int &http_status
)
{
    if (info.state != BindingState::ActivationRequired ||
            info.challenge.empty()) {
        return ESP_ERR_INVALID_STATE;
    }

    Identity identity = {};
    esp_err_t ret = loadIdentity(identity);
    if (ret != ESP_OK) {
        return ret;
    }

    std::string body = "{}";
    if (identity.factory) {
#if SOC_HMAC_SUPPORTED
        if (esp_efuse_get_key_purpose(EFUSE_BLK_KEY0) !=
                ESP_EFUSE_KEY_PURPOSE_HMAC_UP) {
            ESP_LOGE(TAG, "KEY0 is not configured for HMAC upstream");
            return ESP_ERR_INVALID_STATE;
        }

        uint8_t hmac[32] = {};
        ret = esp_hmac_calculate(
                  HMAC_KEY0,
                  info.challenge.data(),
                  info.challenge.size(),
                  hmac
              );
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to calculate activation HMAC");
            return ret;
        }

        char hmac_hex[65] = {};
        for (size_t i = 0; i < sizeof(hmac); ++i) {
            snprintf(hmac_hex + i * 2, 3, "%02x", hmac[i]);
        }

        cJSON *payload = cJSON_CreateObject();
        if (!payload ||
                !cJSON_AddStringToObject(
                    payload, "algorithm", "hmac-sha256"
                ) ||
                !cJSON_AddStringToObject(
                    payload, "serial_number", identity.serial_number
                ) ||
                !cJSON_AddStringToObject(
                    payload, "challenge", info.challenge.c_str()
                ) ||
                !cJSON_AddStringToObject(payload, "hmac", hmac_hex)) {
            cJSON_Delete(payload);
            return ESP_ERR_NO_MEM;
        }
        char *json = cJSON_PrintUnformatted(payload);
        cJSON_Delete(payload);
        if (!json) {
            return ESP_ERR_NO_MEM;
        }
        body.assign(json);
        cJSON_free(json);
#else
        ESP_LOGE(TAG, "Factory activation requires HMAC support");
        return ESP_ERR_NOT_SUPPORTED;
#endif
    }

    std::string url = CONFIG_XIAOZHI_OTA_URL;
    if (!url.empty() && url.back() != '/') {
        url.push_back('/');
    }
    url.append("activate");

    ret = performPost(
              url.c_str(), identity, body.c_str(), nullptr, http_status
          );
    if (ret != ESP_OK) {
        return ret;
    }
    if (http_status == 200) {
        return ESP_OK;
    }
    if (http_status == 202) {
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGE(TAG, "Activation request failed with HTTP status %d", http_status);
    return ESP_FAIL;
}

void XiaozhiActivationClient::cancel()
{
    if (!_mutex || xSemaphoreTake(_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (_active_client) {
        esp_http_client_cancel_request(_active_client);
    }
    xSemaphoreGive(_mutex);
}

} // namespace esp_brookesia::apps
