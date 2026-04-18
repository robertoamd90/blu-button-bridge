#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <ctype.h>
#include "cJSON.h"
#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "board_config.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "gpio_manager.h"
#include "web_manager.h"
#include "ota_manager.h"
#include "ota_release_profile.h"
#include "ble_access.h"
#include "console_manager.h"
#include "auth_manager.h"

static const char *TAG = "web_manager";
#define CONFIG_BACKUP_VERSION 3
#define OTA_MANIFEST_HTTP_BUFFER_SIZE    2048
#define OTA_MANIFEST_HTTP_TX_BUFFER_SIZE 512
#define OTA_MANIFEST_HTTP_MAX_REDIRECTS  5
#define HTTP_BUFFER_INITIAL_CAPACITY     2048
static const char *OTA_MANIFEST_URL = "https://robertoamd90.github.io/blu-button-bridge/ota-manifest.json";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t console_html_start[] asm("_binary_console_html_start");
extern const uint8_t console_html_end[]   asm("_binary_console_html_end");
extern const uint8_t wifi_handoff_html_start[] asm("_binary_wifi_handoff_html_start");
extern const uint8_t wifi_handoff_html_end[]   asm("_binary_wifi_handoff_html_end");

typedef esp_err_t (*route_handler_t)(httpd_req_t *req);
typedef struct {
    route_handler_t inner;
    bool            auth_required;
} route_ctx_t;
typedef struct {
    const char     *uri;
    httpd_method_t  method;
    route_handler_t handler;
    bool            auth_required;
} route_def_t;

static SemaphoreHandle_t   s_ota_mutex = NULL;
static portMUX_TYPE        s_console_stream_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t            s_console_stream_generation = 0;

// ── HTTP helpers ─────────────────────────────────────────────────────────────

static esp_err_t handle_with_auth(httpd_req_t *req)
{
    route_ctx_t *ctx = (route_ctx_t *)req->user_ctx;
    if (!ctx || !ctx->inner) return ESP_FAIL;
    if (ctx->auth_required && !auth_manager_require(req)) return ESP_OK;
    return ctx->inner(req);
}

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    if (req->content_len == 0) { buf[0] = '\0'; return ESP_OK; }
    if (req->content_len >= buf_len) return ESP_FAIL;
    int received = 0, total = (int)req->content_len;
    while (received < total) {
        int ret = httpd_req_recv(req, buf + received, total - received);
        if (ret <= 0) return ESP_FAIL;
        received += ret;
    }
    buf[received] = '\0';
    return ESP_OK;
}

static void send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
}

// Serialise, send, and free a cJSON object.
static void send_cjson(httpd_req_t *req, cJSON *obj)
{
    char *str = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!str) { send_json(req, "{\"ok\":false,\"error\":\"json error\"}"); return; }
    send_json(req, str);
    cJSON_free(str);
}

static esp_err_t send_error(httpd_req_t *req, const char *msg)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", false);
    cJSON_AddStringToObject(obj, "error", msg);
    httpd_resp_set_status(req, "400 Bad Request");
    send_cjson(req, obj);
    return ESP_OK;
}

static esp_err_t send_error_status(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", false);
    cJSON_AddStringToObject(obj, "error", msg);
    httpd_resp_set_status(req, status);
    send_cjson(req, obj);
    return ESP_OK;
}

typedef struct {
    int major;
    int minor;
    int patch;
    bool prerelease;
    bool valid;
} semver_t;

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool alloc_failed;
} http_buffer_t;

typedef struct {
    char tag[32];
    char version_label[40];
    char html_url[256];
    char asset_name[80];
    char download_url[512];
    char digest_hex[65];
    int asset_size;
} ota_release_info_t;

static ota_release_info_t s_last_ota_release = {0};

typedef struct {
    httpd_req_t *req;
    uint32_t generation;
} console_stream_ctx_t;

static void format_version_label(const char *version, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    if (!version || version[0] == '\0') {
        strlcpy(out, "unknown", out_len);
        return;
    }
    if (version[0] == 'v' || version[0] == 'V' || !isdigit((unsigned char)version[0])) {
        strlcpy(out, version, out_len);
        return;
    }
    snprintf(out, out_len, "v%s", version);
}

static bool parse_semver(const char *value, semver_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!value) return false;

    const char *p = value;
    if (*p == 'v' || *p == 'V') p++;

    char *end = NULL;
    long major = strtol(p, &end, 10);
    if (!end || end == p || *end != '.') return false;
    p = end + 1;

    long minor = strtol(p, &end, 10);
    if (!end || end == p || *end != '.') return false;
    p = end + 1;

    long patch = strtol(p, &end, 10);
    if (!end || end == p) return false;

    if (*end != '\0' && *end != '-' && *end != '+') return false;

    out->major = (int)major;
    out->minor = (int)minor;
    out->patch = (int)patch;
    out->prerelease = (*end == '-');
    out->valid = true;
    return true;
}

static int compare_versions_for_update(const char *candidate, const char *current)
{
    semver_t candidate_v = {0}, current_v = {0};
    bool candidate_ok = parse_semver(candidate, &candidate_v);
    bool current_ok = parse_semver(current, &current_v);

    if (!candidate_ok) return 0;
    if (!current_ok) return 1;

    if (candidate_v.major != current_v.major) return (candidate_v.major > current_v.major) ? 1 : -1;
    if (candidate_v.minor != current_v.minor) return (candidate_v.minor > current_v.minor) ? 1 : -1;
    if (candidate_v.patch != current_v.patch) return (candidate_v.patch > current_v.patch) ? 1 : -1;
    if (candidate_v.prerelease != current_v.prerelease) return candidate_v.prerelease ? -1 : 1;
    return 0;
}

static bool parse_release_digest(const char *digest, char *out_hex, size_t out_len)
{
    if (!digest || !out_hex || out_len < 65) return false;
    const char *prefix = "sha256:";
    size_t prefix_len = strlen(prefix);
    if (strncasecmp(digest, prefix, prefix_len) != 0) return false;

    const char *hex = digest + prefix_len;
    if (strlen(hex) != 64) return false;
    for (size_t i = 0; i < 64; i++) {
        if (!isxdigit((unsigned char)hex[i])) return false;
        out_hex[i] = (char)tolower((unsigned char)hex[i]);
    }
    out_hex[64] = '\0';
    return true;
}

static esp_err_t http_buffer_event_handler(esp_http_client_event_t *evt)
{
    http_buffer_t *buffer = (http_buffer_t *)evt->user_data;
    if (!buffer) return ESP_OK;

    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        size_t needed = buffer->len + (size_t)evt->data_len + 1;
        if (needed > buffer->cap) {
            size_t new_cap = (buffer->cap > 0) ? buffer->cap : HTTP_BUFFER_INITIAL_CAPACITY;
            while (new_cap < needed) new_cap *= 2;
            char *new_buf = realloc(buffer->buf, new_cap);
            if (!new_buf) {
                buffer->alloc_failed = true;
                return ESP_ERR_NO_MEM;
            }
            buffer->buf = new_buf;
            buffer->cap = new_cap;
        }
        memcpy(buffer->buf + buffer->len, evt->data, evt->data_len);
        buffer->len += (size_t)evt->data_len;
        buffer->buf[buffer->len] = '\0';
    }

    return ESP_OK;
}

