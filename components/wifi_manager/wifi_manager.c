#include <string.h>
#include <stdbool.h>
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "wifi_manager.h"
#include "gpio_manager.h"
#include "ble_access.h"
#include "esp_log.h"

#include <stdlib.h>
#include "esp_netif.h"
#include "esp_mac.h"
#include "lwip/sockets.h"

static const char *TAG = "wifi";
static const TickType_t AP_HANDOFF_TIMEOUT_TICKS = pdMS_TO_TICKS(15000);
static const uint32_t WIFI_WORK_RECONNECT        = (1U << 0);
static const uint32_t WIFI_WORK_AP_HANDOFF_CLOSE = (1U << 1);

static volatile wifi_status_t s_status = WIFI_STATUS_NOT_CONFIG;
static wifi_status_cb_t     s_status_cb = NULL;
static wifi_ap_status_cb_t  s_ap_status_cb = NULL;
static TimerHandle_t        s_reconnect_timer = NULL;
static TimerHandle_t        s_ap_handoff_timer = NULL;
static volatile bool        s_ap_active = false;
static volatile bool        s_ap_handoff_armed = false;
static volatile bool        s_error_latched = false;
static volatile bool        s_manual_reconfig_in_progress = false;
static TaskHandle_t         s_worker_task_handle = NULL;
static TaskHandle_t         s_dns_task_handle = NULL;
static esp_netif_t         *s_sta_netif = NULL;

static void set_status(wifi_status_t status);
static void notify_ap_status(void);
static void clear_ap_handoff(void);
static bool ap_handoff_available(void);
static void queue_wifi_work(uint32_t work_bits);
static void wifi_worker_task(void *arg);
static void ap_handoff_timer_cb(TimerHandle_t t);
static bool wifi_reason_is_config_error(wifi_err_reason_t reason);
static bool load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len);
static esp_err_t backup_load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len);
static void wifi_ap_load_defaults(wifi_ap_settings_t *cfg);
static esp_err_t wifi_ap_read_config(wifi_ap_settings_t *cfg);
static esp_err_t wifi_ap_store_config(const wifi_ap_settings_t *cfg);
static esp_err_t wifi_ap_apply_config(const wifi_ap_settings_t *cfg);
static bool wifi_apply_sta_config_and_connect(const char *ssid, const char *pass);
static bool wifi_reload_credentials_and_connect(void);
static void wifi_connect(const char *ssid, const char *pass);

// AP config in RAM — ssid is overwritten at runtime by wifi_ap_load_config (MAC-based)
static wifi_ap_settings_t s_ap_cfg = {
    .enabled  = false,
    .ssid     = "BBB-000000",
    .password = "12345678",
};

// ── Event handlers ─────────────────────────────────────────────────────────────

static void set_status(wifi_status_t status)
{
    if (s_status == status) return;
    switch (status) {
        case WIFI_STATUS_UP:
        case WIFI_STATUS_NOT_CONFIG:
        case WIFI_STATUS_DISABLED:
            s_error_latched = false;
            break;
        case WIFI_STATUS_ERROR:
            s_error_latched = true;
            break;
        default:
            break;
    }
    s_status = status;
    if (s_status_cb) {
        s_status_cb(status);
    }
}

static void notify_ap_status(void)
{
    if (s_ap_status_cb) {
        s_ap_status_cb(s_ap_active);
    }
}

static void clear_ap_handoff(void)
{
    s_ap_handoff_armed = false;
    if (s_ap_handoff_timer) {
        xTimerStop(s_ap_handoff_timer, 0);
    }
}

static bool ap_handoff_available(void)
{
    return s_ap_active && !s_ap_cfg.enabled;
}

static void queue_wifi_work(uint32_t work_bits)
{
    if (!s_worker_task_handle) {
        return;
    }

    xTaskNotify(s_worker_task_handle, work_bits, eSetBits);
}

