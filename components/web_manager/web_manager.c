#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <ctype.h>
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "nvs_flash.h"
#include "nvs.h"
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
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "gpio_manager.h"
#include "web_manager.h"
#include "ble_access.h"
#include "console_manager.h"

static const char *TAG = "web_manager";
static const char *AUTH_NS = "http_auth";
#define AUTH_USER_MAX     32
#define AUTH_PASS_MAX     64
#define AUTH_HASH_HEX_LEN 65
#define GITHUB_HTTP_BUFFER_SIZE    8192
#define GITHUB_HTTP_TX_BUFFER_SIZE 1024
#define GITHUB_HTTP_MAX_REDIRECTS  5
static const char *GITHUB_RELEASE_URL = "https://api.github.com/repos/robertoamd90/blu-button-bridge/releases/latest";
static const char *GITHUB_ASSET_NAME  = "BluButtonBridge.bin";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t console_html_start[] asm("_binary_console_html_start");
extern const uint8_t console_html_end[]   asm("_binary_console_html_end");

typedef struct {
    bool enabled;
    bool password_set;
    char username[AUTH_USER_MAX + 1];
    char password_sha256[AUTH_HASH_HEX_LEN];
} auth_config_t;

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

static auth_config_t       s_auth_cfg = {0};
static SemaphoreHandle_t   s_auth_mutex = NULL;
static SemaphoreHandle_t   s_ota_mutex = NULL;
static portMUX_TYPE        s_console_stream_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t            s_console_stream_generation = 0;

static void auth_load_config(void);
static bool auth_require(httpd_req_t *req);

// ── HTTP helpers ─────────────────────────────────────────────────────────────

static void bytes_to_hex(const uint8_t *in, size_t len, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = hex[(in[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[in[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

static bool sha256_hex(const char *input, char out[AUTH_HASH_HEX_LEN])
{
    uint8_t digest[32];
    if (mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                   (const unsigned char *)input, strlen(input), digest) != 0)
        return false;
    bytes_to_hex(digest, sizeof(digest), out);
    return true;
}

static esp_err_t auth_save_config_locked(const auth_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    if (nvs_open(AUTH_NS, NVS_READWRITE, &nvs) != ESP_OK) return ESP_FAIL;
    esp_err_t err = nvs_set_str(nvs, "enabled", cfg->enabled ? "1" : "0");
    if (err == ESP_OK) err = nvs_set_str(nvs, "user", cfg->username);
    if (err == ESP_OK) err = nvs_set_str(nvs, "pass_sha", cfg->password_sha256);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static void auth_load_config(void)
{
    memset(&s_auth_cfg, 0, sizeof(s_auth_cfg));

    nvs_handle_t nvs;
    if (nvs_open(AUTH_NS, NVS_READONLY, &nvs) != ESP_OK) return;

    char enabled[4] = {0};
    size_t len = sizeof(enabled);
    if (nvs_get_str(nvs, "enabled", enabled, &len) == ESP_OK)
        s_auth_cfg.enabled = (enabled[0] == '1');

    len = sizeof(s_auth_cfg.username);
    nvs_get_str(nvs, "user", s_auth_cfg.username, &len);

    len = sizeof(s_auth_cfg.password_sha256);
    nvs_get_str(nvs, "pass_sha", s_auth_cfg.password_sha256, &len);
    s_auth_cfg.password_set = (strlen(s_auth_cfg.password_sha256) == AUTH_HASH_HEX_LEN - 1);
    nvs_close(nvs);
}

static bool send_auth_challenge(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"BluButtonBridge\"");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Authentication required");
    return false;
}

static bool auth_require(httpd_req_t *req)
{
    auth_config_t cfg = {0};
    if (s_auth_mutex) xSemaphoreTake(s_auth_mutex, portMAX_DELAY);
    cfg = s_auth_cfg;
    if (s_auth_mutex) xSemaphoreGive(s_auth_mutex);

    if (!cfg.enabled) return true;
    if (!cfg.password_set || cfg.username[0] == '\0') return send_auth_challenge(req);

    size_t hdr_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hdr_len == 0) return send_auth_challenge(req);

    char header[160];
    if (hdr_len >= sizeof(header)) return send_auth_challenge(req);
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK)
        return send_auth_challenge(req);
    if (strncmp(header, "Basic ", 6) != 0) return send_auth_challenge(req);

    unsigned char decoded[AUTH_USER_MAX + AUTH_PASS_MAX + 4];
    size_t decoded_len = 0;
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                              (const unsigned char *)(header + 6),
                              strlen(header + 6)) != 0) {
        return send_auth_challenge(req);
    }
    decoded[decoded_len] = '\0';

    char *sep = strchr((char *)decoded, ':');
    if (!sep) return send_auth_challenge(req);
    *sep = '\0';
    const char *username = (const char *)decoded;
    const char *password = sep + 1;

    if (strcmp(username, cfg.username) != 0) return send_auth_challenge(req);

    char password_sha256[AUTH_HASH_HEX_LEN];
    if (!sha256_hex(password, password_sha256)) return send_auth_challenge(req);
    if (strcmp(password_sha256, cfg.password_sha256) != 0) return send_auth_challenge(req);

    return true;
}

static bool auth_username_is_valid(const char *username)
{
    return username && strchr(username, ':') == NULL;
}

static bool auth_password_hash_copy(const char *input, char out[AUTH_HASH_HEX_LEN])
{
    if (!input || !out || strlen(input) != AUTH_HASH_HEX_LEN - 1) return false;
    for (size_t i = 0; i < AUTH_HASH_HEX_LEN - 1; i++) {
        if (!isxdigit((unsigned char)input[i])) return false;
        out[i] = (char)tolower((unsigned char)input[i]);
    }
    out[AUTH_HASH_HEX_LEN - 1] = '\0';
    return true;
}

static esp_err_t handle_with_auth(httpd_req_t *req)
{
    route_ctx_t *ctx = (route_ctx_t *)req->user_ctx;
    if (!ctx || !ctx->inner) return ESP_FAIL;
    if (ctx->auth_required && !auth_require(req)) return ESP_OK;
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
    char download_url[512];
    char digest_hex[65];
    int asset_size;
} github_release_info_t;

typedef struct {
    esp_ota_handle_t ota;
    mbedtls_md_context_t sha_ctx;
    bool sha_started;
    size_t bytes_written;
    esp_err_t stream_err;
} ota_download_ctx_t;

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

static bool parse_github_digest(const char *digest, char *out_hex, size_t out_len)
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
            size_t new_cap = (buffer->cap > 0) ? buffer->cap : 4096;
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

static esp_err_t ota_download_event_handler(esp_http_client_event_t *evt)
{
    ota_download_ctx_t *ctx = (ota_download_ctx_t *)evt->user_data;
    if (!ctx) return ESP_OK;

    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data_len <= 0) return ESP_OK;

    int status = esp_http_client_get_status_code(evt->client);
    if (status != 200) return ESP_OK;

    if (!ctx->sha_started) {
        const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        if (!md_info) {
            ctx->stream_err = ESP_FAIL;
            return ESP_FAIL;
        }
        mbedtls_md_init(&ctx->sha_ctx);
        if (mbedtls_md_setup(&ctx->sha_ctx, md_info, 0) != 0 || mbedtls_md_starts(&ctx->sha_ctx) != 0) {
            mbedtls_md_free(&ctx->sha_ctx);
            ctx->stream_err = ESP_FAIL;
            return ESP_FAIL;
        }
        ctx->sha_started = true;
    }

    if (mbedtls_md_update(&ctx->sha_ctx, (const unsigned char *)evt->data, evt->data_len) != 0) {
        ctx->stream_err = ESP_FAIL;
        return ESP_FAIL;
    }

    esp_err_t err = esp_ota_write(ctx->ota, evt->data, evt->data_len);
    if (err != ESP_OK) {
        ctx->stream_err = err;
        return err;
    }

    ctx->bytes_written += (size_t)evt->data_len;
    return ESP_OK;
}