static esp_err_t ota_manifest_http_get_json(const char *url, http_buffer_t *buffer)
{
    if (!url || !buffer) return ESP_ERR_INVALID_ARG;

    free(buffer->buf);
    buffer->buf = NULL;
    buffer->len = 0;
    buffer->cap = 0;
    buffer->alloc_failed = false;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .buffer_size = OTA_MANIFEST_HTTP_BUFFER_SIZE,
        .buffer_size_tx = OTA_MANIFEST_HTTP_TX_BUFFER_SIZE,
        .max_redirection_count = OTA_MANIFEST_HTTP_MAX_REDIRECTS,
        .event_handler = http_buffer_event_handler,
        .user_data = buffer,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "BluButtonBridge");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) return err;
    if (status != 200) return ESP_FAIL;
    if (buffer->alloc_failed) return ESP_ERR_NO_MEM;
    if (!buffer->buf || buffer->len == 0) return ESP_FAIL;
    return ESP_OK;
}

static esp_err_t ota_manifest_fetch_latest_release(ota_release_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));
    const char *board_id = ota_release_profile_board_id();
    const char *expected_asset_name = ota_release_profile_ota_asset_name();
    if (!board_id || board_id[0] == '\0' || !expected_asset_name || expected_asset_name[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    http_buffer_t buffer = {0};

    esp_err_t err = ota_manifest_http_get_json(OTA_MANIFEST_URL, &buffer);
    if (err != ESP_OK) {
        free(buffer.buf);
        return err;
    }

    cJSON *root = cJSON_Parse(buffer.buf);
    free(buffer.buf);
    if (!root) return ESP_FAIL;

    cJSON *tag = cJSON_GetObjectItem(root, "tag");
    cJSON *html_url = cJSON_GetObjectItem(root, "html_url");
    cJSON *boards = cJSON_GetObjectItem(root, "boards");
    if (!cJSON_IsString(tag) || !cJSON_IsObject(boards)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    strlcpy(info->tag, tag->valuestring, sizeof(info->tag));
    format_version_label(tag->valuestring, info->version_label, sizeof(info->version_label));
    if (cJSON_IsString(html_url)) strlcpy(info->html_url, html_url->valuestring, sizeof(info->html_url));

    cJSON *board = cJSON_GetObjectItemCaseSensitive(boards, board_id);
    if (!cJSON_IsObject(board)) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *name = cJSON_GetObjectItem(board, "asset_name");
    cJSON *download_url = cJSON_GetObjectItem(board, "browser_download_url");
    cJSON *digest = cJSON_GetObjectItem(board, "asset_sha256");
    cJSON *size = cJSON_GetObjectItem(board, "asset_size");
    if (!cJSON_IsString(name) || strcmp(name->valuestring, expected_asset_name) != 0 ||
        !cJSON_IsString(download_url) || !cJSON_IsString(digest) || !cJSON_IsNumber(size) ||
        !parse_release_digest(digest->valuestring, info->digest_hex, sizeof(info->digest_hex))) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    strlcpy(info->asset_name, name->valuestring, sizeof(info->asset_name));
    strlcpy(info->download_url, download_url->valuestring, sizeof(info->download_url));
    info->asset_size = (int)size->valuedouble;

    cJSON_Delete(root);
    return ESP_OK;
}
// ── Async background tasks (WiFi/MQTT connect without blocking the HTTP path) ─

typedef struct { char ssid[33]; char pass[65]; bool has_pass; bool handoff; } wifi_creds_t;
typedef struct { char host[128]; uint32_t port; char user[64]; char pass[64]; bool tls; bool has_pass; } mqtt_creds_t;

static void wifi_connect_task(void *arg)
{
    wifi_creds_t *c = (wifi_creds_t *)arg;
    if (c->handoff) {
        (void)wifi_arm_ap_handoff();
    }
    wifi_connect_api(c->ssid, c->pass, c->has_pass);
    free(c);
    vTaskDelete(NULL);
}

static void mqtt_connect_task(void *arg)
{
    mqtt_creds_t *c = (mqtt_creds_t *)arg;
    mqtt_connect_api(c->host, c->port, c->user, c->pass, c->tls, c->has_pass);
    free(c);
    vTaskDelete(NULL);
}

// ── Handlers ─────────────────────────────────────────────────────────────────

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start,
                    index_html_end - index_html_start);
    return ESP_OK;
}

static esp_err_t handle_wifi_handoff_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, (const char *)wifi_handoff_html_start,
                    wifi_handoff_html_end - wifi_handoff_html_start);
    return ESP_OK;
}

static esp_err_t handle_console_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)console_html_start,
                    console_html_end - console_html_start);
    return ESP_OK;
}

static esp_err_t send_sse_event(httpd_req_t *req, const char *event_name, const char *data)
{
    if (event_name && httpd_resp_send_chunk(req, "event: ", 7) != ESP_OK) return ESP_FAIL;
    if (event_name && httpd_resp_send_chunk(req, event_name, HTTPD_RESP_USE_STRLEN) != ESP_OK) return ESP_FAIL;
    if (event_name && httpd_resp_send_chunk(req, "\n", 1) != ESP_OK) return ESP_FAIL;

    const char *cursor = data ? data : "";
    while (true) {
        const char *line_end = strchr(cursor, '\n');
        if (httpd_resp_send_chunk(req, "data: ", 6) != ESP_OK) return ESP_FAIL;
        if (line_end) {
            if (line_end > cursor &&
                httpd_resp_send_chunk(req, cursor, line_end - cursor) != ESP_OK) {
                return ESP_FAIL;
            }
            if (httpd_resp_send_chunk(req, "\n", 1) != ESP_OK) return ESP_FAIL;
            cursor = line_end + 1;
            continue;
        }
        if (*cursor != '\0' &&
            httpd_resp_send_chunk(req, cursor, HTTPD_RESP_USE_STRLEN) != ESP_OK) {
            return ESP_FAIL;
        }
        if (httpd_resp_send_chunk(req, "\n\n", 2) != ESP_OK) return ESP_FAIL;
        return ESP_OK;
    }
}

static bool console_stream_is_owner(uint32_t generation)
{
    bool is_owner = false;
    portENTER_CRITICAL(&s_console_stream_lock);
    is_owner = (generation == s_console_stream_generation);
    portEXIT_CRITICAL(&s_console_stream_lock);
    return is_owner;
}

static void console_stream_close_all(void)
{
    portENTER_CRITICAL(&s_console_stream_lock);
    ++s_console_stream_generation;
    portEXIT_CRITICAL(&s_console_stream_lock);
}

