#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/portmacro.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "esp_log.h"
#include "mqtt_manager.h"

#define MQTT_CONNECTED_BIT  BIT0
#define MQTT_MAX_SUBS       8
#define MQTT_MAX_TOPIC_LEN  128
#define MQTT_MAX_PAYLOAD    1024

static const char *TAG = "mqtt_manager";

typedef struct {
    char              topic[MQTT_MAX_TOPIC_LEN];
    mqtt_message_cb_t cb;
} mqtt_sub_t;

static esp_mqtt_client_handle_t s_client   = NULL;
static EventGroupHandle_t       s_events   = NULL;
static volatile mqtt_status_t   s_status   = MQTT_STATUS_NOT_CONFIG;
static bool                     s_stopping = false;
static mqtt_sub_t               s_subs[MQTT_MAX_SUBS];
static int                      s_nsubs    = 0;
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

// ── Event handler ──────────────────────────────────────────────────────────────

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t ev = event_data;

    if (s_stopping) return;

    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            set_status(MQTT_STATUS_UP);
            xEventGroupSetBits(s_events, MQTT_CONNECTED_BIT);
            ESP_LOGI(TAG, "connected to broker");
            xSemaphoreTake(s_subs_mutex, portMAX_DELAY);
            for (int i = 0; i < s_nsubs; i++) {
                esp_mqtt_client_subscribe(s_client, s_subs[i].topic, 1);
            }
            xSemaphoreGive(s_subs_mutex);
            break;

        case MQTT_EVENT_DISCONNECTED:
            if (s_status == MQTT_STATUS_UP)
                set_status(MQTT_STATUS_DOWN);
            xEventGroupClearBits(s_events, MQTT_CONNECTED_BIT);
            ESP_LOGW(TAG, "disconnected from broker");
            break;

        case MQTT_EVENT_DATA:
            if (ev->topic_len > MQTT_MAX_TOPIC_LEN ||
                ev->data_len  > MQTT_MAX_PAYLOAD) {
                ESP_LOGW(TAG, "message too large, skipped");
                break;
            }
            if (ev->data_len > 0 && ev->topic_len > 0) {
                char payload[ev->data_len + 1];
                memcpy(payload, ev->data, ev->data_len);
                payload[ev->data_len] = '\0';

                char topic[ev->topic_len + 1];
                memcpy(topic, ev->topic, ev->topic_len);
                topic[ev->topic_len] = '\0';

                xSemaphoreTake(s_subs_mutex, portMAX_DELAY);
                for (int i = 0; i < s_nsubs; i++) {
                    if (strcmp(s_subs[i].topic, topic) == 0) {
                        s_subs[i].cb(topic, payload, ev->data_len);
                        break;
                    }
                }
                xSemaphoreGive(s_subs_mutex);
            }
            break;

        case MQTT_EVENT_ERROR:
            set_status(MQTT_STATUS_ERROR);
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
    if (s_events) {
        vEventGroupDelete(s_events);
        s_events = NULL;
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

// ── Public API ────────────────────────────────────────────────────────────────

static void mqtt_connect_broker_locked(const char *host, uint32_t port, const char *username,
                                       const char *password, bool use_tls)
{
    destroy_client();
    s_events = xEventGroupCreate();
    if (!s_events) {
        set_status(MQTT_STATUS_ERROR);
        ESP_LOGE(TAG, "event group allocation failed");
        return;
    }

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

    set_status(MQTT_STATUS_DOWN);
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        set_status(MQTT_STATUS_ERROR);
        ESP_LOGE(TAG, "client init failed");
        destroy_client();
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        set_status(MQTT_STATUS_ERROR);
        ESP_LOGE(TAG, "client start failed");
        destroy_client();
        return;
    }

    EventBits_t bits = xEventGroupWaitBits(s_events, MQTT_CONNECTED_BIT,
                                           false, true, pdMS_TO_TICKS(10000));
    if (!(bits & MQTT_CONNECTED_BIT)) {
        set_status(MQTT_STATUS_ERROR);
        ESP_LOGE(TAG, "broker connection timeout");
    }
}