static void sha256_digest_to_hex(const unsigned char digest[32], char *out_hex, size_t out_len)
{
    if (!out_hex || out_len < 65) return;
    for (int i = 0; i < 32; i++) snprintf(out_hex + (i * 2), out_len - (size_t)(i * 2), "%02x", digest[i]);
}

static esp_err_t github_http_get_json(const char *url, http_buffer_t *buffer)
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
        .buffer_size = GITHUB_HTTP_BUFFER_SIZE,
        .buffer_size_tx = GITHUB_HTTP_TX_BUFFER_SIZE,
        .event_handler = http_buffer_event_handler,
        .user_data = buffer,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;

    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");
    esp_http_client_set_header(client, "User-Agent", "BluButtonBridge");
    esp_http_client_set_header(client, "X-GitHub-Api-Version", "2022-11-28");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) return err;
    if (status != 200) return ESP_FAIL;
    if (buffer->alloc_failed) return ESP_ERR_NO_MEM;
    if (!buffer->buf || buffer->len == 0) return ESP_FAIL;
    return ESP_OK;
}

static esp_err_t github_fetch_latest_release(github_release_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));

    http_buffer_t buffer = {0};

    esp_err_t err = github_http_get_json(GITHUB_RELEASE_URL, &buffer);
    if (err != ESP_OK) {
        free(buffer.buf);
        return err;
    }

    cJSON *root = cJSON_Parse(buffer.buf);
    free(buffer.buf);
    if (!root) return ESP_FAIL;

    cJSON *tag = cJSON_GetObjectItem(root, "tag_name");
    cJSON *html_url = cJSON_GetObjectItem(root, "html_url");
    cJSON *assets = cJSON_GetObjectItem(root, "assets");
    if (!cJSON_IsString(tag) || !cJSON_IsArray(assets)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    strlcpy(info->tag, tag->valuestring, sizeof(info->tag));
    format_version_label(tag->valuestring, info->version_label, sizeof(info->version_label));
    if (cJSON_IsString(html_url)) strlcpy(info->html_url, html_url->valuestring, sizeof(info->html_url));

    bool found_asset = false;
    cJSON *asset = NULL;
    cJSON_ArrayForEach(asset, assets) {
        cJSON *name = cJSON_GetObjectItem(asset, "name");
        if (!cJSON_IsString(name) || strcmp(name->valuestring, GITHUB_ASSET_NAME) != 0) continue;

        cJSON *download_url = cJSON_GetObjectItem(asset, "browser_download_url");
        cJSON *digest = cJSON_GetObjectItem(asset, "digest");
        cJSON *size = cJSON_GetObjectItem(asset, "size");
        if (!cJSON_IsString(download_url) || !cJSON_IsString(digest) || !cJSON_IsNumber(size)) break;
        if (!parse_github_digest(digest->valuestring, info->digest_hex, sizeof(info->digest_hex))) break;

        strlcpy(info->download_url, download_url->valuestring, sizeof(info->download_url));
        info->asset_size = (int)size->valuedouble;
        found_asset = true;
        break;
    }

    cJSON_Delete(root);
    return found_asset ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t ota_install_from_github_release(const github_release_info_t *info)
{
    if (!info || info->download_url[0] == '\0' || info->digest_hex[0] == '\0') return ESP_ERR_INVALID_ARG;

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return ESP_ERR_NOT_FOUND;

    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) return err;

    ota_download_ctx_t ctx = {
        .ota = ota,
        .stream_err = ESP_OK,
    };

    esp_http_client_config_t cfg = {
        .url = info->download_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .buffer_size = GITHUB_HTTP_BUFFER_SIZE,
        .buffer_size_tx = GITHUB_HTTP_TX_BUFFER_SIZE,
        .max_redirection_count = GITHUB_HTTP_MAX_REDIRECTS,
        .event_handler = ota_download_event_handler,
        .user_data = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        esp_ota_abort(ota);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "User-Agent", "BluButtonBridge");

    err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || ctx.stream_err != ESP_OK || status != 200 || ctx.bytes_written == 0) {
        if (ctx.sha_started) mbedtls_md_free(&ctx.sha_ctx);
        esp_ota_abort(ota);
        return (ctx.stream_err != ESP_OK) ? ctx.stream_err : ESP_FAIL;
    }

    unsigned char digest[32];
    char actual_hex[65];
    if (!ctx.sha_started || mbedtls_md_finish(&ctx.sha_ctx, digest) != 0) {
        if (ctx.sha_started) mbedtls_md_free(&ctx.sha_ctx);
        esp_ota_abort(ota);
        return ESP_FAIL;
    }
    mbedtls_md_free(&ctx.sha_ctx);

    sha256_digest_to_hex(digest, actual_hex, sizeof(actual_hex));
    if (strcmp(actual_hex, info->digest_hex) != 0) {
        ESP_LOGW(TAG, "GitHub OTA digest mismatch: expected %s got %s", info->digest_hex, actual_hex);
        esp_ota_abort(ota);
        return ESP_ERR_INVALID_CRC;
    }

    err = esp_ota_end(ota);
    if (err != ESP_OK) return err;

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) return err;

    return ESP_OK;
}

