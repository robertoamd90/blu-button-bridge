#include <string.h>
#include <stdlib.h>
#include "cJSON.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "web_manager.h"
#include "ble_access.h"

static const char *TAG = "web_manager";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

// ── HTTP helpers ─────────────────────────────────────────────────────────────

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

// ── Async background tasks (WiFi/MQTT connect block up to 10s) ───────────────

typedef struct { char ssid[33]; char pass[65]; } wifi_creds_t;
typedef struct { char host[128]; uint32_t port; char user[64]; char pass[64]; bool tls; } mqtt_creds_t;

static void wifi_connect_task(void *arg)
{
    wifi_creds_t *c = (wifi_creds_t *)arg;
    wifi_connect_api(c->ssid, c->pass);
    free(c);
    vTaskDelete(NULL);
}

static void mqtt_connect_task(void *arg)
{
    mqtt_creds_t *c = (mqtt_creds_t *)arg;
    mqtt_connect_api(c->host, c->port, c->user, c->pass, c->tls);
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

static esp_err_t handle_status(httpd_req_t *req)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "wifi",        wifi_status_str(wifi_get_status()));
    cJSON_AddStringToObject(obj, "mqtt",        mqtt_status_str(mqtt_get_status()));
    cJSON_AddBoolToObject  (obj, "ap",          wifi_ap_is_active());
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
    nvs_flash_erase(); // wipes all NVS: WiFi, MQTT, AP config, BLE
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

// GET /api/ble/devices
static esp_err_t handle_ble_devices(httpd_req_t *req)
{
    ble_device_t *devs = malloc(BLE_ACCESS_MAX_DEVICES * sizeof(ble_device_t));
    if (!devs) return send_error(req, "out of memory");
    int n = ble_access_get_devices(devs, BLE_ACCESS_MAX_DEVICES);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        ble_device_t *d = &devs[i];
        char mac_str[18];
        mac_to_str(d->mac, mac_str);
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
        cJSON_AddItemToArray(arr, obj);
    }
    free(devs);
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

// PATCH /api/ble/device  {mac, label, enabled, single_press, double_press, triple_press, long_press, key?}
// Event fields are integer bitmasks referencing MQTT action slots.
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
    cJSON *key_item = cJSON_GetObjectItem(root, "key");

    if (cJSON_IsString(lbl_item)) strlcpy(current.label, lbl_item->valuestring, sizeof(current.label));
    if (cJSON_IsBool(en_item))    current.enabled      = cJSON_IsTrue(en_item);
    if (cJSON_IsNumber(sp_item))  current.single_press = (uint16_t)sp_item->valuedouble;
    if (cJSON_IsNumber(dp_item))  current.double_press = (uint16_t)dp_item->valuedouble;
    if (cJSON_IsNumber(tp_item))  current.triple_press = (uint16_t)tp_item->valuedouble;
    if (cJSON_IsNumber(lp_item))  current.long_press   = (uint16_t)lp_item->valuedouble;

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

// ── Init ─────────────────────────────────────────────────────────────────────

void web_manager_init(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size        = 10240;
    config.max_uri_handlers  = 28;
    config.max_open_sockets  = 4;
    config.lru_purge_enable  = true;
    config.uri_match_fn      = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    const httpd_uri_t uris[] = {
        { .uri = "/",                         .method = HTTP_GET,    .handler = handle_root                },
        { .uri = "/api/status",               .method = HTTP_GET,    .handler = handle_status              },
        { .uri = "/api/wifi/config",          .method = HTTP_GET,    .handler = handle_wifi_config_get     },
        { .uri = "/api/wifi/scan",            .method = HTTP_GET,    .handler = handle_wifi_scan           },
        { .uri = "/api/wifi/connect",         .method = HTTP_POST,   .handler = handle_wifi_connect        },
        { .uri = "/api/wifi",                 .method = HTTP_DELETE, .handler = handle_wifi_delete         },
        { .uri = "/api/mqtt/config",          .method = HTTP_GET,    .handler = handle_mqtt_config_get     },
        { .uri = "/api/mqtt/connect",         .method = HTTP_POST,   .handler = handle_mqtt_connect        },
        { .uri = "/api/mqtt/actions",         .method = HTTP_GET,    .handler = handle_mqtt_actions_get    },
        { .uri = "/api/mqtt/actions",         .method = HTTP_POST,   .handler = handle_mqtt_action_add     },
        { .uri = "/api/mqtt/action",          .method = HTTP_PUT,    .handler = handle_mqtt_action_update  },
        { .uri = "/api/mqtt/action",          .method = HTTP_DELETE, .handler = handle_mqtt_action_delete  },
        { .uri = "/api/mqtt",                 .method = HTTP_DELETE, .handler = handle_mqtt_delete         },
        { .uri = "/api/ap/start",             .method = HTTP_POST,   .handler = handle_ap_start            },
        { .uri = "/api/ap/stop",              .method = HTTP_POST,   .handler = handle_ap_stop             },
        { .uri = "/api/ap/config",            .method = HTTP_GET,    .handler = handle_ap_config_get       },
        { .uri = "/api/ap/config",            .method = HTTP_POST,   .handler = handle_ap_config_set       },
        { .uri = "/api/system/reboot",        .method = HTTP_POST,   .handler = handle_system_reboot       },
        { .uri = "/api/system/factory-reset",  .method = HTTP_POST,   .handler = handle_system_factory_reset  },
        { .uri = "/api/ble/devices",           .method = HTTP_GET,    .handler = handle_ble_devices           },
        { .uri = "/api/ble/register/status",   .method = HTTP_GET,    .handler = handle_ble_reg_status        },
        { .uri = "/api/ble/register/start",    .method = HTTP_POST,   .handler = handle_ble_reg_start         },
        { .uri = "/api/ble/register/cancel",   .method = HTTP_POST,   .handler = handle_ble_reg_cancel        },
        { .uri = "/api/ble/register/confirm",  .method = HTTP_POST,   .handler = handle_ble_reg_confirm       },
        { .uri = "/api/ble/device",            .method = HTTP_PATCH,  .handler = handle_ble_device_update     },
        { .uri = "/api/ble/device/reimport",   .method = HTTP_POST,   .handler = handle_ble_device_reimport   },
        { .uri = "/api/ble/device",            .method = HTTP_DELETE, .handler = handle_ble_device_delete     },
        // Catch-all: must be registered LAST
        { .uri = "/*",                         .method = HTTP_GET,    .handler = handle_captive               },
    };

    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
}
