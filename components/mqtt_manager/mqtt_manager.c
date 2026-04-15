#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/portmacro.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "esp_log.h"
#include "mqtt_manager.h"

#define MQTT_MAX_SUBS       8
#define MQTT_MAX_TOPIC_LEN  128
#define MQTT_MAX_PAYLOAD    1024

static const char *TAG = "mqtt_manager";

typedef struct {
    char              topic[MQTT_MAX_TOPIC_LEN];
    mqtt_message_cb_t cb;
} mqtt_sub_t;

static esp_mqtt_client_handle_t s_client = NULL;
static volatile mqtt_status_t   s_status = MQTT_STATUS_NOT_CONFIG;
static bool                     s_stopping = false;
static bool                     s_network_available = false;
static mqtt_sub_t               s_subs[MQTT_MAX_SUBS];
static int                      s_nsubs = 0;
static mqtt_status_cb_t         s_status_cb = NULL;
static SemaphoreHandle_t        s_op_mutex = NULL;
static SemaphoreHandle_t        s_actions_mutex = NULL;
static SemaphoreHandle_t        s_subs_mutex = NULL;
static StaticSemaphore_t        s_op_mutex_buf;
static StaticSemaphore_t        s_actions_mutex_buf;
static StaticSemaphore_t        s_subs_mutex_buf;
static portMUX_TYPE             s_init_lock = portMUX_INITIALIZER_UNLOCKED;

static bool ensure_runtime_state(void)
{
    if (s_op_mutex && s_actions_mutex && s_subs_mutex) return true;

    portENTER_CRITICAL(&s_init_lock);
    if (!s_op_mutex) {
        s_op_mutex = xSemaphoreCreateMutexStatic(&s_op_mutex_buf);
    }
    if (!s_actions_mutex) {
        s_actions_mutex = xSemaphoreCreateMutexStatic(&s_actions_mutex_buf);
    }
    if (!s_subs_mutex) {
        s_subs_mutex = xSemaphoreCreateMutexStatic(&s_subs_mutex_buf);
    }
    portEXIT_CRITICAL(&s_init_lock);
    return s_op_mutex != NULL && s_actions_mutex != NULL && s_subs_mutex != NULL;
}

static bool load_saved_password(char *pass, size_t pass_len)
{
    nvs_handle_t nvs;
    if (!pass || pass_len == 0) return false;
    if (nvs_open("mqtt", NVS_READONLY, &nvs) != ESP_OK) return false;

    pass[0] = '\0';
    bool ok = (nvs_get_str(nvs, "pass", pass, &pass_len) == ESP_OK && strlen(pass) > 0);
    nvs_close(nvs);
    return ok;
}

static void set_status(mqtt_status_t status)
{
    if (s_status == status) return;
    s_status = status;
    if (s_status_cb) {
        s_status_cb(status);
    }
}

static mqtt_status_t mqtt_error_status(void)
{
    return s_network_available ? MQTT_STATUS_ERROR : MQTT_STATUS_WAITING_NET;
}