// ── Async background tasks (WiFi/MQTT connect without blocking the HTTP path) ─

typedef struct { char ssid[33]; char pass[65]; bool has_pass; } wifi_creds_t;
typedef struct { char host[128]; uint32_t port; char user[64]; char pass[64]; bool tls; bool has_pass; } mqtt_creds_t;

static void wifi_connect_task(void *arg)
{
    wifi_creds_t *c = (wifi_creds_t *)arg;
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
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "wifi",        wifi_status_str(wifi_get_status()));
    cJSON_AddStringToObject(obj, "mqtt",        mqtt_status_str(mqtt_get_status()));
    cJSON_AddStringToObject(obj, "ap",          wifi_ap_is_active() ? "up" : "down");
    cJSON_AddStringToObject(obj, "ble",         ble_status_str(ble_get_status()));
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

// GET /api/ap/config
static esp_err_t handle_ap_config_get(httpd_req_t *req)
{
    wifi_ap_settings_t cfg;
    wifi_ap_load_config(&cfg);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject  (obj, "enabled",  cfg.enabled);
    cJSON_AddStringToObject(obj, "ssid",     cfg.ssid);
    cJSON_AddStringToObject(obj, "password", cfg.password);
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
    wifi_ap_save_config(&cfg);

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
    cJSON_Delete(root);

    if (xTaskCreate(wifi_connect_task, "wifi_conn", 4096, c, 5, NULL) != pdPASS) {
        free(c);
        return send_error(req, "could not start task");
    }
    send_json(req, "{\"ok\":true}");
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
    char ssid[33] = {};
    bool has_ssid = wifi_get_ssid(ssid, sizeof(ssid));
    bool has_pass = wifi_get_password_set();
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "ssid",         ssid);
    cJSON_AddBoolToObject  (obj, "ssid_set",     has_ssid);
    cJSON_AddBoolToObject  (obj, "password_set", has_pass);
    send_cjson(req, obj);
    return ESP_OK;
}

// GET /api/wifi/scan
static esp_err_t handle_wifi_scan(httpd_req_t *req)
{
    wifi_scan_entry_t results[20];
    int n = wifi_scan_get_results(results, 20);
    if (n < 0) return send_error(req, "scan failed");

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", results[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", results[i].rssi);
        cJSON_AddItemToArray(arr, item);
    }
    send_cjson(req, arr);
    return ESP_OK;
}

// GET /api/mqtt/config
static esp_err_t handle_mqtt_config_get(httpd_req_t *req)
{
    char host[128] = {};
    char user[64]  = {};
    uint32_t port  = 1883;
    bool use_tls   = false;
    bool has_pass  = false;
    bool ok = mqtt_get_saved_config(host, sizeof(host), &port,
                                    user, sizeof(user), &use_tls, &has_pass);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject  (obj, "configured",   ok);
    cJSON_AddStringToObject(obj, "host",         host);
    cJSON_AddNumberToObject(obj, "port",         (double)port);
    cJSON_AddStringToObject(obj, "username",     user);
    cJSON_AddBoolToObject  (obj, "tls",          use_tls);
    cJSON_AddBoolToObject  (obj, "password_set", has_pass);
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

// POST /api/system/reboot
static esp_err_t handle_system_reboot(httpd_req_t *req)
{
    send_json(req, "{\"ok\":true}");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// POST /api/system/factory-reset
static esp_err_t handle_system_factory_reset(httpd_req_t *req)
{
    send_json(req, "{\"ok\":true}");
    xTaskCreate(factory_reset_task, "factory_rst", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// GET /api/system/auth
static esp_err_t handle_auth_config_get(httpd_req_t *req)
{
    auth_config_t cfg = {0};
    xSemaphoreTake(s_auth_mutex, portMAX_DELAY);
    cfg = s_auth_cfg;
    xSemaphoreGive(s_auth_mutex);

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "enabled", cfg.enabled);
    cJSON_AddStringToObject(obj, "username", cfg.username);
    cJSON_AddBoolToObject(obj, "password_set", cfg.password_set);
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

    cJSON *enabled_item = cJSON_GetObjectItem(root, "enabled");
    cJSON *user_item    = cJSON_GetObjectItem(root, "username");
    cJSON *pass_item    = cJSON_GetObjectItem(root, "password");

    auth_config_t next = {0};
    xSemaphoreTake(s_auth_mutex, portMAX_DELAY);
    next = s_auth_cfg;
    xSemaphoreGive(s_auth_mutex);

    if (cJSON_IsBool(enabled_item))
        next.enabled = cJSON_IsTrue(enabled_item);
    if (cJSON_IsString(user_item)) {
        if (strlen(user_item->valuestring) > AUTH_USER_MAX) {
            cJSON_Delete(root);
            return send_error(req, "username too long");
        }
        if (!auth_username_is_valid(user_item->valuestring)) {
            cJSON_Delete(root);
            return send_error(req, "username cannot contain ':'");
        }
        strlcpy(next.username, user_item->valuestring, sizeof(next.username));
    }
    if (cJSON_IsString(pass_item)) {
        if (strlen(pass_item->valuestring) > AUTH_PASS_MAX) {
            cJSON_Delete(root);
            return send_error(req, "password too long");
        }
        if (pass_item->valuestring[0] == '\0') {
            next.password_sha256[0] = '\0';
            next.password_set = false;
        } else if (!sha256_hex(pass_item->valuestring, next.password_sha256)) {
            cJSON_Delete(root);
            return send_error(req, "password hashing failed");
        } else {
            next.password_set = true;
        }
    }
    cJSON_Delete(root);

    if (next.enabled && next.username[0] == '\0')
        return send_error(req, "username required when auth is enabled");
    if (next.enabled && !auth_username_is_valid(next.username))
        return send_error(req, "username cannot contain ':'");
    if (next.enabled && !next.password_set)
        return send_error(req, "password required when auth is enabled");

    xSemaphoreTake(s_auth_mutex, portMAX_DELAY);
    esp_err_t err = auth_save_config_locked(&next);
    if (err == ESP_OK) s_auth_cfg = next;
    xSemaphoreGive(s_auth_mutex);
    if (err != ESP_OK) return send_error(req, "could not save auth config");

    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

// ── BLE access handlers ───────────────────────────────────────────────────────

// Format MAC (NimBLE little-endian) → "AA:BB:CC:DD:EE:FF" (MSB first, standard notation)
static void mac_to_str(const uint8_t mac[6], char *out)
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);
}

// Parse "AA:BB:CC:DD:EE:FF" → NimBLE little-endian (mac[0]=LSB)
static bool mac_from_str(const char *str, uint8_t mac[6])
{
    unsigned int b[6];
    if (sscanf(str, "%02X:%02X:%02X:%02X:%02X:%02X",
               &b[5], &b[4], &b[3], &b[2], &b[1], &b[0]) != 6) return false;
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)b[i];
    return true;
}

static bool key_from_str(const char *str, uint8_t key[16])
{
    if (strlen(str) != 32) return false;
    for (int i = 0; i < 16; i++) {
        unsigned int byte;
        if (sscanf(str + i * 2, "%02X", &byte) != 1) return false;
        if (key) key[i] = (uint8_t)byte;
    }
    return true;
}

// ── MQTT action handlers ──────────────────────────────────────────────────────

// GET /api/mqtt/actions
static esp_err_t handle_mqtt_actions_get(httpd_req_t *req)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < MQTT_MAX_ACTIONS; i++) {
        mqtt_action_t a;
        if (mqtt_action_get(i, &a) == ESP_OK) {
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(obj, "idx",     i);
            cJSON_AddStringToObject(obj, "name",    a.name);
            cJSON_AddStringToObject(obj, "topic",   a.topic);
            cJSON_AddStringToObject(obj, "payload", a.payload);
            cJSON_AddItemToArray(arr, obj);
        }
    }
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
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < GPIO_ACTION_MAX; i++) {
        gpio_action_t a;
        if (gpio_action_get(i, &a) == ESP_OK) {
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(obj, "idx", i);
            cJSON_AddStringToObject(obj, "name", a.name);
            cJSON_AddNumberToObject(obj, "gpio", a.gpio_num);
            cJSON_AddBoolToObject(obj, "idle_on", a.idle_on);
            cJSON_AddBoolToObject(obj, "active_low", a.active_low);
            cJSON_AddStringToObject(obj, "action", gpio_action_kind_str((gpio_action_kind_t)a.action));
            cJSON_AddNumberToObject(obj, "restore_delay_ms", a.restore_delay_ms);
            cJSON_AddItemToArray(arr, obj);
        }
    }
    send_cjson(req, arr);
    return ESP_OK;
}