static void wifi_worker_task(void *arg)
{
    (void)arg;

    while (true) {
        uint32_t work_bits = 0;
        xTaskNotifyWait(0, 0xFFFFFFFFu, &work_bits, portMAX_DELAY);

        if (work_bits & WIFI_WORK_RECONNECT) {
            if (!s_manual_reconfig_in_progress &&
                    (s_status == WIFI_STATUS_CONNECTING || s_status == WIFI_STATUS_ERROR)) {
                set_status(WIFI_STATUS_CONNECTING);
                esp_wifi_connect();
            }
        }

        if (work_bits & WIFI_WORK_AP_HANDOFF_CLOSE) {
            if (!s_ap_handoff_armed) {
                continue;
            }

            s_ap_handoff_armed = false;
            if (s_ap_active && !s_ap_cfg.enabled && s_status == WIFI_STATUS_UP) {
                ESP_LOGI(TAG, "closing AP after WiFi handoff timeout");
                wifi_stop_ap();
            }
        }
    }
}

static void on_wifi_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
    set_status(WIFI_STATUS_UP);
    xTimerStop(s_reconnect_timer, 0);
    ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&event->ip_info.ip));
    if (s_ap_active && !s_ap_cfg.enabled) {
        if (s_ap_handoff_armed) {
            if (s_ap_handoff_timer && xTimerChangePeriod(s_ap_handoff_timer, AP_HANDOFF_TIMEOUT_TICKS, 0) == pdPASS) {
                ESP_LOGI(TAG, "keeping AP up briefly for WiFi handoff page");
            } else {
                ESP_LOGW(TAG, "failed to start AP handoff timer, stopping AP immediately");
                clear_ap_handoff();
                wifi_stop_ap();
            }
        } else {
            wifi_stop_ap();
        }
    }
}

static void ap_handoff_timer_cb(TimerHandle_t t)
{
    (void)t;
    queue_wifi_work(WIFI_WORK_AP_HANDOFF_CLOSE);
}

static void reconnect_timer_cb(TimerHandle_t t)
{
    (void)t;
    queue_wifi_work(WIFI_WORK_RECONNECT);
}

static void on_wifi_disconnected(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)data;

    if (s_manual_reconfig_in_progress) {
        return;
    }

    ESP_LOGI(TAG, "disconnected from WiFi (reason %d)", event->reason);
    clear_ap_handoff();

    if (s_status != WIFI_STATUS_DISABLED && s_status != WIFI_STATUS_NOT_CONFIG) {
        if (!event || (event->reason != WIFI_REASON_ASSOC_LEAVE &&
                       event->reason != WIFI_REASON_STA_LEAVING)) {
            if (event && wifi_reason_is_config_error(event->reason)) {
                set_status(WIFI_STATUS_ERROR);
            } else {
                set_status(WIFI_STATUS_CONNECTING);
            }
            xTimerReset(s_reconnect_timer, 0);
        }
    }
}

// ── Private helpers ────────────────────────────────────────────────────────────

static bool load_credentials(char *ssid, size_t ssid_len,
                              char *pass, size_t pass_len)
{
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) != ESP_OK) return false;

    ssid[0] = '\0';
    pass[0] = '\0';

    bool ok = (nvs_get_str(nvs, "ssid", ssid, &ssid_len) == ESP_OK &&
               strlen(ssid) > 0);
    if (ok) {
        nvs_get_str(nvs, "pass", pass, &pass_len);
    }

    nvs_close(nvs);
    return ok;
}

static esp_err_t backup_load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    if (!ssid || ssid_len == 0 || !pass || pass_len == 0) return ESP_ERR_INVALID_ARG;

    ssid[0] = '\0';
    pass[0] = '\0';

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    size_t ssid_len_local = ssid_len;
    err = nvs_get_str(nvs, "ssid", ssid, &ssid_len_local);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    } else if (err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }

    if (ssid[0] != '\0') {
        size_t pass_len_local = pass_len;
        err = nvs_get_str(nvs, "pass", pass, &pass_len_local);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }

    nvs_close(nvs);
    return err;
}