static void console_stream_task(void *arg)
{
    console_stream_ctx_t *ctx = (console_stream_ctx_t *)arg;
    if (!ctx || !ctx->req) {
        free(ctx);
        vTaskDelete(NULL);
        return;
    }

    httpd_req_t *req = ctx->req;
    uint32_t generation = ctx->generation;

    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_set_hdr(req, "X-Accel-Buffering", "no");

    uint32_t cursor = 0;
    TickType_t last_heartbeat = xTaskGetTickCount();
    console_line_t lines[8];

    while (console_stream_is_owner(generation)) {
        bool dropped = false;
        size_t count = console_manager_get_since(&cursor, lines, 8, &dropped);
        if (dropped && send_sse_event(req, "notice", "Some older log lines were dropped from the in-memory backlog.") != ESP_OK) {
            break;
        }
        for (size_t i = 0; i < count; i++) {
            if (send_sse_event(req, "log", lines[i].text) != ESP_OK) {
                goto done;
            }
            if (!console_stream_is_owner(generation)) {
                goto done;
            }
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - last_heartbeat) >= pdMS_TO_TICKS(5000)) {
            if (httpd_resp_send_chunk(req, ": keep-alive\n\n", 14) != ESP_OK) {
                break;
            }
            last_heartbeat = now;
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (!console_stream_is_owner(generation)) {
        send_sse_event(req, "replaced", "This console was replaced by a newer viewer.");
    }

done:
    httpd_resp_send_chunk(req, NULL, 0);
    if (httpd_req_async_handler_complete(req) != ESP_OK) {
        ESP_LOGW(TAG, "console async request cleanup failed");
    }
    free(ctx);
    vTaskDelete(NULL);
}

static esp_err_t handle_console_stream(httpd_req_t *req)
{
    httpd_req_t *async_req = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &async_req);
    if (err != ESP_OK || !async_req) {
        return send_error_status(req, "503 Service Unavailable", "could not start console stream");
    }

    console_stream_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        httpd_req_async_handler_complete(async_req);
        return send_error_status(req, "503 Service Unavailable", "could not start console stream");
    }

    portENTER_CRITICAL(&s_console_stream_lock);
    ctx->generation = ++s_console_stream_generation;
    portEXIT_CRITICAL(&s_console_stream_lock);
    ctx->req = async_req;

    if (xTaskCreate(console_stream_task, "console_sse", 4096, ctx, 5, NULL) != pdPASS) {
        free(ctx);
        httpd_req_async_handler_complete(async_req);
        return send_error_status(req, "503 Service Unavailable", "could not start console stream");
    }

    return ESP_OK;
}

static esp_err_t handle_status(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char sta_ip[16] = {};
    cJSON *obj = cJSON_CreateObject();
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    cJSON_AddStringToObject(obj, "wifi",        wifi_status_str(wifi_get_status()));
    cJSON_AddBoolToObject  (obj, "wifi_error_latched", wifi_get_error_latched());
    cJSON_AddStringToObject(obj, "mqtt",        mqtt_status_str(mqtt_get_status()));
    cJSON_AddStringToObject(obj, "ap",          wifi_ap_is_active() ? "up" : "down");
    cJSON_AddStringToObject(obj, "ble",         ble_status_str(ble_get_status()));
    if (wifi_get_sta_ip(sta_ip, sizeof(sta_ip))) {
        cJSON_AddStringToObject(obj, "sta_ip", sta_ip);
    }
    cJSON_AddStringToObject(obj, "fw_version",  app->version);
    send_cjson(req, obj);
    return ESP_OK;
}

// POST /api/ap/start
static esp_err_t handle_ap_start(httpd_req_t *req)
{
    wifi_start_ap();
    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

// POST /api/ap/stop
static esp_err_t handle_ap_stop(httpd_req_t *req)
{
    wifi_stop_ap();
    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t handle_ap_handoff_complete(httpd_req_t *req)
{
    wifi_complete_ap_handoff();
    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

// GET /api/ap/config
static esp_err_t handle_ap_config_get(httpd_req_t *req)
{
    cJSON *obj = wifi_ap_config_export();
    if (!obj) return send_error(req, "could not read AP config");
    send_cjson(req, obj);
    return ESP_OK;
}

// POST /api/ap/config  {"enabled":true,"ssid":"BBB-XXYYZZ","password":"12345678"}
static esp_err_t handle_ap_config_set(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof(body)) != ESP_OK)
        return send_error(req, "body too large");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, "invalid json");

    wifi_ap_settings_t cfg;
    wifi_ap_load_config(&cfg); // start with current values as base

    cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass_item = cJSON_GetObjectItem(root, "password");
    cJSON *en_item   = cJSON_GetObjectItem(root, "enabled");

    if (cJSON_IsString(ssid_item) && strlen(ssid_item->valuestring) > 0)
        strlcpy(cfg.ssid, ssid_item->valuestring, sizeof(cfg.ssid));
    if (cJSON_IsString(pass_item))
        strlcpy(cfg.password, pass_item->valuestring, sizeof(cfg.password));
    if (cJSON_IsBool(en_item))
        cfg.enabled = cJSON_IsTrue(en_item);

    cJSON_Delete(root);

    bool was_active = wifi_ap_is_active();
    esp_err_t err = wifi_ap_save_config(&cfg);
    if (err != ESP_OK) return send_error(req, "could not save AP config");

    if (cfg.enabled)
        wifi_start_ap(); // start or restart with new config
    else if (was_active)
        wifi_stop_ap();  // "always on" disabled — hand back to auto-management

    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

// Catch-all: redirect to captive portal page (handles OS connectivity checks)
static esp_err_t handle_captive(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}

// POST /api/wifi/connect  — responds immediately, connects in background
static esp_err_t handle_wifi_connect(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof(body)) != ESP_OK)
        return send_error(req, "body too large");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, "invalid json");

    cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass_item = cJSON_GetObjectItem(root, "password");

    if (!cJSON_IsString(ssid_item) || strlen(ssid_item->valuestring) == 0) {
        cJSON_Delete(root);
        return send_error(req, "ssid required");
    }

    wifi_creds_t *c = malloc(sizeof(wifi_creds_t));
    if (!c) { cJSON_Delete(root); return send_error(req, "out of memory"); }
    strlcpy(c->ssid, ssid_item->valuestring, sizeof(c->ssid));
    strlcpy(c->pass, cJSON_IsString(pass_item) ? pass_item->valuestring : "", sizeof(c->pass));
    c->has_pass = cJSON_IsString(pass_item);
    c->handoff = wifi_should_offer_ap_handoff();
    const bool handoff = c->handoff;
    cJSON_Delete(root);

    if (xTaskCreate(wifi_connect_task, "wifi_conn", 4096, c, 5, NULL) != pdPASS) {
        free(c);
        return send_error(req, "could not start task");
    }

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", true);
    if (handoff) {
        cJSON_AddBoolToObject(obj, "handoff", true);
        cJSON_AddStringToObject(obj, "redirect", "/wifi-handoff");
    }
    send_cjson(req, obj);
    return ESP_OK;
}