// GET /api/gpio/pins
static esp_err_t handle_gpio_pins_get(httpd_req_t *req)
{
    uint8_t gpios[16];
    int n = gpio_action_get_allowed_gpios(gpios, 16);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(gpios[i]));
    }
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

    cJSON *idx_item   = cJSON_GetObjectItem(root, "idx");
    cJSON *name_item  = cJSON_GetObjectItem(root, "name");
    cJSON *gpio_item  = cJSON_GetObjectItem(root, "gpio");
    cJSON *idle_item  = cJSON_GetObjectItem(root, "idle_on");
    cJSON *low_item   = cJSON_GetObjectItem(root, "active_low");
    cJSON *act_item   = cJSON_GetObjectItem(root, "action");
    cJSON *delay_item = cJSON_GetObjectItem(root, "restore_delay_ms");

    if (require_idx && !cJSON_IsNumber(idx_item)) {
        cJSON_Delete(root);
        send_error(req, "idx required");
        return ESP_FAIL;
    }
    if (!cJSON_IsString(name_item) || strlen(name_item->valuestring) == 0) {
        cJSON_Delete(root);
        send_error(req, "name required");
        return ESP_FAIL;
    }
    if (!cJSON_IsNumber(gpio_item)) {
        cJSON_Delete(root);
        send_error(req, "gpio required");
        return ESP_FAIL;
    }
    if (!cJSON_IsString(act_item)) {
        cJSON_Delete(root);
        send_error(req, "action required");
        return ESP_FAIL;
    }

    gpio_action_kind_t action_kind;
    if (!gpio_action_kind_parse(act_item->valuestring, &action_kind)) {
        cJSON_Delete(root);
        send_error(req, "action must be on, off, or toggle");
        return ESP_FAIL;
    }

    memset(out, 0, sizeof(*out));
    strlcpy(out->name, name_item->valuestring, sizeof(out->name));
    out->gpio_num = (uint8_t)gpio_item->valuedouble;
    out->idle_on = cJSON_IsTrue(idle_item);
    out->active_low = cJSON_IsTrue(low_item);
    out->action = (uint8_t)action_kind;
    out->restore_delay_ms = cJSON_IsNumber(delay_item) && delay_item->valuedouble > 0
                          ? (uint32_t)delay_item->valuedouble
                          : 0;
    if (idx_out) *idx_out = cJSON_IsNumber(idx_item) ? (int)idx_item->valuedouble : -1;

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
    ble_device_t devs[BLE_ACCESS_MAX_DEVICES];
    int n = ble_access_get_devices(devs, BLE_ACCESS_MAX_DEVICES);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        ble_device_t *d = &devs[i];
        ble_device_telemetry_t telemetry = {0};
        char mac_str[18];
        mac_to_str(d->mac, mac_str);
        ble_access_get_device_telemetry(d->mac, &telemetry);
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "mac",              mac_str);
        cJSON_AddStringToObject(obj, "label",            d->label);
        cJSON_AddBoolToObject  (obj, "enabled",          d->enabled);
        cJSON_AddBoolToObject  (obj, "key_import_error", ble_access_has_key_import_error(d->mac));
        cJSON_AddBoolToObject  (obj, "decrypt_error",    ble_access_has_decrypt_error(d->mac));
        cJSON_AddNumberToObject(obj, "single_press",     d->single_press);
        cJSON_AddNumberToObject(obj, "double_press",     d->double_press);
        cJSON_AddNumberToObject(obj, "triple_press",     d->triple_press);
        cJSON_AddNumberToObject(obj, "long_press",       d->long_press);
        cJSON_AddNumberToObject(obj, "gpio_single_press", d->gpio_single_press);
        cJSON_AddNumberToObject(obj, "gpio_double_press", d->gpio_double_press);
        cJSON_AddNumberToObject(obj, "gpio_triple_press", d->gpio_triple_press);
        cJSON_AddNumberToObject(obj, "gpio_long_press",   d->gpio_long_press);
        if (telemetry.has_battery_percent)
            cJSON_AddNumberToObject(obj, "battery_percent", telemetry.battery_percent);
        else
            cJSON_AddNullToObject(obj, "battery_percent");
        if (telemetry.has_last_button_event)
            cJSON_AddStringToObject(obj, "last_button_event",
                                    ble_button_event_str(telemetry.last_button_event));
        else
            cJSON_AddNullToObject(obj, "last_button_event");
        if (telemetry.has_last_seen)
            cJSON_AddNumberToObject(obj, "last_seen_age_s", telemetry.last_seen_age_s);
        else
            cJSON_AddNullToObject(obj, "last_seen_age_s");
        if (telemetry.has_last_button_event)
            cJSON_AddNumberToObject(obj, "last_button_event_age_s",
                                    telemetry.last_button_event_age_s);
        else
            cJSON_AddNullToObject(obj, "last_button_event_age_s");
        cJSON_AddItemToArray(arr, obj);
    }
    send_cjson(req, arr);
    return ESP_OK;
}