// ── Event handler ──────────────────────────────────────────────────────────────

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t ev = event_data;

    if (s_stopping) return;

    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            set_status(MQTT_STATUS_UP);
            ESP_LOGI(TAG, "connected to broker");
            // Safe without s_op_mutex: destroy_client() blocks until the MQTT
            // task exits, so s_client cannot be freed while this callback runs.
            xSemaphoreTake(s_subs_mutex, portMAX_DELAY);
            for (int i = 0; i < s_nsubs; i++) {
                esp_mqtt_client_subscribe(s_client, s_subs[i].topic, 1);
            }
            xSemaphoreGive(s_subs_mutex);
            break;

        case MQTT_EVENT_DISCONNECTED:
            if (!s_network_available) {
                set_status(MQTT_STATUS_WAITING_NET);
            } else if (s_status != MQTT_STATUS_ERROR) {
                set_status(MQTT_STATUS_CONNECTING);
            }
            ESP_LOGW(TAG, "disconnected from broker");
            break;

        case MQTT_EVENT_DATA:
            if (ev->topic_len > MQTT_MAX_TOPIC_LEN ||
                ev->data_len  > MQTT_MAX_PAYLOAD) {
                ESP_LOGW(TAG, "message too large, skipped");
                break;
            }
            if (ev->topic_len > 0) {
                char payload[MQTT_MAX_PAYLOAD + 1];
                if (ev->data_len > 0) {
                    memcpy(payload, ev->data, ev->data_len);
                }
                payload[ev->data_len] = '\0';

                char topic[MQTT_MAX_TOPIC_LEN + 1];
                memcpy(topic, ev->topic, ev->topic_len);
                topic[ev->topic_len] = '\0';

                mqtt_message_cb_t cb = NULL;
                xSemaphoreTake(s_subs_mutex, portMAX_DELAY);
                for (int i = 0; i < s_nsubs; i++) {
                    if (strcmp(s_subs[i].topic, topic) == 0) {
                        cb = s_subs[i].cb;
                        break;
                    }
                }
                xSemaphoreGive(s_subs_mutex);
                if (cb) cb(topic, payload, ev->data_len);
            }
            break;

        case MQTT_EVENT_ERROR:
            set_status(mqtt_error_status());
            ESP_LOGE(TAG, "MQTT error");
            break;

        default:
            break;
    }
}

// ── Helpers ─────────────────────────────────────────────────────────────────────

static void destroy_client(void)
{
    if (s_client) {
        s_stopping = true;
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        s_stopping = false;
    }
}

static bool load_credentials(char *host, size_t host_len,
                              char *port_str, size_t port_len,
                              char *user, size_t user_len,
                              char *pass, size_t pass_len,
                              bool *use_tls)
{
    nvs_handle_t nvs;
    if (nvs_open("mqtt", NVS_READONLY, &nvs) != ESP_OK) return false;

    char tls_str[4] = {};
    size_t tls_len = sizeof(tls_str);

    host[0] = '\0';
    port_str[0] = '\0';
    user[0] = '\0';
    pass[0] = '\0';
    if (use_tls) *use_tls = false;

    bool ok = (nvs_get_str(nvs, "host", host, &host_len) == ESP_OK &&
               nvs_get_str(nvs, "port", port_str, &port_len) == ESP_OK &&
               strlen(host) > 0 &&
               strlen(port_str) > 0);

    if (ok) {
        nvs_get_str(nvs, "user", user, &user_len);
        nvs_get_str(nvs, "pass", pass, &pass_len);
    }

    if (ok && use_tls && nvs_get_str(nvs, "tls", tls_str, &tls_len) == ESP_OK) {
        *use_tls = (tls_str[0] == '1');
    }

    nvs_close(nvs);
    return ok;
}

static bool load_saved_config(char *host, size_t host_len,
                              uint32_t *port, char *user, size_t user_len,
                              char *pass, size_t pass_len, bool *use_tls)
{
    char port_str[8] = {};
    if (!load_credentials(host, host_len, port_str, sizeof(port_str),
                          user, user_len, pass, pass_len, use_tls)) {
        return false;
    }
    if (port) *port = (uint32_t)atoi(port_str);
    return true;
}

static esp_err_t snapshot_saved_config_locked(char *host, size_t host_len,
                                              char *port_str, size_t port_len,
                                              char *user, size_t user_len,
                                              char *pass, size_t pass_len,
                                              char *tls_str, size_t tls_len)
{
    if (host_len > 0) host[0] = '\0';
    if (port_len > 0) port_str[0] = '\0';
    if (user_len > 0) user[0] = '\0';
    if (pass_len > 0) pass[0] = '\0';
    if (tls_len > 0) tls_str[0] = '\0';

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("mqtt", NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    err = nvs_get_str(nvs, "host", host, &host_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, "port", port_str, &port_len);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, "user", user, &user_len);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, "pass", pass, &pass_len);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, "tls", tls_str, &tls_len);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }

    nvs_close(nvs);
    return err;
}