static bool wifi_reason_is_config_error(wifi_err_reason_t reason)
{
    switch (reason) {
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_ASSOC_FAIL:
        case WIFI_REASON_IE_INVALID:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_IE_IN_4WAY_DIFFERS:
        case WIFI_REASON_GROUP_CIPHER_INVALID:
        case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
        case WIFI_REASON_AKMP_INVALID:
        case WIFI_REASON_UNSUPP_RSN_IE_VERSION:
        case WIFI_REASON_INVALID_RSN_IE_CAP:
        case WIFI_REASON_802_1X_AUTH_FAILED:
        case WIFI_REASON_CIPHER_SUITE_REJECTED:
        case WIFI_REASON_BAD_CIPHER_OR_AKM:
        case WIFI_REASON_NOT_AUTHORIZED_THIS_LOCATION:
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        case WIFI_REASON_ASSOC_NOT_AUTHED:
        case WIFI_REASON_CONNECTION_FAIL:
            return true;
        default:
            return false;
    }
}

static bool wifi_apply_sta_config_and_connect(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0') {
        return false;
    }

    wifi_config_t cfg = {};
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    size_t ssid_len = strnlen(ssid, sizeof(cfg.sta.ssid));
    memcpy(cfg.sta.ssid, ssid, ssid_len);
    strlcpy((char *)cfg.sta.password, pass ? pass : "", sizeof(cfg.sta.password));

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set sta config failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect attempt failed: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

static bool wifi_reload_credentials_and_connect(void)
{
    char ssid[33] = {};
    char pass[65] = {};

    if (!load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        set_status(WIFI_STATUS_NOT_CONFIG);
        return false;
    }

    ESP_LOGI(TAG, "connecting to %s", ssid);
    set_status(WIFI_STATUS_CONNECTING);
    bool started = wifi_apply_sta_config_and_connect(ssid, pass);
    if (!started) {
        set_status(WIFI_STATUS_ERROR);
    }
    return started;
}

static void set_device_hostname(void)
{
    if (!s_sta_netif) return;

    uint8_t mac[6];
    char hostname[32];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        // If we can't read the MAC address, use a generic hostname. This is unlikely to happen, but better than failing completely.
        strlcpy(hostname, "blubuttonbridge", sizeof(hostname));
    } else {
        snprintf(hostname, sizeof(hostname), "bbb-%02x%02x%02x",
                 mac[3], mac[4], mac[5]);
    }

    esp_err_t err = esp_netif_set_hostname(s_sta_netif, hostname);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "hostname set to %s", hostname);
    } else {
        ESP_LOGW(TAG, "failed to set hostname: %s", esp_err_to_name(err));
    }
}

// Starts a connection attempt and lets retries continue in the background.
static void wifi_connect(const char *ssid, const char *pass)
{
    xTimerStop(s_reconnect_timer, 0);
    s_error_latched = false;
    set_status(WIFI_STATUS_CONNECTING);
    s_manual_reconfig_in_progress = true;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));

    bool started = wifi_apply_sta_config_and_connect(ssid, pass);
    s_manual_reconfig_in_progress = false;

    if (!started) {
        set_status(WIFI_STATUS_ERROR);
        xTimerReset(s_reconnect_timer, 0);
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void wifi_connect_api(const char *ssid, const char *pass, bool password_provided)
{
    char pass_to_use[65] = {};
    bool keep_existing_pass = false;
    if (!password_provided) {
        char saved_ssid[33] = {};
        char saved_pass[65] = {};
        if (load_credentials(saved_ssid, sizeof(saved_ssid), saved_pass, sizeof(saved_pass)) &&
                strcmp(saved_ssid, ssid) == 0 &&
                strlen(saved_pass) > 0) {
            keep_existing_pass = true;
            strlcpy(pass_to_use, saved_pass, sizeof(pass_to_use));
        }
    } else {
        strlcpy(pass_to_use, pass, sizeof(pass_to_use));
    }

    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "ssid", ssid);
        if (password_provided) {
            nvs_set_str(nvs, "pass", pass);
        } else if (!keep_existing_pass) {
            nvs_set_str(nvs, "pass", "");
        }
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    wifi_connect(ssid, pass_to_use);
}