// GET /api/ble/register/status
static esp_err_t handle_ble_reg_status(httpd_req_t *req)
{
    cJSON *obj = cJSON_CreateObject();
    bool registering = ble_access_is_registering();
    cJSON_AddBoolToObject(obj, "registering", registering);
    uint8_t pending_mac[6];
    if (ble_access_has_pending_mac(pending_mac)) {
        char mac_str[18];
        mac_to_str(pending_mac, mac_str);
        cJSON_AddStringToObject(obj, "pending_mac", mac_str);
    } else {
        cJSON_AddNullToObject(obj, "pending_mac");
    }
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
    if (!mac_from_str(mac_item->valuestring, mac)) {
        cJSON_Delete(root);
        return send_error(req, "invalid mac");
    }
    if (!key_from_str(key_item->valuestring, key)) {
        cJSON_Delete(root);
        return send_error(req, "key must be 32 hex chars");
    }

    const char *label = cJSON_IsString(label_item) ? label_item->valuestring : "Device";
    esp_err_t err = ble_access_register_confirm(mac, key, label);
    cJSON_Delete(root);

    if (err == ESP_ERR_NO_MEM)      return send_error(req, "device list full");
    if (err == ESP_ERR_INVALID_ARG) return send_error(req, "mac already registered");
    if (err != ESP_OK)              return send_error(req, "registration failed");

    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

// PATCH /api/ble/device
// Event fields are integer bitmasks referencing MQTT or GPIO action slots.
static esp_err_t handle_ble_device_update(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof(body)) != ESP_OK)
        return send_error(req, "body too large");

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, "invalid json");

    cJSON *mac_item = cJSON_GetObjectItem(root, "mac");
    if (!cJSON_IsString(mac_item)) { cJSON_Delete(root); return send_error(req, "mac required"); }

    uint8_t mac[6];
    if (!mac_from_str(mac_item->valuestring, mac)) {
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
    cJSON *sp_item  = cJSON_GetObjectItem(root, "single_press");
    cJSON *dp_item  = cJSON_GetObjectItem(root, "double_press");
    cJSON *tp_item  = cJSON_GetObjectItem(root, "triple_press");
    cJSON *lp_item  = cJSON_GetObjectItem(root, "long_press");
    cJSON *gsp_item = cJSON_GetObjectItem(root, "gpio_single_press");
    cJSON *gdp_item = cJSON_GetObjectItem(root, "gpio_double_press");
    cJSON *gtp_item = cJSON_GetObjectItem(root, "gpio_triple_press");
    cJSON *glp_item = cJSON_GetObjectItem(root, "gpio_long_press");
    cJSON *key_item = cJSON_GetObjectItem(root, "key");

    if (cJSON_IsString(lbl_item)) strlcpy(current.label, lbl_item->valuestring, sizeof(current.label));
    if (cJSON_IsBool(en_item))    current.enabled      = cJSON_IsTrue(en_item);
    if (cJSON_IsNumber(sp_item))  current.single_press = (uint16_t)sp_item->valuedouble;
    if (cJSON_IsNumber(dp_item))  current.double_press = (uint16_t)dp_item->valuedouble;
    if (cJSON_IsNumber(tp_item))  current.triple_press = (uint16_t)tp_item->valuedouble;
    if (cJSON_IsNumber(lp_item))  current.long_press   = (uint16_t)lp_item->valuedouble;
    if (cJSON_IsNumber(gsp_item)) current.gpio_single_press = (uint16_t)gsp_item->valuedouble;
    if (cJSON_IsNumber(gdp_item)) current.gpio_double_press = (uint16_t)gdp_item->valuedouble;
    if (cJSON_IsNumber(gtp_item)) current.gpio_triple_press = (uint16_t)gtp_item->valuedouble;
    if (cJSON_IsNumber(glp_item)) current.gpio_long_press   = (uint16_t)glp_item->valuedouble;

    if (cJSON_IsString(key_item) &&
            strlen(key_item->valuestring) > 0 &&
            !key_from_str(key_item->valuestring, NULL)) {
        cJSON_Delete(root);
        return send_error(req, "key must be 32 hex chars");
    }

    // Optional key update: present and non-empty = replace, absent or empty = keep
    uint8_t new_key[16];
    bool has_new_key = cJSON_IsString(key_item) &&
                       strlen(key_item->valuestring) > 0 &&
                       key_from_str(key_item->valuestring, new_key);
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
    if (!mac_from_str(mac_item->valuestring, mac)) {
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
    if (!mac_from_str(mac_item->valuestring, mac)) {
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
    github_release_info_t release;
    esp_err_t err = github_fetch_latest_release(&release);
    if (err == ESP_ERR_NOT_FOUND) return send_error(req, "latest release is missing BluButtonBridge.bin or its sha256 digest");
    if (err != ESP_OK) return send_error(req, "could not fetch latest GitHub release");

    char current_version[40];
    format_version_label(app->version, current_version, sizeof(current_version));

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", true);
    cJSON_AddStringToObject(obj, "current_version", current_version);
    cJSON_AddStringToObject(obj, "latest_version", release.version_label);
    cJSON_AddBoolToObject(obj, "update_available", compare_versions_for_update(release.tag, app->version) > 0);
    cJSON_AddStringToObject(obj, "release_url", release.html_url);
    cJSON_AddStringToObject(obj, "asset_name", GITHUB_ASSET_NAME);
    cJSON_AddNumberToObject(obj, "asset_size", release.asset_size);
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

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) { ota_unlock(); return send_error(req, "no OTA partition"); }
    if ((size_t)req->content_len > part->size) {
        ota_unlock();
        return send_error_status(req, "413 Payload Too Large",
                                 "firmware image too large for OTA partition");
    }

    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) { ota_unlock(); return send_error(req, "OTA begin failed"); }

    char buf[1024];
    int remaining = req->content_len;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf));
        if (n <= 0) {
            esp_ota_abort(ota);
            ota_unlock();
            return send_error(req, "receive error");
        }
        err = esp_ota_write(ota, buf, n);
        if (err != ESP_OK) {
            esp_ota_abort(ota);
            ota_unlock();
            return send_error(req, "OTA write failed");
        }
        remaining -= n;
    }

    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        ota_unlock();
        return send_error_status(req, "422 Unprocessable Entity", "OTA validation failed");
    }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ota_unlock();
        return send_error(req, "set boot partition failed");
    }

    ota_unlock();
    send_json(req, "{\"ok\":true}");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// POST /api/system/update