// DELETE /api/wifi
static esp_err_t handle_wifi_delete(httpd_req_t *req)
{
    wifi_clean_credentials();
    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

// POST /api/mqtt/connect  — responds immediately, connects in background
static esp_err_t handle_mqtt_connect(httpd_req_t *req)
{
    char body[512];
    if (read_body(req, body, sizeof(body)) != ESP_OK)
        return send_error(req, "body too large");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, "invalid json");

    cJSON *host_item = cJSON_GetObjectItem(root, "host");
    cJSON *port_item = cJSON_GetObjectItem(root, "port");
    cJSON *user_item = cJSON_GetObjectItem(root, "username");
    cJSON *pass_item = cJSON_GetObjectItem(root, "password");
    cJSON *tls_item  = cJSON_GetObjectItem(root, "tls");

    if (!cJSON_IsString(host_item) || strlen(host_item->valuestring) == 0) {
        cJSON_Delete(root);
        return send_error(req, "host required");
    }

    mqtt_creds_t *c = malloc(sizeof(mqtt_creds_t));
    if (!c) { cJSON_Delete(root); return send_error(req, "out of memory"); }
    strlcpy(c->host, host_item->valuestring, sizeof(c->host));
    strlcpy(c->user, cJSON_IsString(user_item) ? user_item->valuestring : "", sizeof(c->user));
    strlcpy(c->pass, cJSON_IsString(pass_item) ? pass_item->valuestring : "", sizeof(c->pass));
    c->port = cJSON_IsNumber(port_item) ? (uint32_t)port_item->valuedouble : 1883;
    c->tls  = cJSON_IsTrue(tls_item);
    c->has_pass = cJSON_IsString(pass_item);
    cJSON_Delete(root);

    if (xTaskCreate(mqtt_connect_task, "mqtt_conn", 4096, c, 5, NULL) != pdPASS) {
        free(c);
        return send_error(req, "could not start task");
    }
    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

// DELETE /api/mqtt
static esp_err_t handle_mqtt_delete(httpd_req_t *req)
{
    mqtt_clean_credentials();
    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

// GET /api/wifi/config
static esp_err_t handle_wifi_config_get(httpd_req_t *req)
{
    cJSON *obj = wifi_config_export(WIFI_EXPORT_VIEW_FE);
    if (!obj) return send_error(req, "could not read WiFi config");
    send_cjson(req, obj);
    return ESP_OK;
}

// GET /api/wifi/scan
static esp_err_t handle_wifi_scan(httpd_req_t *req)
{
    cJSON *arr = NULL;
    esp_err_t err = wifi_scan_export(&arr);
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error_status(req, "409 Conflict",
                                 "scan unavailable while WiFi is connecting");
    }
    if (err != ESP_OK) {
        return send_error(req, "scan failed");
    }
    send_cjson(req, arr);
    return ESP_OK;
}

// GET /api/mqtt/config
static esp_err_t handle_mqtt_config_get(httpd_req_t *req)
{
    cJSON *obj = mqtt_config_export(MQTT_EXPORT_VIEW_FE);
    if (!obj) return send_error(req, "could not read MQTT config");
    send_cjson(req, obj);
    return ESP_OK;
}

// ── System handlers ───────────────────────────────────────────────────────────

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(300)); // let HTTP response reach the client
    esp_restart();
}

static void factory_reset_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_err_t err = nvs_flash_erase(); // wipes all NVS: WiFi, MQTT, AP config, BLE
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "factory reset failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
    }
    esp_restart();
}

static esp_err_t start_background_task(TaskFunction_t task_fn, const char *name)
{
    if (xTaskCreate(task_fn, name, 2048, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start task '%s'", name);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t send_ok_and_start_task(httpd_req_t *req,
                                        TaskFunction_t task_fn,
                                        const char *name,
                                        const char *error)
{
    esp_err_t err = start_background_task(task_fn, name);
    if (err != ESP_OK) return send_error(req, error);
    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

// POST /api/system/reboot
static esp_err_t handle_system_reboot(httpd_req_t *req)
{
    return send_ok_and_start_task(req, reboot_task, "reboot", "could not schedule reboot");
}

// POST /api/system/factory-reset
static esp_err_t handle_system_factory_reset(httpd_req_t *req)
{
    return send_ok_and_start_task(req, factory_reset_task, "factory_rst",
                                  "could not schedule factory reset");
}

// GET /api/system/auth
static esp_err_t handle_auth_config_get(httpd_req_t *req)
{
    cJSON *obj = auth_manager_config_export();
    if (!obj) return send_error(req, "could not read auth config");
    send_cjson(req, obj);
    return ESP_OK;
}

// POST /api/system/auth
static esp_err_t handle_auth_config_set(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof(body)) != ESP_OK)
        return send_error(req, "body too large");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, "invalid json");

    const char *error = NULL;
    esp_err_t err = auth_manager_config_update_from_json(root, &error);
    cJSON_Delete(root);
    if (err != ESP_OK) return send_error(req, error ? error : "could not save auth config");

    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

// ── BLE access handlers ───────────────────────────────────────────────────────

// ── MQTT action handlers ──────────────────────────────────────────────────────

// GET /api/mqtt/actions
static esp_err_t handle_mqtt_actions_get(httpd_req_t *req)
{
    cJSON *arr = mqtt_actions_export();
    if (!arr) return send_error(req, "could not read MQTT actions");
    send_cjson(req, arr);
    return ESP_OK;
}

// POST /api/mqtt/actions  {"name":"...","topic":"...","payload":"..."}
static esp_err_t handle_mqtt_action_add(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof(body)) != ESP_OK)
        return send_error(req, "body too large");
    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, "invalid json");

    cJSON *n = cJSON_GetObjectItem(root, "name");
    cJSON *t = cJSON_GetObjectItem(root, "topic");
    cJSON *p = cJSON_GetObjectItem(root, "payload");
    if (!cJSON_IsString(n) || strlen(n->valuestring) == 0) {
        cJSON_Delete(root);
        return send_error(req, "name required");
    }

    mqtt_action_t a = {0};
    strlcpy(a.name,    n->valuestring, sizeof(a.name));
    if (cJSON_IsString(t)) strlcpy(a.topic,   t->valuestring, sizeof(a.topic));
    if (cJSON_IsString(p)) strlcpy(a.payload, p->valuestring, sizeof(a.payload));
    cJSON_Delete(root);

    int idx = mqtt_action_add(&a);
    if (idx < 0) return send_error(req, "action list full");

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject  (obj, "ok",  true);
    cJSON_AddNumberToObject(obj, "idx", idx);
    send_cjson(req, obj);
    return ESP_OK;
}

// PUT /api/mqtt/action  {"idx":N,"name":"...","topic":"...","payload":"..."}
static esp_err_t handle_mqtt_action_update(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof(body)) != ESP_OK)
        return send_error(req, "body too large");
    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, "invalid json");

    cJSON *idx_item = cJSON_GetObjectItem(root, "idx");
    cJSON *n = cJSON_GetObjectItem(root, "name");
    cJSON *t = cJSON_GetObjectItem(root, "topic");
    cJSON *p = cJSON_GetObjectItem(root, "payload");
    if (!cJSON_IsNumber(idx_item) || !cJSON_IsString(n) || strlen(n->valuestring) == 0) {
        cJSON_Delete(root);
        return send_error(req, "idx and name required");
    }

    mqtt_action_t a = {0};
    strlcpy(a.name,    n->valuestring, sizeof(a.name));
    if (cJSON_IsString(t)) strlcpy(a.topic,   t->valuestring, sizeof(a.topic));
    if (cJSON_IsString(p)) strlcpy(a.payload, p->valuestring, sizeof(a.payload));
    int idx = (int)idx_item->valuedouble;
    cJSON_Delete(root);

    esp_err_t err = mqtt_action_update(idx, &a);
    if (err == ESP_ERR_NOT_FOUND) return send_error(req, "action not found");
    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

