#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "mbedtls/md.h"
#include "nvs.h"
#include "esp_log.h"
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

typedef enum {
    AUTH_SECRET_NONE = 0,
    AUTH_SECRET_PLAINTEXT,
    AUTH_SECRET_SHA256,
} auth_secret_kind_t;

typedef struct {
    const cJSON *enabled_item;
    const cJSON *username_item;
    const cJSON *secret_item;
    auth_secret_kind_t secret_kind;
    const char *username_too_long_error;
    const char *username_invalid_error;
    const char *secret_too_long_error;
    const char *secret_invalid_error;
} auth_candidate_input_t;

static auth_config_t s_auth_cfg = {0};
static SemaphoreHandle_t s_auth_mutex = NULL;
static esp_err_t s_auth_status = ESP_ERR_INVALID_STATE;

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

static esp_err_t auth_load_optional_str(nvs_handle_t nvs,
                                        const char *key,
                                        char *out,
                                        size_t out_len)
{
    size_t len = out_len;
    esp_err_t err = nvs_get_str(nvs, key, out, &len);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    return err;
}

static esp_err_t auth_load_config(void)
{
    memset(&s_auth_cfg, 0, sizeof(s_auth_cfg));

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(AUTH_NS, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    char enabled[4] = {0};
    size_t len = sizeof(enabled);
    err = nvs_get_str(nvs, "enabled", enabled, &len);
    if (err == ESP_OK) {
        s_auth_cfg.enabled = (enabled[0] == '1');
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs);
        return err;
    }

    err = auth_load_optional_str(nvs, "user", s_auth_cfg.username, sizeof(s_auth_cfg.username));
    if (err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }

    err = auth_load_optional_str(nvs, "pass_sha", s_auth_cfg.password_sha256,
                                 sizeof(s_auth_cfg.password_sha256));
    nvs_close(nvs);
    return err;
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
    if (auth_manager_status() != ESP_OK || !s_auth_mutex) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_auth_mutex, portMAX_DELAY);
    err = auth_save_config_locked(next);
    if (err == ESP_OK) s_auth_cfg = *next;
    xSemaphoreGive(s_auth_mutex);

    if (err != ESP_OK && out_error) *out_error = "could not save auth config";
    return err;
}

static esp_err_t auth_candidate_from_input(const auth_candidate_input_t *input,
                                           auth_config_t *next,
                                           const char **out_error)
{
    if (!input || !next) return ESP_ERR_INVALID_ARG;

    auth_snapshot_config(next);

    if (cJSON_IsBool(input->enabled_item)) next->enabled = cJSON_IsTrue(input->enabled_item);
    if (cJSON_IsString(input->username_item)) {
        if (strlen(input->username_item->valuestring) > AUTH_MANAGER_USERNAME_MAX) {
            if (out_error) *out_error = input->username_too_long_error;
            return ESP_ERR_INVALID_ARG;
        }
        if (!auth_username_is_valid(input->username_item->valuestring)) {
            if (out_error) *out_error = input->username_invalid_error;
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(next->username, input->username_item->valuestring, sizeof(next->username));
    }

    if (!cJSON_IsString(input->secret_item)) return ESP_OK;

    if (input->secret_kind == AUTH_SECRET_PLAINTEXT) {
        if (strlen(input->secret_item->valuestring) > AUTH_MANAGER_PASSWORD_MAX) {
            if (out_error) *out_error = input->secret_too_long_error;
            return ESP_ERR_INVALID_ARG;
        }
        if (input->secret_item->valuestring[0] == '\0') {
            next->password_sha256[0] = '\0';
            return ESP_OK;
        }
        if (!sha256_hex(input->secret_item->valuestring, next->password_sha256)) {
            if (out_error) *out_error = input->secret_invalid_error;
            return ESP_ERR_INVALID_ARG;
        }
        return ESP_OK;
    }

    if (input->secret_kind == AUTH_SECRET_SHA256) {
        if (input->secret_item->valuestring[0] == '\0') {
            next->password_sha256[0] = '\0';
            return ESP_OK;
        }
        if (!auth_password_hash_copy(input->secret_item->valuestring, next->password_sha256)) {
            if (out_error) *out_error = input->secret_invalid_error;
            return ESP_ERR_INVALID_ARG;
        }
    }

    return ESP_OK;
}

void auth_manager_init(void)
{
    if (s_auth_status == ESP_OK) return;

    if (!s_auth_mutex) {
        s_auth_mutex = xSemaphoreCreateMutex();
    }
    if (!s_auth_mutex) {
        s_auth_status = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "Failed to allocate auth mutex");
        return;
    }

    s_auth_status = auth_load_config();
    if (s_auth_status != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load auth config: %s", esp_err_to_name(s_auth_status));
        return;
    }
}

esp_err_t auth_manager_status(void)
{
    return s_auth_status;
}

bool auth_manager_is_enabled(void)
{
    if (auth_manager_status() != ESP_OK) return false;

    auth_config_t cfg;
    auth_snapshot_config(&cfg);
    return cfg.enabled;
}

bool auth_manager_verify_credentials(const char *username, const char *password)
{
    if (auth_manager_status() != ESP_OK || !username || !password) return false;

    auth_config_t cfg;
    auth_snapshot_config(&cfg);
    if (!cfg.enabled || !auth_config_has_password(&cfg) || cfg.username[0] == '\0') return false;
    if (strcmp(username, cfg.username) != 0) return false;

    char password_sha256[AUTH_HASH_HEX_LEN];
    if (!sha256_hex(password, password_sha256)) return false;
    if (strcmp(password_sha256, cfg.password_sha256) != 0) return false;

    return true;
}

cJSON *auth_manager_config_export(void)
{
    if (auth_manager_status() != ESP_OK || !s_auth_mutex) return NULL;

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
    auth_config_t next;
    auth_candidate_input_t input = {
        .enabled_item = cJSON_GetObjectItemCaseSensitive(root, "enabled"),
        .username_item = cJSON_GetObjectItemCaseSensitive(root, "username"),
        .secret_item = cJSON_GetObjectItemCaseSensitive(root, "password"),
        .secret_kind = AUTH_SECRET_PLAINTEXT,
        .username_too_long_error = "username too long",
        .username_invalid_error = "username cannot contain ':'",
        .secret_too_long_error = "password too long",
        .secret_invalid_error = "password hashing failed",
    };
    esp_err_t err = auth_candidate_from_input(&input, &next, out_error);
    if (err != ESP_OK) return err;

    return auth_apply_config_candidate(&next, out_error);
}

bool auth_manager_backup_export(cJSON *root)
{
    if (!root || auth_manager_status() != ESP_OK || !s_auth_mutex) return false;

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
    auth_candidate_input_t input = {
        .enabled_item = cJSON_GetObjectItemCaseSensitive(auth, "enabled"),
        .username_item = cJSON_GetObjectItemCaseSensitive(auth, "username"),
        .secret_item = cJSON_GetObjectItemCaseSensitive(auth, "password_sha256"),
        .secret_kind = AUTH_SECRET_SHA256,
        .username_too_long_error = "invalid auth username in backup",
        .username_invalid_error = "invalid auth username in backup",
        .secret_invalid_error = "invalid auth password hash in backup",
    };
    esp_err_t err = auth_candidate_from_input(&input, &next, out_error);
    if (err != ESP_OK) return err;

    return auth_apply_config_candidate(&next, out_error);
}