static esp_err_t handle_update_install(httpd_req_t *req)
{
    (void)req;
    if (!ota_try_lock()) return send_error(req, "another OTA operation is already in progress");

    const esp_app_desc_t *app = esp_app_get_description();
    github_release_info_t release;
    esp_err_t err = github_fetch_latest_release(&release);
    if (err == ESP_ERR_NOT_FOUND) {
        ota_unlock();
        return send_error(req, "latest release is missing BluButtonBridge.bin or its sha256 digest");
    }
    if (err != ESP_OK) {
        ota_unlock();
        return send_error(req, "could not fetch latest GitHub release");
    }

    if (compare_versions_for_update(release.tag, app->version) <= 0) {
        ota_unlock();
        return send_error(req, "no newer GitHub release is available");
    }

    ESP_LOGI(TAG, "Installing GitHub release %s from %s", release.version_label, release.download_url);
    err = ota_install_from_github_release(&release);
    ota_unlock();

    if (err == ESP_ERR_INVALID_CRC) return send_error(req, "firmware digest verification failed");
    if (err != ESP_OK) return send_error(req, "GitHub OTA download failed");

    send_json(req, "{\"ok\":true}");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// ── Config backup / restore ──────────────────────────────────────────────────

static uint16_t json_u16(cJSON *obj, const char *key)
{
    cJSON *v = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(v) ? (uint16_t)v->valuedouble : 0;
}

// GET /api/system/config
static esp_err_t handle_config_download(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 2);

    // WiFi STA
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    {
        char ssid[33] = {0}, pass[65] = {0};
        nvs_handle_t h;
        if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
            size_t len = sizeof(ssid);
            nvs_get_str(h, "ssid", ssid, &len);
            len = sizeof(pass);
            nvs_get_str(h, "pass", pass, &len);
            nvs_close(h);
        }
        cJSON_AddStringToObject(wifi, "ssid", ssid);
        cJSON_AddStringToObject(wifi, "password", pass);
    }

    // AP
    cJSON *ap = cJSON_AddObjectToObject(root, "ap");
    {
        wifi_ap_settings_t cfg;
        wifi_ap_load_config(&cfg);
        cJSON_AddBoolToObject(ap, "enabled", cfg.enabled);
        cJSON_AddStringToObject(ap, "ssid", cfg.ssid);
        cJSON_AddStringToObject(ap, "password", cfg.password);
    }

    // HTTP Basic Auth
    cJSON *auth = cJSON_AddObjectToObject(root, "auth");
    {
        auth_config_t cfg = {0};
        if (s_auth_mutex) xSemaphoreTake(s_auth_mutex, portMAX_DELAY);
        cfg = s_auth_cfg;
        if (s_auth_mutex) xSemaphoreGive(s_auth_mutex);
        cJSON_AddBoolToObject(auth, "enabled", cfg.enabled);
        cJSON_AddStringToObject(auth, "username", cfg.username);
        cJSON_AddBoolToObject(auth, "password_set", cfg.password_set);
        cJSON_AddStringToObject(auth, "password_sha256", cfg.password_sha256);
    }

    // MQTT broker
    cJSON *mqtt = cJSON_AddObjectToObject(root, "mqtt");
    {
        char host[128] = {0}, user[64] = {0}, pass[64] = {0};
        char port_s[8] = {0}, tls_s[4] = {0};
        nvs_handle_t h;
        if (nvs_open("mqtt", NVS_READONLY, &h) == ESP_OK) {
            size_t len;
            len = sizeof(host);  nvs_get_str(h, "host", host, &len);
            len = sizeof(port_s); nvs_get_str(h, "port", port_s, &len);
            len = sizeof(user);  nvs_get_str(h, "user", user, &len);
            len = sizeof(pass);  nvs_get_str(h, "pass", pass, &len);
            len = sizeof(tls_s); nvs_get_str(h, "tls", tls_s, &len);
            nvs_close(h);
        }
        cJSON_AddStringToObject(mqtt, "host", host);
        cJSON_AddNumberToObject(mqtt, "port", atoi(port_s));
        cJSON_AddStringToObject(mqtt, "username", user);
        cJSON_AddStringToObject(mqtt, "password", pass);
        cJSON_AddBoolToObject(mqtt, "tls", strcmp(tls_s, "1") == 0);
    }

    // MQTT actions
    cJSON *ma = cJSON_AddArrayToObject(root, "mqtt_actions");
    for (int i = 0; i < MQTT_MAX_ACTIONS; i++) {
        mqtt_action_t a;
        if (mqtt_action_get(i, &a) == ESP_OK) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "idx", i);
            cJSON_AddStringToObject(o, "name", a.name);
            cJSON_AddStringToObject(o, "topic", a.topic);
            cJSON_AddStringToObject(o, "payload", a.payload);
            cJSON_AddItemToArray(ma, o);
        }
    }

    // GPIO actions
    cJSON *ga = cJSON_AddArrayToObject(root, "gpio_actions");
    for (int i = 0; i < GPIO_ACTION_MAX; i++) {
        gpio_action_t a;
        if (gpio_action_get(i, &a) == ESP_OK) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "idx", i);
            cJSON_AddStringToObject(o, "name", a.name);
            cJSON_AddNumberToObject(o, "gpio", a.gpio_num);
            cJSON_AddBoolToObject(o, "idle_on", a.idle_on);
            cJSON_AddBoolToObject(o, "active_low", a.active_low);
            cJSON_AddStringToObject(o, "action", gpio_action_kind_str((gpio_action_kind_t)a.action));
            cJSON_AddNumberToObject(o, "restore_delay_ms", a.restore_delay_ms);
            cJSON_AddItemToArray(ga, o);
        }
    }

    // BLE devices
    cJSON *ba = cJSON_AddArrayToObject(root, "ble_devices");
    {
        ble_device_t devs[BLE_ACCESS_MAX_DEVICES];
        int n = ble_access_get_devices(devs, BLE_ACCESS_MAX_DEVICES);
        for (int i = 0; i < n; i++) {
            cJSON *o = cJSON_CreateObject();
            char ms[18];
            mac_to_str(devs[i].mac, ms);
            cJSON_AddStringToObject(o, "mac", ms);
            char kh[33];
            for (int k = 0; k < 16; k++) snprintf(kh + k * 2, 3, "%02X", devs[i].key[k]);
            cJSON_AddStringToObject(o, "key", kh);
            cJSON_AddStringToObject(o, "label", devs[i].label);
            cJSON_AddBoolToObject(o, "enabled", devs[i].enabled);
            cJSON_AddNumberToObject(o, "last_counter", devs[i].last_counter);
            cJSON_AddNumberToObject(o, "single_press", devs[i].single_press);
            cJSON_AddNumberToObject(o, "double_press", devs[i].double_press);
            cJSON_AddNumberToObject(o, "triple_press", devs[i].triple_press);
            cJSON_AddNumberToObject(o, "long_press", devs[i].long_press);
            cJSON_AddNumberToObject(o, "gpio_single_press", devs[i].gpio_single_press);
            cJSON_AddNumberToObject(o, "gpio_double_press", devs[i].gpio_double_press);
            cJSON_AddNumberToObject(o, "gpio_triple_press", devs[i].gpio_triple_press);
            cJSON_AddNumberToObject(o, "gpio_long_press", devs[i].gpio_long_press);
            cJSON_AddItemToArray(ba, o);
        }
    }

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
    if (req->content_len <= 0 || req->content_len > 8192)
        return send_error(req, "body too large (max 8 KB)");

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

    // WiFi STA
    cJSON *wifi = cJSON_GetObjectItem(root, "wifi");
    if (wifi) {
        nvs_handle_t h;
        if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
            cJSON *s = cJSON_GetObjectItem(wifi, "ssid");
            cJSON *p = cJSON_GetObjectItem(wifi, "password");
            if (cJSON_IsString(s) && strlen(s->valuestring)) nvs_set_str(h, "ssid", s->valuestring);
            if (cJSON_IsString(p) && strlen(p->valuestring)) nvs_set_str(h, "pass", p->valuestring);
            nvs_commit(h);
            nvs_close(h);
        }
    }

    // AP
    cJSON *ap = cJSON_GetObjectItem(root, "ap");
    if (ap) {
        wifi_ap_settings_t cfg;
        wifi_ap_load_config(&cfg);
        cJSON *en = cJSON_GetObjectItem(ap, "enabled");
        cJSON *s  = cJSON_GetObjectItem(ap, "ssid");
        cJSON *p  = cJSON_GetObjectItem(ap, "password");
        if (cJSON_IsBool(en))  cfg.enabled = cJSON_IsTrue(en);
        if (cJSON_IsString(s)) strlcpy(cfg.ssid, s->valuestring, sizeof(cfg.ssid));
        if (cJSON_IsString(p)) strlcpy(cfg.password, p->valuestring, sizeof(cfg.password));
        wifi_ap_save_config(&cfg);
    }

    // HTTP Basic Auth
    cJSON *auth = cJSON_GetObjectItem(root, "auth");
    if (auth) {
        auth_config_t next = {0};
        xSemaphoreTake(s_auth_mutex, portMAX_DELAY);
        next = s_auth_cfg;
        xSemaphoreGive(s_auth_mutex);

        cJSON *en = cJSON_GetObjectItem(auth, "enabled");
        cJSON *u  = cJSON_GetObjectItem(auth, "username");
        cJSON *ps = cJSON_GetObjectItem(auth, "password_set");
        cJSON *ph = cJSON_GetObjectItem(auth, "password_sha256");

        if (cJSON_IsBool(en)) next.enabled = cJSON_IsTrue(en);
        if (cJSON_IsString(u)) {
            if (strlen(u->valuestring) > AUTH_USER_MAX || !auth_username_is_valid(u->valuestring)) {
                cJSON_Delete(root);
                return send_error(req, "invalid auth username in backup");
            }
            strlcpy(next.username, u->valuestring, sizeof(next.username));
        }
        if (cJSON_IsString(ph)) {
            if (ph->valuestring[0] == '\0') {
                next.password_sha256[0] = '\0';
                next.password_set = false;
            } else if (!auth_password_hash_copy(ph->valuestring, next.password_sha256)) {
                cJSON_Delete(root);
                return send_error(req, "invalid auth password hash in backup");
            } else {
                next.password_set = true;
            }
        } else if (cJSON_IsBool(ps) && !cJSON_IsTrue(ps)) {
            next.password_sha256[0] = '\0';
            next.password_set = false;
        }

        if (next.enabled && next.username[0] == '\0') {
            cJSON_Delete(root);
            return send_error(req, "backup enables auth without a username");
        }
        if (next.enabled && !next.password_set) {
            cJSON_Delete(root);
            return send_error(req, "backup enables auth without a password hash");
        }

        xSemaphoreTake(s_auth_mutex, portMAX_DELAY);
        esp_err_t err = auth_save_config_locked(&next);
        if (err == ESP_OK) s_auth_cfg = next;
        xSemaphoreGive(s_auth_mutex);
        if (err != ESP_OK) {
            cJSON_Delete(root);
            return send_error(req, "could not restore auth config");
        }
    }

    // MQTT broker
    cJSON *mqtt = cJSON_GetObjectItem(root, "mqtt");
    if (mqtt) {
        nvs_handle_t h;
        if (nvs_open("mqtt", NVS_READWRITE, &h) == ESP_OK) {
            cJSON *host = cJSON_GetObjectItem(mqtt, "host");
            cJSON *port = cJSON_GetObjectItem(mqtt, "port");
            cJSON *user = cJSON_GetObjectItem(mqtt, "username");
            cJSON *pass = cJSON_GetObjectItem(mqtt, "password");
            cJSON *tls  = cJSON_GetObjectItem(mqtt, "tls");
            if (cJSON_IsString(host)) nvs_set_str(h, "host", host->valuestring);
            if (cJSON_IsNumber(port)) {
                char ps[8]; snprintf(ps, sizeof(ps), "%d", (int)port->valuedouble);
                nvs_set_str(h, "port", ps);
            }
            if (cJSON_IsString(user)) nvs_set_str(h, "user", user->valuestring);
            if (cJSON_IsString(pass)) nvs_set_str(h, "pass", pass->valuestring);
            if (cJSON_IsBool(tls))    nvs_set_str(h, "tls", cJSON_IsTrue(tls) ? "1" : "0");
            nvs_commit(h);
            nvs_close(h);
        }
    }

    // MQTT actions
    cJSON *ma = cJSON_GetObjectItem(root, "mqtt_actions");
    if (ma && cJSON_IsArray(ma)) {
        mqtt_action_t actions[MQTT_MAX_ACTIONS];
        memset(actions, 0, sizeof(actions));
        cJSON *item;
        cJSON_ArrayForEach(item, ma) {
            int idx = cJSON_IsNumber(cJSON_GetObjectItem(item, "idx"))
                      ? (int)cJSON_GetObjectItem(item, "idx")->valuedouble : -1;
            if (idx < 0 || idx >= MQTT_MAX_ACTIONS) continue;
            cJSON *n = cJSON_GetObjectItem(item, "name");
            cJSON *t = cJSON_GetObjectItem(item, "topic");
            cJSON *p = cJSON_GetObjectItem(item, "payload");
            if (cJSON_IsString(n)) strlcpy(actions[idx].name, n->valuestring, sizeof(actions[idx].name));
            if (cJSON_IsString(t)) strlcpy(actions[idx].topic, t->valuestring, sizeof(actions[idx].topic));
            if (cJSON_IsString(p)) strlcpy(actions[idx].payload, p->valuestring, sizeof(actions[idx].payload));
        }
        nvs_handle_t h;
        if (nvs_open("mqtt", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_blob(h, "actions", actions, sizeof(actions));
            nvs_commit(h); nvs_close(h);
        }
    }

    // GPIO actions
    cJSON *ga = cJSON_GetObjectItem(root, "gpio_actions");
    if (ga && cJSON_IsArray(ga)) {
        gpio_action_t actions[GPIO_ACTION_MAX];
        memset(actions, 0, sizeof(actions));
        cJSON *item;
        cJSON_ArrayForEach(item, ga) {
            int idx = cJSON_IsNumber(cJSON_GetObjectItem(item, "idx"))
                      ? (int)cJSON_GetObjectItem(item, "idx")->valuedouble : -1;
            if (idx < 0 || idx >= GPIO_ACTION_MAX) continue;
            cJSON *n = cJSON_GetObjectItem(item, "name");
            if (!cJSON_IsString(n)) continue;
            strlcpy(actions[idx].name, n->valuestring, sizeof(actions[idx].name));
            cJSON *g  = cJSON_GetObjectItem(item, "gpio");
            cJSON *il = cJSON_GetObjectItem(item, "idle_on");
            cJSON *al = cJSON_GetObjectItem(item, "active_low");
            cJSON *ak = cJSON_GetObjectItem(item, "action");
            cJSON *rd = cJSON_GetObjectItem(item, "restore_delay_ms");
            if (cJSON_IsNumber(g))  actions[idx].gpio_num = (uint8_t)g->valuedouble;
            if (cJSON_IsBool(il))   actions[idx].idle_on = cJSON_IsTrue(il);
            if (cJSON_IsBool(al))   actions[idx].active_low = cJSON_IsTrue(al);
            if (cJSON_IsString(ak)) {
                gpio_action_kind_t kind;
                if (gpio_action_kind_parse(ak->valuestring, &kind))
                    actions[idx].action = (uint8_t)kind;
            }
            if (cJSON_IsNumber(rd)) actions[idx].restore_delay_ms = (uint32_t)rd->valuedouble;
        }
        nvs_handle_t h;
        if (nvs_open("gpio", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_blob(h, "actions", actions, sizeof(actions));
            nvs_commit(h); nvs_close(h);
        }
    }

    // BLE devices
    cJSON *ba = cJSON_GetObjectItem(root, "ble_devices");
    if (ba && cJSON_IsArray(ba)) {
        ble_device_t devs[BLE_ACCESS_MAX_DEVICES];
        memset(devs, 0, sizeof(devs));
        int count = 0;
        cJSON *item;
        cJSON_ArrayForEach(item, ba) {
            if (count >= BLE_ACCESS_MAX_DEVICES) break;
            cJSON *mj = cJSON_GetObjectItem(item, "mac");
            cJSON *kj = cJSON_GetObjectItem(item, "key");
            if (!cJSON_IsString(mj) || !cJSON_IsString(kj)) continue;
            ble_device_t *d = &devs[count];
            if (!mac_from_str(mj->valuestring, d->mac)) continue;
            if (!key_from_str(kj->valuestring, d->key)) continue;
            cJSON *lj = cJSON_GetObjectItem(item, "label");
            strlcpy(d->label, cJSON_IsString(lj) ? lj->valuestring : "Device", sizeof(d->label));
            cJSON *ej = cJSON_GetObjectItem(item, "enabled");
            d->enabled = cJSON_IsBool(ej) ? cJSON_IsTrue(ej) : true;
            cJSON *cj = cJSON_GetObjectItem(item, "last_counter");
            if (cJSON_IsNumber(cj)) d->last_counter = (uint32_t)cj->valuedouble;
            d->single_press       = json_u16(item, "single_press");
            d->double_press       = json_u16(item, "double_press");
            d->triple_press       = json_u16(item, "triple_press");
            d->long_press         = json_u16(item, "long_press");
            d->gpio_single_press  = json_u16(item, "gpio_single_press");
            d->gpio_double_press  = json_u16(item, "gpio_double_press");
            d->gpio_triple_press  = json_u16(item, "gpio_triple_press");
            d->gpio_long_press    = json_u16(item, "gpio_long_press");
            count++;
        }
        nvs_handle_t h;
        if (nvs_open("ble_access", NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_set_u8(h, "count", (uint8_t)count);
            for (int i = 0; i < count; i++) {
                char key[16];
                snprintf(key, sizeof(key), "dev_%d", i);
                nvs_set_blob(h, key, &devs[i], sizeof(ble_device_t));
            }
            nvs_commit(h); nvs_close(h);
        }
    }

    cJSON_Delete(root);
    send_json(req, "{\"ok\":true}");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// ── Init ─────────────────────────────────────────────────────────────────────

void web_manager_init(void)
{
    s_auth_mutex = xSemaphoreCreateMutex();
    if (!s_auth_mutex) {
        ESP_LOGE(TAG, "Failed to allocate auth mutex");
        return;
    }
    auth_load_config();
    if (!s_ota_mutex) s_ota_mutex = xSemaphoreCreateMutex();
    if (!s_ota_mutex) {
        ESP_LOGE(TAG, "Failed to create OTA mutex");
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size        = 10240;
    config.max_uri_handlers  = 44;
    config.max_open_sockets  = 4;
    config.lru_purge_enable  = true;
    config.uri_match_fn      = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    static route_ctx_t route_ctxs[46];
    const route_def_t routes[] = {
        { .uri = "/",                         .method = HTTP_GET,    .handler = handle_root,                .auth_required = true },
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

    for (int i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
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