// POST /api/mqtt/action/test  {"idx":N}
static esp_err_t handle_mqtt_action_test(httpd_req_t *req)
{
    char body[64];
    if (read_body(req, body, sizeof(body)) != ESP_OK)
        return send_error(req, "body too large");
    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, "invalid json");

    cJSON *idx_item = cJSON_GetObjectItem(root, "idx");
    if (!cJSON_IsNumber(idx_item)) { cJSON_Delete(root); return send_error(req, "idx required"); }
    int idx = (int)idx_item->valuedouble;
    cJSON_Delete(root);

    mqtt_action_trigger(idx);
    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

// DELETE /api/mqtt/action  {"idx":N}
static esp_err_t handle_mqtt_action_delete(httpd_req_t *req)
{
    char body[64];
    if (read_body(req, body, sizeof(body)) != ESP_OK)
        return send_error(req, "body too large");
    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, "invalid json");

    cJSON *idx_item = cJSON_GetObjectItem(root, "idx");
    if (!cJSON_IsNumber(idx_item)) { cJSON_Delete(root); return send_error(req, "idx required"); }
    int idx = (int)idx_item->valuedouble;
    cJSON_Delete(root);

    esp_err_t err = mqtt_action_delete(idx);
    if (err == ESP_ERR_NOT_FOUND) return send_error(req, "action not found");
    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

// ── GPIO action handlers ──────────────────────────────────────────────────────

// GET /api/gpio/actions
static esp_err_t handle_gpio_actions_get(httpd_req_t *req)
{
    cJSON *arr = gpio_actions_export();
    if (!arr) return send_error(req, "could not read GPIO actions");
    send_cjson(req, arr);
    return ESP_OK;
}

// GET /api/gpio/pins
static esp_err_t handle_gpio_pins_get(httpd_req_t *req)
{
    cJSON *arr = gpio_pins_export();
    if (!arr) return send_error(req, "could not read GPIO pins");
    send_cjson(req, arr);
    return ESP_OK;
}