const char *wifi_status_str(wifi_status_t s)
{
    static const char *names[] = {
#define X(name, str) str,
        WIFI_STATUS_LIST
#undef X
    };
    return (s < sizeof(names)/sizeof(*names)) ? names[s] : "?";
}

wifi_status_t wifi_get_status(void)
{
    return s_status;
}

bool wifi_get_sta_ip(char *buf, size_t len)
{
    if (!buf || len == 0 || !s_sta_netif) {
        return false;
    }

    buf[0] = '\0';

    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_get_ip_info(s_sta_netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return false;
    }

    snprintf(buf, len, IPSTR, IP2STR(&ip_info.ip));
    return true;
}

bool wifi_get_error_latched(void)
{
    return s_error_latched;
}

bool wifi_should_offer_ap_handoff(void)
{
    return ap_handoff_available();
}

bool wifi_arm_ap_handoff(void)
{
    clear_ap_handoff();

    if (!ap_handoff_available()) {
        return false;
    }

    s_ap_handoff_armed = true;
    return true;
}

void wifi_complete_ap_handoff(void)
{
    if (!s_ap_handoff_armed) {
        return;
    }

    clear_ap_handoff();
    if (s_ap_active && !s_ap_cfg.enabled && s_status == WIFI_STATUS_UP) {
        ESP_LOGI(TAG, "closing AP after WiFi handoff callback");
        wifi_stop_ap();
    }
}

void wifi_set_status_callback(wifi_status_cb_t cb)
{
    s_status_cb = cb;
    if (s_status_cb) {
        s_status_cb(s_status);
    }
}

void wifi_set_ap_status_callback(wifi_ap_status_cb_t cb)
{
    s_ap_status_cb = cb;
    if (s_ap_status_cb) {
        s_ap_status_cb(s_ap_active);
    }
}

void wifi_disconnect(void)
{
    clear_ap_handoff();
    s_manual_reconfig_in_progress = false;
    set_status(WIFI_STATUS_DISABLED);
    xTimerStop(s_reconnect_timer, 0);
    esp_wifi_disconnect();
}

void wifi_clean_credentials(void)
{
    clear_ap_handoff();
    s_manual_reconfig_in_progress = false;
    set_status(WIFI_STATUS_NOT_CONFIG);
    xTimerStop(s_reconnect_timer, 0);
    esp_wifi_disconnect();
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_key(nvs, "ssid");
        nvs_erase_key(nvs, "pass");
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

void wifi_init(void)
{
    // Stack init
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(s_sta_netif ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_ap() ? ESP_OK : ESP_ERR_NO_MEM);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    set_device_hostname();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(xTaskCreate(wifi_worker_task, "wifi_work", 4096, NULL, 5, &s_worker_task_handle) == pdPASS
                    ? ESP_OK
                    : ESP_ERR_NO_MEM);
    s_reconnect_timer = xTimerCreate("wifi_rc", pdMS_TO_TICKS(5000), pdFALSE, NULL, reconnect_timer_cb);
    s_ap_handoff_timer = xTimerCreate("wifi_ap_handoff", AP_HANDOFF_TIMEOUT_TICKS, pdFALSE, NULL, ap_handoff_timer_cb);
    ESP_ERROR_CHECK(s_ap_handoff_timer ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP,         on_wifi_got_ip,       NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi_disconnected, NULL));
    gpio_manager_set_boot_ap_callback(wifi_start_ap);

    // Application logic: load config, start AP and/or connect
    wifi_ap_load_config(&s_ap_cfg);

    bool started_from_saved_credentials = wifi_reload_credentials_and_connect();
    if (s_ap_cfg.enabled || !started_from_saved_credentials) {
        wifi_start_ap();
    }
}

// ── Captive-portal DNS server ──────────────────────────────────────────────────
// Responds to every UDP DNS query with 192.168.4.1, so the OS pops a
// "sign-in" notification and opens the browser to our config page.