void mqtt_connect_broker(const char *host, uint32_t port, const char *username,
               const char *password, bool use_tls)
{
    if (!ensure_runtime_state()) {
        set_status(MQTT_STATUS_ERROR);
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

static esp_err_t actions_save_locked(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("mqtt", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, "actions", s_actions, sizeof(s_actions));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
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

    mqtt_publish(action.topic, action.payload);
    return ESP_OK;
}

void mqtt_init(void)
{
    if (!ensure_runtime_state()) {
        set_status(MQTT_STATUS_ERROR);
        ESP_LOGE(TAG, "runtime init failed");
        return;
    }

    if (!s_actions_loaded) {
        xSemaphoreTake(s_actions_mutex, portMAX_DELAY);
        actions_load_locked();
        xSemaphoreGive(s_actions_mutex);
        s_actions_loaded = true;
    }
    if (s_status == MQTT_STATUS_UP) return;

    char host[128] = {}, port_str[8] = {}, user[64] = {}, pass[64] = {};
    bool use_tls = false;

    if (load_credentials(host, sizeof(host), port_str, sizeof(port_str),
                         user, sizeof(user), pass, sizeof(pass), &use_tls)) {
        ESP_LOGI(TAG, "connecting to %s:%s%s", host, port_str, use_tls ? " (TLS)" : "");
        mqtt_connect_broker(host, (uint32_t)atoi(port_str), user, pass, use_tls);
    }
}

void mqtt_connect_api(const char *host, uint32_t port,
                      const char *username, const char *password, bool use_tls,
                      bool password_provided)
{
    if (!ensure_runtime_state()) {
        set_status(MQTT_STATUS_ERROR);
        ESP_LOGE(TAG, "runtime init failed");
        return;
    }

    char pass_to_use[64] = {};
    bool keep_existing_pass = false;

    xSemaphoreTake(s_op_mutex, portMAX_DELAY);

    if (password_provided) {
        strncpy(pass_to_use, password, sizeof(pass_to_use) - 1);
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
    mqtt_connect_broker_locked(host, port, username, pass_to_use, use_tls);
    xSemaphoreGive(s_op_mutex);
}

bool mqtt_get_saved_config(char *host, size_t host_len, uint32_t *port,
                           char *user, size_t user_len, bool *use_tls, bool *has_pass)
{
    nvs_handle_t nvs;
    if (nvs_open("mqtt", NVS_READONLY, &nvs) != ESP_OK) return false;

    char port_str[8] = {};
    size_t port_len = sizeof(port_str);
    char pass[64] = {};
    size_t pass_len = sizeof(pass);
    char tls_str[4] = {};
    size_t tls_len = sizeof(tls_str);

    bool ok = (nvs_get_str(nvs, "host", host, &host_len) == ESP_OK && strlen(host) > 0);
    if (ok) {
        nvs_get_str(nvs, "port", port_str, &port_len);
        nvs_get_str(nvs, "user", user, &user_len);
        nvs_get_str(nvs, "pass", pass, &pass_len);
        if (nvs_get_str(nvs, "tls", tls_str, &tls_len) == ESP_OK && use_tls)
            *use_tls = (tls_str[0] == '1');
        if (port)     *port     = (uint32_t)atoi(port_str);
        if (has_pass) *has_pass = (strlen(pass) > 0);
    }
    nvs_close(nvs);
    return ok;
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
    strncpy(s_subs[s_nsubs].topic, topic, sizeof(s_subs[s_nsubs].topic) - 1);
    s_subs[s_nsubs].cb = cb;
    s_nsubs++;

    xSemaphoreGive(s_subs_mutex);
    if (s_status == MQTT_STATUS_UP) {
        esp_mqtt_client_subscribe(s_client, topic, 1);
    }
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