static esp_err_t read_gpio_action_body(httpd_req_t *req, char *body, size_t body_len,
                                       gpio_action_t *out, int *idx_out, bool require_idx)
{
    if (read_body(req, body, body_len) != ESP_OK) {
        send_error(req, "body too large");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        send_error(req, "invalid json");
        return ESP_FAIL;
    }
    const char *error = NULL;
    if (gpio_action_parse_json(root, out, idx_out, require_idx, &error) != ESP_OK) {
        cJSON_Delete(root);
        send_error(req, error ? error : "invalid gpio action");
        return ESP_FAIL;
    }
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/gpio/actions
static esp_err_t handle_gpio_action_add(httpd_req_t *req)
{
    char body[256];
    gpio_action_t action;
    esp_err_t parse_err = read_gpio_action_body(req, body, sizeof(body), &action, NULL, false);
    if (parse_err != ESP_OK) return ESP_OK;

    int idx = -1;
    esp_err_t err = gpio_action_add(&action, &idx);
    if (err == ESP_ERR_INVALID_ARG)   return send_error(req, "invalid gpio action");
    if (err == ESP_ERR_INVALID_STATE) return send_error(req, "gpio already used by another action");
    if (err == ESP_ERR_NO_MEM)        return send_error(req, "action list full");
    if (err != ESP_OK)                return send_error(req, "could not save action");

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", true);
    cJSON_AddNumberToObject(obj, "idx", idx);
    send_cjson(req, obj);
    return ESP_OK;
}

// PUT /api/gpio/action
static esp_err_t handle_gpio_action_update(httpd_req_t *req)
{
    char body[256];
    gpio_action_t action;
    int idx = -1;
    esp_err_t parse_err = read_gpio_action_body(req, body, sizeof(body), &action, &idx, true);
    if (parse_err != ESP_OK) return ESP_OK;

    esp_err_t err = gpio_action_update(idx, &action);
    if (err == ESP_ERR_INVALID_ARG)   return send_error(req, "invalid gpio action");
    if (err == ESP_ERR_INVALID_STATE) return send_error(req, "gpio already used by another action");
    if (err == ESP_ERR_NOT_FOUND)     return send_error(req, "action not found");
    if (err != ESP_OK)                return send_error(req, "could not save action");

    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

// POST /api/gpio/action/test  {"idx":N}
static esp_err_t handle_gpio_action_test(httpd_req_t *req)
{
    char body[64];
    if (read_body(req, body, sizeof(body)) != ESP_OK)
        return send_error(req, "body too large");
    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, "invalid json");

    cJSON *idx_item = cJSON_GetObjectItem(root, "idx");
    if (!cJSON_IsNumber(idx_item)) { cJSON_Delete(root); return send_error(req, "idx required"); }
    int idx = (int)idx_item->valuedouble;
    cJSON_Delete(root);

    gpio_action_trigger(idx);
    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

// DELETE /api/gpio/action  {"idx":N}
static esp_err_t handle_gpio_action_delete(httpd_req_t *req)
{
    char body[64];
    if (read_body(req, body, sizeof(body)) != ESP_OK)
        return send_error(req, "body too large");
    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, "invalid json");

    cJSON *idx_item = cJSON_GetObjectItem(root, "idx");
    if (!cJSON_IsNumber(idx_item)) { cJSON_Delete(root); return send_error(req, "idx required"); }
    int idx = (int)idx_item->valuedouble;
    cJSON_Delete(root);

    esp_err_t err = gpio_action_delete(idx);
    if (err == ESP_ERR_NOT_FOUND) return send_error(req, "action not found");
    if (err != ESP_OK)            return send_error(req, "could not delete action");
    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

// GET /api/ble/devices
static esp_err_t handle_ble_devices(httpd_req_t *req)
{
    cJSON *arr = ble_access_devices_export(BLE_ACCESS_EXPORT_VIEW_FE);
    if (!arr) return send_error(req, "could not read BLE devices");
    send_cjson(req, arr);
    return ESP_OK;
}

// GET /api/ble/register/status
static esp_err_t handle_ble_reg_status(httpd_req_t *req)
{
    cJSON *obj = ble_access_registration_status_export();
    if (!obj) return send_error(req, "could not read BLE registration status");
    send_cjson(req, obj);
    return ESP_OK;
}

// POST /api/ble/register/start
static esp_err_t handle_ble_reg_start(httpd_req_t *req)
{
    ble_access_register_start();
    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

// POST /api/ble/register/cancel
static esp_err_t handle_ble_reg_cancel(httpd_req_t *req)
{
    ble_access_register_cancel();
    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

// POST /api/ble/register/confirm  {"mac":"AA:BB:CC:DD:EE:FF","key":"<32 hex>","label":"..."}
static esp_err_t handle_ble_reg_confirm(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof(body)) != ESP_OK)
        return send_error(req, "body too large");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, "invalid json");

    cJSON *mac_item   = cJSON_GetObjectItem(root, "mac");
    cJSON *key_item   = cJSON_GetObjectItem(root, "key");
    cJSON *label_item = cJSON_GetObjectItem(root, "label");

    if (!cJSON_IsString(mac_item) || !cJSON_IsString(key_item)) {
        cJSON_Delete(root);
        return send_error(req, "mac and key required");
    }

    uint8_t mac[6], key[16];
    if (!ble_access_mac_from_str(mac_item->valuestring, mac)) {
        cJSON_Delete(root);
        return send_error(req, "invalid mac");
    }
    if (!ble_access_key_from_str(key_item->valuestring, key)) {
        cJSON_Delete(root);
        return send_error(req, "key must be 32 hex chars");
    }

    const char *label = cJSON_IsString(label_item) ? label_item->valuestring : "Device";
    esp_err_t err = ble_access_register_confirm(mac, key, label);
    cJSON_Delete(root);

    if (err == ESP_ERR_NO_MEM)      return send_error(req, "device list full");
    if (err == ESP_ERR_INVALID_ARG) return send_error(req, "mac already registered");
    if (err == ESP_ERR_INVALID_STATE)
        return send_error(req, "no captured device is waiting for confirmation");
    if (err == ESP_ERR_NOT_SUPPORTED)
        return send_error(req, "device exposes more buttons than supported");
    if (err != ESP_OK)
        return send_error(req, "could not validate the key against the captured packet");

    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

// PATCH /api/ble/device
// Button mappings are integer bitmasks referencing MQTT or GPIO action slots.
static esp_err_t handle_ble_device_update(httpd_req_t *req)
{
    char body[2048];
    if (read_body(req, body, sizeof(body)) != ESP_OK)
        return send_error(req, "body too large");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, "invalid json");

    cJSON *mac_item = cJSON_GetObjectItem(root, "mac");
    if (!cJSON_IsString(mac_item)) { cJSON_Delete(root); return send_error(req, "mac required"); }

    uint8_t mac[6];
    if (!ble_access_mac_from_str(mac_item->valuestring, mac)) {
        cJSON_Delete(root);
        return send_error(req, "invalid mac");
    }

    ble_device_t current;
    if (ble_access_get_device_by_mac(mac, &current) != ESP_OK) {
        cJSON_Delete(root);
        return send_error(req, "device not found");
    }

    cJSON *lbl_item = cJSON_GetObjectItem(root, "label");
    cJSON *en_item  = cJSON_GetObjectItem(root, "enabled");
    cJSON *buttons_item = cJSON_GetObjectItem(root, "buttons");
    cJSON *key_item = cJSON_GetObjectItem(root, "key");

    if (cJSON_IsString(lbl_item)) strlcpy(current.label, lbl_item->valuestring, sizeof(current.label));
    if (cJSON_IsBool(en_item))    current.enabled      = cJSON_IsTrue(en_item);

    if (buttons_item) {
        const char *buttons_error = NULL;
        ble_button_config_t next_buttons[BLE_ACCESS_MAX_BUTTONS];
        if (!ble_access_parse_button_configs_json(buttons_item, current.button_count,
                                                  next_buttons, &buttons_error)) {
            cJSON_Delete(root);
            return send_error(req, buttons_error ? buttons_error : "invalid buttons payload");
        }
        memcpy(current.buttons, next_buttons, sizeof(current.buttons));
    }

    if (cJSON_IsString(key_item) &&
            strlen(key_item->valuestring) > 0 &&
            !ble_access_key_from_str(key_item->valuestring, NULL)) {
        cJSON_Delete(root);
        return send_error(req, "key must be 32 hex chars");
    }

    // Optional key update: present and non-empty = replace, absent or empty = keep
    uint8_t new_key[16];
    bool has_new_key = cJSON_IsString(key_item) &&
                       strlen(key_item->valuestring) > 0 &&
                       ble_access_key_from_str(key_item->valuestring, new_key);
    cJSON_Delete(root);

    esp_err_t err = ble_access_device_update(mac, &current);
    if (err == ESP_ERR_NOT_FOUND) return send_error(req, "device not found");
    if (err != ESP_OK)            return send_error(req, "update failed");

    if (has_new_key) {
        err = ble_access_device_update_key(mac, new_key);
        if (err != ESP_OK) return send_error(req, "key update failed — other fields saved");
    }

    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

// POST /api/ble/device/reimport  {"mac":"AA:BB:CC:DD:EE:FF"}
static esp_err_t handle_ble_device_reimport(httpd_req_t *req)
{
    char body[64];
    if (read_body(req, body, sizeof(body)) != ESP_OK)
        return send_error(req, "body too large");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, "invalid json");

    cJSON *mac_item = cJSON_GetObjectItem(root, "mac");
    if (!cJSON_IsString(mac_item)) { cJSON_Delete(root); return send_error(req, "mac required"); }

    uint8_t mac[6];
    if (!ble_access_mac_from_str(mac_item->valuestring, mac)) {
        cJSON_Delete(root);
        return send_error(req, "invalid mac");
    }
    cJSON_Delete(root);

    esp_err_t err = ble_access_device_reimport_key(mac);
    if (err == ESP_ERR_NOT_FOUND) return send_error(req, "device not found");
    if (err != ESP_OK)            return send_error(req, "local key re-import failed");

    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

// DELETE /api/ble/device  {"mac":"AA:BB:CC:DD:EE:FF"}
static esp_err_t handle_ble_device_delete(httpd_req_t *req)
{
    char body[64];
    if (read_body(req, body, sizeof(body)) != ESP_OK)
        return send_error(req, "body too large");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, "invalid json");

    cJSON *mac_item = cJSON_GetObjectItem(root, "mac");
    if (!cJSON_IsString(mac_item)) { cJSON_Delete(root); return send_error(req, "mac required"); }

    uint8_t mac[6];
    if (!ble_access_mac_from_str(mac_item->valuestring, mac)) {
        cJSON_Delete(root);
        return send_error(req, "invalid mac");
    }
    cJSON_Delete(root);

    esp_err_t err = ble_access_device_delete(mac);
    if (err == ESP_ERR_NOT_FOUND) return send_error(req, "device not found");
    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

// ── OTA update ────────────────────────────────────────────────────────────────

static bool ota_try_lock(void)
{
    return s_ota_mutex && xSemaphoreTake(s_ota_mutex, 0) == pdTRUE;
}

static void ota_unlock(void)
{
    if (s_ota_mutex) xSemaphoreGive(s_ota_mutex);
}

// GET /api/system/update/check
static esp_err_t handle_update_check(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    // Intentionally close active console streams before the OTA manifest check.
    // With the SSE console viewer still attached, free heap can drop enough
    // to make the outbound HTTPS/TLS setup unreliable for update verification.
    // The short delay gives the replaced stream time to unwind and release memory.
    const char *expected_asset_name = ota_release_profile_ota_asset_name();
    console_stream_close_all();
    vTaskDelay(pdMS_TO_TICKS(300));
    ota_release_info_t release;
    esp_err_t err = ota_manifest_fetch_latest_release(&release);
    if (err == ESP_ERR_NOT_FOUND) {
        char msg[160];
        snprintf(msg, sizeof(msg), "latest release is missing %s or its sha256 digest",
                 expected_asset_name ? expected_asset_name : "the expected OTA asset");
        return send_error(req, msg);
    }
    if (err != ESP_OK) return send_error(req, "could not fetch latest OTA manifest");

    char current_version[40];
    format_version_label(app->version, current_version, sizeof(current_version));

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", true);
    cJSON_AddStringToObject(obj, "current_version", current_version);
    cJSON_AddStringToObject(obj, "latest_version", release.version_label);
    cJSON_AddBoolToObject(obj, "update_available", compare_versions_for_update(release.tag, app->version) > 0);
    cJSON_AddStringToObject(obj, "release_url", release.html_url);
    cJSON_AddStringToObject(obj, "asset_name", release.asset_name);
    cJSON_AddNumberToObject(obj, "asset_size", release.asset_size);
    s_last_ota_release = release;
    send_cjson(req, obj);
    return ESP_OK;
}

// POST /api/system/ota  (raw binary body)
static esp_err_t handle_ota_upload(httpd_req_t *req)
{
    if (!ota_try_lock()) return send_error(req, "another OTA operation is already in progress");
    if (req->content_len <= 0) {
        ota_unlock();
        return send_error(req, "empty firmware image");
    }

    ota_upload_session_t *session = NULL;
    esp_err_t err = ota_manager_upload_begin((size_t)req->content_len, &session);
    if (err == ESP_ERR_NOT_FOUND) {
        ota_unlock();
        return send_error(req, "no OTA partition");
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        ota_unlock();
        return send_error_status(req, "413 Payload Too Large",
                                 "firmware image too large for OTA partition");
    }
    if (err != ESP_OK) { ota_unlock(); return send_error(req, "OTA begin failed"); }

    char buf[1024];
    int remaining = req->content_len;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf));
        if (n <= 0) {
            ota_manager_upload_abort(session);
            ota_unlock();
            return send_error(req, "receive error");
        }
        err = ota_manager_upload_write(session, buf, (size_t)n);
        if (err != ESP_OK) {
            ota_manager_upload_abort(session);
            ota_unlock();
            return send_error(req, "OTA write failed");
        }
        remaining -= n;
    }

    err = ota_manager_upload_finish(session);
    if (err != ESP_OK) {
        ota_unlock();
        return send_error_status(req, "422 Unprocessable Entity", "OTA validation failed");
    }

    ota_unlock();
    return send_ok_and_start_task(req, reboot_task, "reboot", "could not schedule reboot");
}