static void dns_server_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { vTaskDelete(NULL); return; }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    // 100 ms receive timeout so the loop reacts quickly to s_ap_active=false
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[512];
    while (s_ap_active) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&client, &clen);
        if (len < 12) continue; // timeout or too short

        // Build response: copy query, flip QR bit, set ANCOUNT=1
        if (len + 16 > (int)sizeof(buf)) continue;
        uint8_t resp[512];
        memcpy(resp, buf, len);
        resp[2] = 0x81; // QR=1, AA=1
        resp[3] = 0x80; // RA=1, RCODE=0
        resp[6] = 0; resp[7] = 1; // ANCOUNT=1

        // Validate QNAME labels and make sure QTYPE/QCLASS are present.
        int pos = 12;
        while (pos < len && buf[pos] != 0) {
            uint8_t label_len = buf[pos];
            if (label_len > 63 || pos + 1 + label_len > len) goto malformed_dns_query;
            pos += 1 + label_len;
        }
        if (pos + 5 > len) continue;
        pos += 5;

        // Append A record answer
        int rpos = len;
        resp[rpos++] = 0xC0; resp[rpos++] = 0x0C; // NAME ptr -> offset 12
        resp[rpos++] = 0x00; resp[rpos++] = 0x01;  // TYPE A
        resp[rpos++] = 0x00; resp[rpos++] = 0x01;  // CLASS IN
        resp[rpos++] = 0x00; resp[rpos++] = 0x00;
        resp[rpos++] = 0x00; resp[rpos++] = 60;    // TTL 60s
        resp[rpos++] = 0x00; resp[rpos++] = 0x04;  // RDLENGTH 4
        resp[rpos++] = 192;  resp[rpos++] = 168;
        resp[rpos++] = 4;    resp[rpos++] = 1;     // 192.168.4.1

        sendto(sock, resp, rpos, 0, (struct sockaddr *)&client, clen);
        continue;
malformed_dns_query:
        ;
    }
    close(sock);
    s_dns_task_handle = NULL;
    vTaskDelete(NULL);
}

// ── AP lifecycle ───────────────────────────────────────────────────────────────

void wifi_start_ap(void)
{
    if (s_ap_active) {
        // Restart: signal old DNS task to exit, wait for it to close the socket
        s_ap_active = false;
        vTaskDelay(pdMS_TO_TICKS(200)); // DNS task exits within ~100ms
    }
    clear_ap_handoff();
    s_ap_active = true;
    ble_access_scan_stop();
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    notify_ap_status();

    wifi_config_t ap_cfg = { 0 };
    size_t ap_ssid_len = strnlen(s_ap_cfg.ssid, sizeof(ap_cfg.ap.ssid));
    memcpy(ap_cfg.ap.ssid, s_ap_cfg.ssid, ap_ssid_len);
    ap_cfg.ap.ssid_len      = (uint8_t)ap_ssid_len;
    ap_cfg.ap.channel       = 1;
    ap_cfg.ap.max_connection = 4;
    if (strlen(s_ap_cfg.password) >= 8) {
        strlcpy((char *)ap_cfg.ap.password, s_ap_cfg.password, sizeof(ap_cfg.ap.password));
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    xTaskCreate(dns_server_task, "dns_srv", 4096, NULL, 5, &s_dns_task_handle);
    ESP_LOGI(TAG, "AP '%s' (%s) started at 192.168.4.1",
             s_ap_cfg.ssid,
             strlen(s_ap_cfg.password) >= 8 ? "WPA2" : "open");
}

void wifi_stop_ap(void)
{
    if (!s_ap_active) return;
    clear_ap_handoff();
    s_ap_active = false; // signals DNS task to exit its loop
    esp_wifi_set_mode(WIFI_MODE_STA);
    ble_access_scan_start();
    notify_ap_status();
    ESP_LOGI(TAG, "AP stopped");
}

bool wifi_ap_is_active(void)
{
    return s_ap_active;
}

static void wifi_ap_load_defaults(wifi_ap_settings_t *cfg)
{
    if (!cfg) return;

    // Apply defaults first — SSID derived from last 3 bytes of AP MAC address
    cfg->enabled = false;
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) == ESP_OK) {
        snprintf(cfg->ssid, sizeof(cfg->ssid), "BBB-%02X%02X%02X",
                 mac[3], mac[4], mac[5]);
    } else {
        strlcpy(cfg->ssid, "BBB-Config", sizeof(cfg->ssid));
    }
    strlcpy(cfg->password, "12345678", sizeof(cfg->password));
}

