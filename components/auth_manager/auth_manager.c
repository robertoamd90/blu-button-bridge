#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "auth_manager.h"

static const char *AUTH_NS = "http_auth";
static const char *TAG = "auth_manager";
#define AUTH_HASH_HEX_LEN 65

typedef struct {
    bool enabled;
    char username[AUTH_MANAGER_USERNAME_MAX + 1];
    char password_sha256[AUTH_HASH_HEX_LEN];
} auth_config_t;

static auth_config_t s_auth_cfg = {0};
static SemaphoreHandle_t s_auth_mutex = NULL;

static bool auth_config_has_password(const auth_config_t *cfg)
{
    return cfg && strlen(cfg->password_sha256) == AUTH_HASH_HEX_LEN - 1;
}

static void bytes_to_hex(const uint8_t *in, size_t len, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = hex[(in[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[in[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

static bool sha256_hex(const char *input, char out[AUTH_HASH_HEX_LEN])
{
    uint8_t digest[32];
    if (mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                   (const unsigned char *)input, strlen(input), digest) != 0) {
        return false;
    }
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
    if (nvs_get_str(nvs, "enabled", enabled, &len) == ESP_OK) {
        s_auth_cfg.enabled = (enabled[0] == '1');
    }

    len = sizeof(s_auth_cfg.username);
    nvs_get_str(nvs, "user", s_auth_cfg.username, &len);

    len = sizeof(s_auth_cfg.password_sha256);
    nvs_get_str(nvs, "pass_sha", s_auth_cfg.password_sha256, &len);
    nvs_close(nvs);
}

static void auth_snapshot_config(auth_config_t *out)
{
    if (!out) return;

    memset(out, 0, sizeof(*out));
    if (!s_auth_mutex) return;

    xSemaphoreTake(s_auth_mutex, portMAX_DELAY);
    *out = s_auth_cfg;
    xSemaphoreGive(s_auth_mutex);
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

static esp_err_t auth_validate_candidate(const auth_config_t *next, const char **out_error)
{
    if (!next) return ESP_ERR_INVALID_ARG;
    if (out_error) *out_error = NULL;

    if (next->enabled && next->username[0] == '\0') {
        if (out_error) *out_error = "username required when auth is enabled";
        return ESP_ERR_INVALID_ARG;
    }
    if (next->enabled && !auth_username_is_valid(next->username)) {
        if (out_error) *out_error = "username cannot contain ':'";
        return ESP_ERR_INVALID_ARG;
    }
    if (next->enabled && !auth_config_has_password(next)) {
        if (out_error) *out_error = "password required when auth is enabled";
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t auth_apply_config_candidate(const auth_config_t *next, const char **out_error)
{
    esp_err_t err = auth_validate_candidate(next, out_error);
    if (err != ESP_OK) return err;
    if (!s_auth_mutex) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_auth_mutex, portMAX_DELAY);
    err = auth_save_config_locked(next);
    if (err == ESP_OK) s_auth_cfg = *next;
    xSemaphoreGive(s_auth_mutex);

    if (err != ESP_OK && out_error) *out_error = "could not save auth config";
    return err;
}

static bool send_auth_challenge(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"BluButtonBridge\"");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Authentication required");
    return false;
}

void auth_manager_init(void)
{
    if (s_auth_mutex) return;

    s_auth_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_auth_mutex ? ESP_OK : ESP_ERR_NO_MEM);

    auth_load_config();
}

bool auth_manager_require(httpd_req_t *req)
{
    if (!req) return false;
    if (!s_auth_mutex) {
        ESP_LOGE(TAG, "Auth manager unavailable");
        return send_auth_challenge(req);
    }

    auth_config_t cfg;
    auth_snapshot_config(&cfg);

    if (!cfg.enabled) return true;
    if (!auth_config_has_password(&cfg) || cfg.username[0] == '\0') return send_auth_challenge(req);

    size_t hdr_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hdr_len == 0) return send_auth_challenge(req);

    char header[160];
    if (hdr_len >= sizeof(header)) return send_auth_challenge(req);
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK) {
        return send_auth_challenge(req);
    }
    if (strncmp(header, "Basic ", 6) != 0) return send_auth_challenge(req);

    unsigned char decoded[AUTH_MANAGER_USERNAME_MAX + AUTH_MANAGER_PASSWORD_MAX + 4];
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

cJSON *auth_manager_config_export(void)
{
    if (!s_auth_mutex) return NULL;

    auth_config_t cfg;
    auth_snapshot_config(&cfg);

    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;
    if (!cJSON_AddBoolToObject(obj, "enabled", cfg.enabled) ||
        !cJSON_AddStringToObject(obj, "username", cfg.username) ||
        !cJSON_AddBoolToObject(obj, "password_set", auth_config_has_password(&cfg))) {
        cJSON_Delete(obj);
        return NULL;
    }
    return obj;
}

esp_err_t auth_manager_config_update_from_json(const cJSON *root_obj, const char **out_error)
{
    if (out_error) *out_error = NULL;
    if (!root_obj) {
        if (out_error) *out_error = "invalid json";
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = (cJSON *)root_obj;
    cJSON *enabled_item = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    cJSON *user_item = cJSON_GetObjectItemCaseSensitive(root, "username");
    cJSON *pass_item = cJSON_GetObjectItemCaseSensitive(root, "password");

    auth_config_t next;
    auth_snapshot_config(&next);

    if (cJSON_IsBool(enabled_item)) next.enabled = cJSON_IsTrue(enabled_item);
    if (cJSON_IsString(user_item)) {
        if (strlen(user_item->valuestring) > AUTH_MANAGER_USERNAME_MAX) {
            if (out_error) *out_error = "username too long";
            return ESP_ERR_INVALID_ARG;
        }
        if (!auth_username_is_valid(user_item->valuestring)) {
            if (out_error) *out_error = "username cannot contain ':'";
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(next.username, user_item->valuestring, sizeof(next.username));
    }
    if (cJSON_IsString(pass_item)) {
        if (strlen(pass_item->valuestring) > AUTH_MANAGER_PASSWORD_MAX) {
            if (out_error) *out_error = "password too long";
            return ESP_ERR_INVALID_ARG;
        }
        if (pass_item->valuestring[0] == '\0') {
            next.password_sha256[0] = '\0';
        } else if (!sha256_hex(pass_item->valuestring, next.password_sha256)) {
            if (out_error) *out_error = "password hashing failed";
            return ESP_ERR_INVALID_ARG;
        }
    }

    return auth_apply_config_candidate(&next, out_error);
}

bool auth_manager_backup_export(cJSON *root)
{
    if (!root || !s_auth_mutex) return false;

    cJSON *auth = cJSON_AddObjectToObject(root, "auth");
    if (!auth) return false;

    auth_config_t cfg;
    auth_snapshot_config(&cfg);
    if (!cJSON_AddBoolToObject(auth, "enabled", cfg.enabled) ||
        !cJSON_AddStringToObject(auth, "username", cfg.username) ||
        !cJSON_AddStringToObject(auth, "password_sha256", cfg.password_sha256)) {
        return false;
    }
    return true;
}

esp_err_t auth_manager_backup_import(const cJSON *root_obj, const char **out_error)
{
    if (out_error) *out_error = NULL;
    if (!root_obj) return ESP_ERR_INVALID_ARG;

    cJSON *root = (cJSON *)root_obj;
    cJSON *auth = cJSON_GetObjectItemCaseSensitive(root, "auth");
    if (!auth) return ESP_OK;

    auth_config_t next;
    auth_snapshot_config(&next);

    cJSON *enabled_item = cJSON_GetObjectItemCaseSensitive(auth, "enabled");
    cJSON *user_item = cJSON_GetObjectItemCaseSensitive(auth, "username");
    cJSON *password_hash_item = cJSON_GetObjectItemCaseSensitive(auth, "password_sha256");

    if (cJSON_IsBool(enabled_item)) next.enabled = cJSON_IsTrue(enabled_item);
    if (cJSON_IsString(user_item)) {
        if (strlen(user_item->valuestring) > AUTH_MANAGER_USERNAME_MAX ||
            !auth_username_is_valid(user_item->valuestring)) {
            if (out_error) *out_error = "invalid auth username in backup";
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(next.username, user_item->valuestring, sizeof(next.username));
    }
    if (cJSON_IsString(password_hash_item)) {
        if (password_hash_item->valuestring[0] == '\0') {
            next.password_sha256[0] = '\0';
        } else if (!auth_password_hash_copy(password_hash_item->valuestring, next.password_sha256)) {
            if (out_error) *out_error = "invalid auth password hash in backup";
            return ESP_ERR_INVALID_ARG;
        }
    }

    return auth_apply_config_candidate(&next, out_error);
}