static esp_err_t handle_update_install(httpd_req_t *req)
{
    if (!ota_try_lock()) return send_error(req, "another OTA operation is already in progress");

    if (!s_last_ota_release.download_url[0] || !s_last_ota_release.tag[0]) {
        ota_unlock();
        return send_error(req, "check for updates first");
    }

    const esp_app_desc_t *app = esp_app_get_description();
    if (compare_versions_for_update(s_last_ota_release.tag, app->version) <= 0) {
        ota_unlock();
        return send_error(req, "no newer published update is available");
    }

    esp_err_t err = ota_manager_stage_update_job(s_last_ota_release.version_label,
                                                 s_last_ota_release.download_url,
                                                 s_last_ota_release.digest_hex);
    ota_unlock();
    if (err != ESP_OK) return send_error(req, "could not stage OTA update");

    return send_ok_and_start_task(req, reboot_task, "reboot", "could not schedule reboot");
}

// ── Config backup / restore ──────────────────────────────────────────────────

static cJSON *build_config_backup_root(const char **out_error)
{
    if (out_error) *out_error = NULL;

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    if (!cJSON_AddNumberToObject(root, "version", CONFIG_BACKUP_VERSION)) goto json_fail;

    if (!wifi_backup_export(root)) {
        if (out_error) *out_error = "could not export WiFi config";
        goto fail;
    }

    if (!auth_manager_backup_export(root)) {
        if (out_error) *out_error = "could not export auth config";
        goto fail;
    }

    if (!mqtt_backup_export(root)) {
        if (out_error) *out_error = "could not export MQTT config";
        goto fail;
    }

    if (!gpio_backup_export(root)) {
        if (out_error) *out_error = "could not export GPIO config";
        goto fail;
    }

    if (!ble_access_backup_export(root)) {
        if (out_error) *out_error = "could not export BLE config";
        goto fail;
    }
    return root;

json_fail:
    if (out_error) *out_error = "json error";
fail:
    cJSON_Delete(root);
    return NULL;
}

// GET /api/system/config
static esp_err_t handle_config_download(httpd_req_t *req)
{
    const char *error = NULL;
    cJSON *root = build_config_backup_root(&error);
    if (!root) return send_error(req, error ? error : "could not export config");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"bbb-config.json\"");
    char *str = cJSON_Print(root);
    cJSON_Delete(root);
    if (!str) return send_error(req, "json error");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    return ESP_OK;
}

// POST /api/system/config  (JSON body)
static esp_err_t handle_config_restore(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 12288)
        return send_error(req, "body too large (max 12 KB)");

    char *body = malloc(req->content_len + 1);
    if (!body) return send_error(req, "out of memory");

    int received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) { free(body); return send_error(req, "receive error"); }
        received += n;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return send_error(req, "invalid json");

    cJSON *version_item = cJSON_GetObjectItem(root, "version");
    if (!cJSON_IsNumber(version_item) ||
            version_item->valuedouble != (double)version_item->valueint ||
            (int)version_item->valuedouble != CONFIG_BACKUP_VERSION) {
        cJSON_Delete(root);
        return send_error(req, "unsupported config backup version");
    }
    const char *error = NULL;

    if (wifi_backup_import(root) != ESP_OK) {
        cJSON_Delete(root);
        return send_error(req, "could not restore WiFi config");
    }
    if (auth_manager_backup_import(root, &error) != ESP_OK) {
        cJSON_Delete(root);
        return send_error(req, error ? error : "could not restore auth config");
    }
    if (mqtt_backup_import(root) != ESP_OK) {
        cJSON_Delete(root);
        return send_error(req, "could not restore MQTT config");
    }
    if (gpio_backup_import(root) != ESP_OK) {
        cJSON_Delete(root);
        return send_error(req, "could not restore GPIO config");
    }
    if (ble_access_backup_import(root, &error) != ESP_OK) {
        cJSON_Delete(root);
        return send_error(req, error ? error : "could not restore BLE config");
    }

    cJSON_Delete(root);
    return send_ok_and_start_task(req, reboot_task, "reboot",
                                  "could not schedule reboot");
}

// ── Init ─────────────────────────────────────────────────────────────────────