static esp_err_t wifi_ap_read_config(wifi_ap_settings_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    wifi_ap_load_defaults(cfg);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("ap_cfg", NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    char val[4] = {};
    size_t len  = sizeof(val);
    err = nvs_get_str(nvs, "en", val, &len);
    if (err == ESP_OK) {
        cfg->enabled = (val[0] == '1');
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs);
        return err;
    }

    len = sizeof(cfg->ssid);
    err = nvs_get_str(nvs, "ssid", cfg->ssid, &len);
    if (err == ESP_OK && strlen(cfg->ssid) == 0) {
        strlcpy(cfg->ssid, "BBB-Config", sizeof(cfg->ssid)); // guard empty
    } else if (err != ESP_ERR_NVS_NOT_FOUND && err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }

    len = sizeof(cfg->password);
    err = nvs_get_str(nvs, "pass", cfg->password, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;

    nvs_close(nvs);
    return err;
}

void wifi_ap_load_config(wifi_ap_settings_t *cfg)
{
    esp_err_t err = wifi_ap_read_config(cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not load AP config: %s", esp_err_to_name(err));
    }
}

esp_err_t wifi_ap_save_config(const wifi_ap_settings_t *cfg)
{
    return wifi_ap_apply_config(cfg);
}

static esp_err_t wifi_ap_store_config(const wifi_ap_settings_t *cfg)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("ap_cfg", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_str(nvs, "en", cfg->enabled ? "1" : "0");
    if (err == ESP_OK) err = nvs_set_str(nvs, "ssid", cfg->ssid);
    if (err == ESP_OK) err = nvs_set_str(nvs, "pass", cfg->password);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static esp_err_t wifi_ap_apply_config(const wifi_ap_settings_t *cfg)
{
    esp_err_t err = wifi_ap_store_config(cfg);
    if (err != ESP_OK) return err;
    memcpy(&s_ap_cfg, cfg, sizeof(*cfg));
    return ESP_OK;
}

struct cJSON *wifi_config_export(wifi_export_view_t view)
{
    cJSON *wifi = cJSON_CreateObject();
    if (!wifi) return NULL;

    char ssid[33] = {0};
    char pass[65] = {0};
    esp_err_t err = backup_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not snapshot WiFi config for export: %s", esp_err_to_name(err));
        cJSON_Delete(wifi);
        return NULL;
    }

    if (view == WIFI_EXPORT_VIEW_BACKUP) {
        cJSON_AddStringToObject(wifi, "ssid", ssid);
        cJSON_AddStringToObject(wifi, "password", pass);
    } else {
        cJSON_AddStringToObject(wifi, "ssid", ssid);
        cJSON_AddBoolToObject(wifi, "ssid_set", ssid[0] != '\0');
        cJSON_AddBoolToObject(wifi, "password_set", pass[0] != '\0');
    }

    return wifi;
}

struct cJSON *wifi_ap_config_export(wifi_export_view_t view)
{
    (void)view;

    cJSON *ap = cJSON_CreateObject();
    if (!ap) return NULL;

    wifi_ap_settings_t cfg;
    esp_err_t err = wifi_ap_read_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not snapshot AP config for export: %s", esp_err_to_name(err));
        cJSON_Delete(ap);
        return NULL;
    }

    cJSON_AddBoolToObject(ap, "enabled", cfg.enabled);
    cJSON_AddStringToObject(ap, "ssid", cfg.ssid);
    cJSON_AddStringToObject(ap, "password", cfg.password);
    return ap;
}