static esp_err_t save_config_snapshot_locked(const char *host,
                                             const char *port_str,
                                             const char *user,
                                             const char *pass,
                                             const char *tls_str)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("mqtt", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_str(nvs, "host", host ? host : "");
    if (err == ESP_OK) err = nvs_set_str(nvs, "port", port_str ? port_str : "");
    if (err == ESP_OK) err = nvs_set_str(nvs, "user", user ? user : "");
    if (err == ESP_OK) err = nvs_set_str(nvs, "pass", pass ? pass : "");
    if (err == ESP_OK) err = nvs_set_str(nvs, "tls", tls_str ? tls_str : "0");
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

// ── Public API ────────────────────────────────────────────────────────────────

static void mqtt_connect_broker_locked(const char *host, uint32_t port, const char *username,
                                       const char *password, bool use_tls)
{
    destroy_client();

    esp_mqtt_client_config_t cfg = {
        .broker.address.hostname      = host,
        .broker.address.port          = port,
        .broker.address.transport     = use_tls ? MQTT_TRANSPORT_OVER_SSL
                                                : MQTT_TRANSPORT_OVER_TCP,
        .broker.verification.skip_cert_common_name_check = use_tls,
        .credentials.username         = username,
        .credentials.authentication.password = password,
        .session.keepalive            = 15,
        .network.reconnect_timeout_ms = 2000,
        .network.timeout_ms           = 5000,
    };

    set_status(MQTT_STATUS_CONNECTING);
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        set_status(mqtt_error_status());
        ESP_LOGE(TAG, "client init failed");
        destroy_client();
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        set_status(mqtt_error_status());
        ESP_LOGE(TAG, "client start failed");
        destroy_client();
        return;
    }
}

void mqtt_connect_broker(const char *host, uint32_t port, const char *username,
               const char *password, bool use_tls)
{
    if (!ensure_runtime_state()) {
        set_status(mqtt_error_status());
        ESP_LOGE(TAG, "runtime init failed");
        return;
    }

    xSemaphoreTake(s_op_mutex, portMAX_DELAY);
    mqtt_connect_broker_locked(host, port, username, password, use_tls);
    xSemaphoreGive(s_op_mutex);
}

// ── MQTT Actions ──────────────────────────────────────────────────────────────

static mqtt_action_t s_actions[MQTT_MAX_ACTIONS];
static bool          s_actions_loaded = false;