void web_manager_init(void)
{
    esp_err_t auth_err = auth_manager_init();
    if (auth_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init auth manager: %s", esp_err_to_name(auth_err));
        return;
    }
    if (!s_ota_mutex) {
        s_ota_mutex = xSemaphoreCreateMutex();
    }
    if (!s_ota_mutex) {
        ESP_LOGE(TAG, "Failed to create OTA mutex");
        return;
    }

    static const route_def_t routes[] = {
        { .uri = "/",                         .method = HTTP_GET,    .handler = handle_root,                .auth_required = true },
        { .uri = "/wifi-handoff",             .method = HTTP_GET,    .handler = handle_wifi_handoff_page,   .auth_required = true },
        { .uri = "/console",                  .method = HTTP_GET,    .handler = handle_console_page,        .auth_required = true },
        { .uri = "/api/console/stream",       .method = HTTP_GET,    .handler = handle_console_stream,      .auth_required = true },
        { .uri = "/api/status",               .method = HTTP_GET,    .handler = handle_status,              .auth_required = true },
        { .uri = "/api/wifi/config",          .method = HTTP_GET,    .handler = handle_wifi_config_get,     .auth_required = true },
        { .uri = "/api/wifi/scan",            .method = HTTP_GET,    .handler = handle_wifi_scan,           .auth_required = true },
        { .uri = "/api/wifi/connect",         .method = HTTP_POST,   .handler = handle_wifi_connect,        .auth_required = true },
        { .uri = "/api/wifi",                 .method = HTTP_DELETE, .handler = handle_wifi_delete,         .auth_required = true },
        { .uri = "/api/mqtt/config",          .method = HTTP_GET,    .handler = handle_mqtt_config_get,     .auth_required = true },
        { .uri = "/api/mqtt/connect",         .method = HTTP_POST,   .handler = handle_mqtt_connect,        .auth_required = true },
        { .uri = "/api/mqtt/actions",         .method = HTTP_GET,    .handler = handle_mqtt_actions_get,    .auth_required = true },
        { .uri = "/api/mqtt/actions",         .method = HTTP_POST,   .handler = handle_mqtt_action_add,     .auth_required = true },
        { .uri = "/api/mqtt/action",          .method = HTTP_PUT,    .handler = handle_mqtt_action_update,  .auth_required = true },
        { .uri = "/api/mqtt/action",          .method = HTTP_DELETE, .handler = handle_mqtt_action_delete,  .auth_required = true },
        { .uri = "/api/mqtt/action/test",     .method = HTTP_POST,   .handler = handle_mqtt_action_test,    .auth_required = true },
        { .uri = "/api/mqtt",                 .method = HTTP_DELETE, .handler = handle_mqtt_delete,         .auth_required = true },
        { .uri = "/api/gpio/actions",         .method = HTTP_GET,    .handler = handle_gpio_actions_get,    .auth_required = true },
        { .uri = "/api/gpio/pins",            .method = HTTP_GET,    .handler = handle_gpio_pins_get,       .auth_required = true },
        { .uri = "/api/gpio/actions",         .method = HTTP_POST,   .handler = handle_gpio_action_add,     .auth_required = true },
        { .uri = "/api/gpio/action",          .method = HTTP_PUT,    .handler = handle_gpio_action_update,  .auth_required = true },
        { .uri = "/api/gpio/action",          .method = HTTP_DELETE, .handler = handle_gpio_action_delete,  .auth_required = true },
        { .uri = "/api/gpio/action/test",     .method = HTTP_POST,   .handler = handle_gpio_action_test,    .auth_required = true },
        { .uri = "/api/ap/start",             .method = HTTP_POST,   .handler = handle_ap_start,            .auth_required = true },
        { .uri = "/api/ap/stop",              .method = HTTP_POST,   .handler = handle_ap_stop,             .auth_required = true },
        { .uri = "/api/ap/handoff/complete",  .method = HTTP_POST,   .handler = handle_ap_handoff_complete, .auth_required = true },
        { .uri = "/api/ap/config",            .method = HTTP_GET,    .handler = handle_ap_config_get,       .auth_required = true },
        { .uri = "/api/ap/config",            .method = HTTP_POST,   .handler = handle_ap_config_set,       .auth_required = true },
        { .uri = "/api/system/reboot",        .method = HTTP_POST,   .handler = handle_system_reboot,       .auth_required = true },
        { .uri = "/api/system/factory-reset", .method = HTTP_POST,   .handler = handle_system_factory_reset,.auth_required = true },
        { .uri = "/api/system/auth",          .method = HTTP_GET,    .handler = handle_auth_config_get,     .auth_required = true },
        { .uri = "/api/system/auth",          .method = HTTP_POST,   .handler = handle_auth_config_set,     .auth_required = true },
        { .uri = "/api/system/update/check",  .method = HTTP_GET,    .handler = handle_update_check,        .auth_required = true },
        { .uri = "/api/system/update",        .method = HTTP_POST,   .handler = handle_update_install,      .auth_required = true },
        { .uri = "/api/system/ota",           .method = HTTP_POST,   .handler = handle_ota_upload,          .auth_required = true },
        { .uri = "/api/system/config",        .method = HTTP_GET,    .handler = handle_config_download,     .auth_required = true },
        { .uri = "/api/system/config",        .method = HTTP_POST,   .handler = handle_config_restore,      .auth_required = true },
        { .uri = "/api/ble/devices",          .method = HTTP_GET,    .handler = handle_ble_devices,         .auth_required = true },
        { .uri = "/api/ble/register/status",  .method = HTTP_GET,    .handler = handle_ble_reg_status,      .auth_required = true },
        { .uri = "/api/ble/register/start",   .method = HTTP_POST,   .handler = handle_ble_reg_start,       .auth_required = true },
        { .uri = "/api/ble/register/cancel",  .method = HTTP_POST,   .handler = handle_ble_reg_cancel,      .auth_required = true },
        { .uri = "/api/ble/register/confirm", .method = HTTP_POST,   .handler = handle_ble_reg_confirm,     .auth_required = true },
        { .uri = "/api/ble/device",           .method = HTTP_PATCH,  .handler = handle_ble_device_update,   .auth_required = true },
        { .uri = "/api/ble/device/reimport",  .method = HTTP_POST,   .handler = handle_ble_device_reimport, .auth_required = true },
        { .uri = "/api/ble/device",           .method = HTTP_DELETE, .handler = handle_ble_device_delete,   .auth_required = true },
        { .uri = "/*",                        .method = HTTP_GET,    .handler = handle_captive,             .auth_required = true },
    };
    enum { ROUTE_COUNT = sizeof(routes) / sizeof(routes[0]) };
    static route_ctx_t route_ctxs[ROUTE_COUNT];

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size        = 10240;
    config.max_uri_handlers  = ROUTE_COUNT;
    config.max_open_sockets  = 4;
    config.lru_purge_enable  = true;
    config.uri_match_fn      = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    for (size_t i = 0; i < ROUTE_COUNT; i++) {
        route_ctxs[i].inner = routes[i].handler;
        route_ctxs[i].auth_required = routes[i].auth_required;
        httpd_uri_t uri = {
            .uri = routes[i].uri,
            .method = routes[i].method,
            .handler = handle_with_auth,
            .user_ctx = &route_ctxs[i],
        };
        httpd_register_uri_handler(server, &uri);
    }

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
}