esp_err_t wifi_scan_export(struct cJSON **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;

    wifi_scan_entry_t results[20];
    int n = 0;
    esp_err_t err = wifi_scan_get_results(results, 20, &n);
    if (err != ESP_OK) return err;

    cJSON *arr = cJSON_CreateArray();
    if (!arr) return ESP_ERR_NO_MEM;
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            cJSON_Delete(arr);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(item, "ssid", results[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", results[i].rssi);
        if (!cJSON_AddItemToArray(arr, item)) {
            cJSON_Delete(item);
            cJSON_Delete(arr);
            return ESP_ERR_NO_MEM;
        }
    }

    *out = arr;
    return ESP_OK;
}

struct cJSON *wifi_backup_export(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON *wifi = wifi_config_export(WIFI_EXPORT_VIEW_BACKUP);
    cJSON *ap = wifi_ap_config_export(WIFI_EXPORT_VIEW_BACKUP);
    if (!wifi || !ap) {
        if (wifi) cJSON_Delete(wifi);
        if (ap) cJSON_Delete(ap);
        cJSON_Delete(root);
        return NULL;
    }
    if (!cJSON_AddItemToObject(root, "wifi", wifi)) {
        cJSON_Delete(wifi);
        cJSON_Delete(ap);
        cJSON_Delete(root);
        return NULL;
    }
    if (!cJSON_AddItemToObject(root, "ap", ap)) {
        cJSON_Delete(ap);
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

esp_err_t wifi_backup_import(const struct cJSON *root_obj)
{
    cJSON *root = (cJSON *)root_obj;
    if (!root) return ESP_ERR_INVALID_ARG;

    cJSON *wifi = cJSON_GetObjectItem(root, "wifi");
    if (cJSON_IsObject(wifi)) {
        nvs_handle_t h;
        esp_err_t err = nvs_open("wifi", NVS_READWRITE, &h);
        if (err != ESP_OK) return err;

        cJSON *ssid = cJSON_GetObjectItem(wifi, "ssid");
        cJSON *pass = cJSON_GetObjectItem(wifi, "password");
        if (cJSON_IsString(ssid)) {
            err = nvs_set_str(h, "ssid", ssid->valuestring);
        }
        if (err == ESP_OK && cJSON_IsString(pass)) {
            err = nvs_set_str(h, "pass", pass->valuestring);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
        if (err != ESP_OK) return err;
    }

    cJSON *ap = cJSON_GetObjectItem(root, "ap");
    if (cJSON_IsObject(ap)) {
        wifi_ap_settings_t cfg;
        wifi_ap_load_config(&cfg);

        cJSON *enabled = cJSON_GetObjectItem(ap, "enabled");
        cJSON *ssid = cJSON_GetObjectItem(ap, "ssid");
        cJSON *pass = cJSON_GetObjectItem(ap, "password");
        if (cJSON_IsBool(enabled)) cfg.enabled = cJSON_IsTrue(enabled);
        if (cJSON_IsString(ssid)) strlcpy(cfg.ssid, ssid->valuestring, sizeof(cfg.ssid));
        if (cJSON_IsString(pass)) strlcpy(cfg.password, pass->valuestring, sizeof(cfg.password));
        esp_err_t err = wifi_ap_apply_config(&cfg);
        if (err != ESP_OK) return err;
    }

    return ESP_OK;
}

esp_err_t wifi_scan_get_results(wifi_scan_entry_t *results, int max_count, int *out_count)
{
    if (!results || max_count <= 0 || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        if (err == ESP_ERR_WIFI_STATE) {
            return ESP_ERR_INVALID_STATE;
        }
        return err;
    }

    uint16_t count = (uint16_t)max_count;
    wifi_ap_record_t *aps = malloc(sizeof(wifi_ap_record_t) * max_count);
    if (!aps) return ESP_ERR_NO_MEM;

    err = esp_wifi_scan_get_ap_records(&count, aps);
    if (err != ESP_OK) {
        free(aps);
        return err;
    }

    for (int i = 0; i < (int)count; i++) {
        strlcpy(results[i].ssid, (char *)aps[i].ssid, sizeof(results[i].ssid));
        results[i].rssi = aps[i].rssi;
    }

    free(aps);
    *out_count = (int)count;
    return ESP_OK;
}