static void actions_load_locked(void)
{
    nvs_handle_t h;
    if (nvs_open("mqtt", NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = sizeof(s_actions);
    // Ignore errors: if key missing, s_actions stays zeroed (all slots free)
    nvs_get_blob(h, "actions", s_actions, &sz);
    nvs_close(h);
}

static esp_err_t actions_save_blob(const mqtt_action_t actions[MQTT_MAX_ACTIONS])
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("mqtt", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, "actions", actions, sizeof(mqtt_action_t) * MQTT_MAX_ACTIONS);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t actions_save_locked(void)
{
    return actions_save_blob(s_actions);
}

static void ensure_actions_loaded_locked(void)
{
    if (s_actions_loaded) return;
    actions_load_locked();
    s_actions_loaded = true;
}

static void actions_snapshot_locked(mqtt_action_t out[MQTT_MAX_ACTIONS])
{
    ensure_actions_loaded_locked();
    memcpy(out, s_actions, sizeof(s_actions));
}

static cJSON *action_export_json(const mqtt_action_t *action, int idx)
{
    cJSON *item = cJSON_CreateObject();
    if (!item) return NULL;

    cJSON_AddNumberToObject(item, "idx", idx);
    cJSON_AddStringToObject(item, "name", action->name);
    cJSON_AddStringToObject(item, "topic", action->topic);
    cJSON_AddStringToObject(item, "payload", action->payload);
    return item;
}

static esp_err_t actions_import_locked(const mqtt_action_t actions[MQTT_MAX_ACTIONS])
{
    esp_err_t err = actions_save_blob(actions);
    if (err == ESP_OK) {
        memcpy(s_actions, actions, sizeof(s_actions));
        s_actions_loaded = true;
    }
    return err;
}

int mqtt_action_add(const mqtt_action_t *a)
{
    if (!ensure_runtime_state()) return -1;
    if (!a || a->name[0] == '\0') return -1;

    xSemaphoreTake(s_actions_mutex, portMAX_DELAY);
    for (int i = 0; i < MQTT_MAX_ACTIONS; i++) {
        if (s_actions[i].name[0] == '\0') {
            s_actions[i] = *a;
            if (actions_save_locked() == ESP_OK) {
                xSemaphoreGive(s_actions_mutex);
                return i;
            }
            memset(&s_actions[i], 0, sizeof(s_actions[i]));
            break;
        }
    }
    xSemaphoreGive(s_actions_mutex);
    return -1;
}

esp_err_t mqtt_action_update(int idx, const mqtt_action_t *a)
{
    if (!ensure_runtime_state()) return ESP_ERR_NO_MEM;
    if (idx < 0 || idx >= MQTT_MAX_ACTIONS) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_actions_mutex, portMAX_DELAY);
    if (s_actions[idx].name[0] == '\0') {
        xSemaphoreGive(s_actions_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    mqtt_action_t previous = s_actions[idx];
    s_actions[idx] = *a;
    esp_err_t err = actions_save_locked();
    if (err != ESP_OK) {
        s_actions[idx] = previous;
    }
    xSemaphoreGive(s_actions_mutex);
    return err;
}

esp_err_t mqtt_action_delete(int idx)
{
    if (!ensure_runtime_state()) return ESP_ERR_NO_MEM;
    if (idx < 0 || idx >= MQTT_MAX_ACTIONS) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_actions_mutex, portMAX_DELAY);
    if (s_actions[idx].name[0] == '\0') {
        xSemaphoreGive(s_actions_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    mqtt_action_t previous = s_actions[idx];
    memset(&s_actions[idx], 0, sizeof(mqtt_action_t));
    esp_err_t err = actions_save_locked();
    if (err != ESP_OK) {
        s_actions[idx] = previous;
    }
    xSemaphoreGive(s_actions_mutex);
    return err;
}

esp_err_t mqtt_action_get(int idx, mqtt_action_t *out)
{
    if (!ensure_runtime_state()) return ESP_ERR_NO_MEM;
    if (idx < 0 || idx >= MQTT_MAX_ACTIONS) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_actions_mutex, portMAX_DELAY);
    if (s_actions[idx].name[0] == '\0') {
        xSemaphoreGive(s_actions_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    *out = s_actions[idx];
    xSemaphoreGive(s_actions_mutex);
    return ESP_OK;
}

esp_err_t mqtt_action_trigger(int idx)
{
    if (!ensure_runtime_state()) return ESP_ERR_NO_MEM;
    if (idx < 0 || idx >= MQTT_MAX_ACTIONS) return ESP_ERR_INVALID_ARG;

    mqtt_action_t action = {0};
    xSemaphoreTake(s_actions_mutex, portMAX_DELAY);
    if (s_actions[idx].name[0] == '\0') {
        xSemaphoreGive(s_actions_mutex);
        return ESP_OK;   // deleted — skip silently
    }
    action = s_actions[idx];
    xSemaphoreGive(s_actions_mutex);

    if (mqtt_publish(action.topic, action.payload) < 0) {
        ESP_LOGW(TAG, "action '%s': publish failed", action.name);
    }
    return ESP_OK;
}

struct cJSON *mqtt_config_export(mqtt_export_view_t view)
{
    if (!ensure_runtime_state()) return NULL;

    cJSON *mqtt = cJSON_CreateObject();
    if (!mqtt) return NULL;

    char host[128] = {0};
    char user[64] = {0};
    char pass[64] = {0};
    char port_str[8] = {0};
    char tls_str[4] = {0};
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_op_mutex, portMAX_DELAY);
    err = snapshot_saved_config_locked(host, sizeof(host), port_str, sizeof(port_str),
                                       user, sizeof(user), pass, sizeof(pass),
                                       tls_str, sizeof(tls_str));
    xSemaphoreGive(s_op_mutex);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not snapshot MQTT config for export: %s", esp_err_to_name(err));
        cJSON_Delete(mqtt);
        return NULL;
    }

    const bool configured = host[0] != '\0';
    const int port = (port_str[0] != '\0') ? atoi(port_str) : 1883;
    const bool use_tls = strcmp(tls_str, "1") == 0;

    if (view == MQTT_EXPORT_VIEW_BACKUP) {
        cJSON_AddStringToObject(mqtt, "host", host);
        cJSON_AddNumberToObject(mqtt, "port", port);
        cJSON_AddStringToObject(mqtt, "username", user);
        cJSON_AddStringToObject(mqtt, "password", pass);
        cJSON_AddBoolToObject(mqtt, "tls", use_tls);
    } else {
        cJSON_AddBoolToObject(mqtt, "configured", configured);
        cJSON_AddStringToObject(mqtt, "host", host);
        cJSON_AddNumberToObject(mqtt, "port", port);
        cJSON_AddStringToObject(mqtt, "username", user);
        cJSON_AddBoolToObject(mqtt, "tls", use_tls);
        cJSON_AddBoolToObject(mqtt, "password_set", pass[0] != '\0');
    }

    return mqtt;
}

struct cJSON *mqtt_actions_export(void)
{
    if (!ensure_runtime_state()) return NULL;

    cJSON *actions = cJSON_CreateArray();
    if (!actions) return NULL;

    mqtt_action_t snapshot[MQTT_MAX_ACTIONS];
    xSemaphoreTake(s_actions_mutex, portMAX_DELAY);
    actions_snapshot_locked(snapshot);
    xSemaphoreGive(s_actions_mutex);
    for (int i = 0; i < MQTT_MAX_ACTIONS; i++) {
        if (snapshot[i].name[0] == '\0') continue;

        cJSON *item = action_export_json(&snapshot[i], i);
        if (!item || !cJSON_AddItemToArray(actions, item)) {
            if (item) cJSON_Delete(item);
            cJSON_Delete(actions);
            return NULL;
        }
    }

    return actions;
}

bool mqtt_backup_export(struct cJSON *root)
{
    if (!root) return false;
    cJSON *mqtt = mqtt_config_export(MQTT_EXPORT_VIEW_BACKUP);
    cJSON *actions = mqtt_actions_export();
    if (!mqtt || !actions) {
        if (mqtt) cJSON_Delete(mqtt);
        if (actions) cJSON_Delete(actions);
        return false;
    }
    if (!cJSON_AddItemToObject(root, "mqtt", mqtt)) {
        cJSON_Delete(mqtt);
        cJSON_Delete(actions);
        return false;
    }
    if (!cJSON_AddItemToObject(root, "mqtt_actions", actions)) {
        cJSON_Delete(actions);
        return false;
    }
    return true;
}

esp_err_t mqtt_backup_import(const struct cJSON *root_obj)
{
    cJSON *root = (cJSON *)root_obj;
    if (!root) return ESP_ERR_INVALID_ARG;

    cJSON *mqtt = cJSON_GetObjectItem(root, "mqtt");
    if (cJSON_IsObject(mqtt)) {
        if (!ensure_runtime_state()) return ESP_ERR_NO_MEM;

        cJSON *host = cJSON_GetObjectItem(mqtt, "host");
        cJSON *port = cJSON_GetObjectItem(mqtt, "port");
        cJSON *user = cJSON_GetObjectItem(mqtt, "username");
        cJSON *pass = cJSON_GetObjectItem(mqtt, "password");
        cJSON *tls = cJSON_GetObjectItem(mqtt, "tls");
        if (!cJSON_IsString(host) || !cJSON_IsNumber(port) ||
                !cJSON_IsString(user) || !cJSON_IsString(pass) || !cJSON_IsBool(tls)) {
            return ESP_ERR_INVALID_ARG;
        }

        char port_buf[8] = {0};
        char tls_buf[4] = {0};
        snprintf(port_buf, sizeof(port_buf), "%d", (int)port->valuedouble);
        strlcpy(tls_buf, cJSON_IsTrue(tls) ? "1" : "0", sizeof(tls_buf));
        xSemaphoreTake(s_op_mutex, portMAX_DELAY);
        esp_err_t err = save_config_snapshot_locked(host->valuestring,
                                                    port_buf,
                                                    user->valuestring,
                                                    pass->valuestring,
                                                    tls_buf);
        xSemaphoreGive(s_op_mutex);
        if (err != ESP_OK) return err;
    }

    cJSON *actions = cJSON_GetObjectItem(root, "mqtt_actions");
    if (cJSON_IsArray(actions)) {
        mqtt_action_t next[MQTT_MAX_ACTIONS];
        memset(next, 0, sizeof(next));

        cJSON *item;
        cJSON_ArrayForEach(item, actions) {
            cJSON *idx = cJSON_GetObjectItem(item, "idx");
            if (!cJSON_IsNumber(idx)) continue;

            int action_idx = (int)idx->valuedouble;
            if (action_idx < 0 || action_idx >= MQTT_MAX_ACTIONS) continue;

            cJSON *name = cJSON_GetObjectItem(item, "name");
            cJSON *topic = cJSON_GetObjectItem(item, "topic");
            cJSON *payload = cJSON_GetObjectItem(item, "payload");
            if (cJSON_IsString(name)) strlcpy(next[action_idx].name, name->valuestring, sizeof(next[action_idx].name));
            if (cJSON_IsString(topic)) strlcpy(next[action_idx].topic, topic->valuestring, sizeof(next[action_idx].topic));
            if (cJSON_IsString(payload)) strlcpy(next[action_idx].payload, payload->valuestring, sizeof(next[action_idx].payload));
        }

        xSemaphoreTake(s_actions_mutex, portMAX_DELAY);
        esp_err_t err = actions_import_locked(next);
        xSemaphoreGive(s_actions_mutex);
        if (err != ESP_OK) return err;
    }

    return ESP_OK;
}

void mqtt_init(void)
{
    if (!ensure_runtime_state()) {
        set_status(mqtt_error_status());
        ESP_LOGE(TAG, "runtime init failed");
        return;
    }

    xSemaphoreTake(s_actions_mutex, portMAX_DELAY);
    ensure_actions_loaded_locked();
    xSemaphoreGive(s_actions_mutex);

    char host[128] = {}, port_str[8] = {}, user[64] = {}, pass[64] = {};
    bool use_tls = false;
    if (load_credentials(host, sizeof(host), port_str, sizeof(port_str),
                         user, sizeof(user), pass, sizeof(pass), &use_tls)) {
        set_status(MQTT_STATUS_WAITING_NET);
    } else {
        set_status(MQTT_STATUS_NOT_CONFIG);
    }
}

void mqtt_set_network_available(bool available)
{
    if (!ensure_runtime_state()) {
        set_status(available ? MQTT_STATUS_ERROR : MQTT_STATUS_WAITING_NET);
        ESP_LOGE(TAG, "runtime init failed");
        return;
    }
    s_network_available = available;

    if (!available) {
        if (s_status != MQTT_STATUS_DISABLED) {
            char host[128] = {}, user[64] = {}, pass[64] = {};
            uint32_t port = 0;
            bool use_tls = false;
            set_status(load_saved_config(host, sizeof(host), &port, user, sizeof(user),
                                         pass, sizeof(pass), &use_tls)
                           ? MQTT_STATUS_WAITING_NET
                           : MQTT_STATUS_NOT_CONFIG);
        }
        return;
    }

    if (s_status == MQTT_STATUS_DISABLED || s_status == MQTT_STATUS_UP) return;
    if (s_client) {
        set_status(MQTT_STATUS_CONNECTING);
        return;
    }

    char host[128] = {}, user[64] = {}, pass[64] = {};
    uint32_t port = 0;
    bool use_tls = false;
    if (!load_saved_config(host, sizeof(host), &port, user, sizeof(user),
                           pass, sizeof(pass), &use_tls)) {
        set_status(MQTT_STATUS_NOT_CONFIG);
        return;
    }

    ESP_LOGI(TAG, "connecting to %s:%" PRIu32 "%s", host, port, use_tls ? " (TLS)" : "");
    mqtt_connect_broker(host, port, user, pass, use_tls);
}

void mqtt_connect_api(const char *host, uint32_t port,
                      const char *username, const char *password, bool use_tls,
                      bool password_provided)
{
    if (!ensure_runtime_state()) {
        set_status(mqtt_error_status());
        ESP_LOGE(TAG, "runtime init failed");
        return;
    }

    char pass_to_use[64] = {};
    bool keep_existing_pass = false;

    xSemaphoreTake(s_op_mutex, portMAX_DELAY);

    if (password_provided) {
        strlcpy(pass_to_use, password, sizeof(pass_to_use));
    } else {
        keep_existing_pass = load_saved_password(pass_to_use, sizeof(pass_to_use));
    }

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%" PRIu32, port);
    nvs_handle_t nvs;
    if (nvs_open("mqtt", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "host", host);
        nvs_set_str(nvs, "port", port_str);
        nvs_set_str(nvs, "user", username);
        if (password_provided) {
            nvs_set_str(nvs, "pass", password);
        } else if (keep_existing_pass) {
            nvs_set_str(nvs, "pass", pass_to_use);
        } else {
            nvs_set_str(nvs, "pass", "");
        }
        nvs_set_str(nvs, "tls",  use_tls ? "1" : "0");
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    if (s_network_available) {
        mqtt_connect_broker_locked(host, port, username, pass_to_use, use_tls);
    } else {
        destroy_client();
        set_status(MQTT_STATUS_WAITING_NET);
    }
    xSemaphoreGive(s_op_mutex);
}

void mqtt_disconnect(void)
{
    if (!ensure_runtime_state()) return;
    xSemaphoreTake(s_op_mutex, portMAX_DELAY);
    set_status(MQTT_STATUS_DISABLED);
    destroy_client();
    xSemaphoreGive(s_op_mutex);
}

void mqtt_clean_credentials(void)
{
    mqtt_disconnect();
    set_status(MQTT_STATUS_NOT_CONFIG);
    nvs_handle_t nvs;
    if (nvs_open("mqtt", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_key(nvs, "host");
        nvs_erase_key(nvs, "port");
        nvs_erase_key(nvs, "user");
        nvs_erase_key(nvs, "pass");
        nvs_erase_key(nvs, "tls");
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

int mqtt_publish(const char *topic, const char *payload)
{
    if (!ensure_runtime_state()) return -1;
    xSemaphoreTake(s_op_mutex, portMAX_DELAY);
    if (s_status != MQTT_STATUS_UP) {
        xSemaphoreGive(s_op_mutex);
        ESP_LOGW(TAG, "publish skipped: not connected");
        return -1;
    }
    int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 0);
    xSemaphoreGive(s_op_mutex);
    return msg_id < 0 ? -1 : 0;
}

void mqtt_set_status_callback(mqtt_status_cb_t cb)
{
    s_status_cb = cb;
    if (s_status_cb) {
        s_status_cb(s_status);
    }
}

void mqtt_subscribe(const char *topic, mqtt_message_cb_t cb)
{
    if (!ensure_runtime_state()) {
        ESP_LOGE(TAG, "runtime init failed");
        return;
    }

    xSemaphoreTake(s_subs_mutex, portMAX_DELAY);
    // Update the callback if the topic is already registered
    for (int i = 0; i < s_nsubs; i++) {
        if (strcmp(s_subs[i].topic, topic) == 0) {
            s_subs[i].cb = cb;
            xSemaphoreGive(s_subs_mutex);
            return;
        }
    }

    if (s_nsubs >= MQTT_MAX_SUBS) {
        ESP_LOGE(TAG, "mqtt_subscribe: max subscriptions (%d) reached", MQTT_MAX_SUBS);
        xSemaphoreGive(s_subs_mutex);
        return;
    }
    strlcpy(s_subs[s_nsubs].topic, topic, sizeof(s_subs[s_nsubs].topic));
    s_subs[s_nsubs].cb = cb;
    s_nsubs++;

    xSemaphoreGive(s_subs_mutex);
    xSemaphoreTake(s_op_mutex, portMAX_DELAY);
    if (s_status == MQTT_STATUS_UP && s_client) {
        esp_mqtt_client_subscribe(s_client, topic, 1);
    }
    xSemaphoreGive(s_op_mutex);
}

const char *mqtt_status_str(mqtt_status_t s)
{
    static const char *names[] = {
#define X(name, str) str,
        MQTT_STATUS_LIST
#undef X
    };
    return (s < sizeof(names)/sizeof(*names)) ? names[s] : "?";
}

mqtt_status_t mqtt_get_status(void)
{
    return s_status;
}
