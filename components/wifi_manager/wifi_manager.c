#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "wifi_manager.h"

#include <stdlib.h>
#include "esp_netif.h"
#include "esp_mac.h"
#include "lwip/sockets.h"

#define WIFI_CONNECTED_BIT  BIT0

static EventGroupHandle_t   wifi_events;
static volatile wifi_status_t s_status = WIFI_STATUS_NOT_CONFIG;
static TimerHandle_t        s_reconnect_timer = NULL;
static volatile bool        s_ap_active = false;
static TaskHandle_t         s_dns_task_handle = NULL;

// AP config in RAM — ssid is overwritten at runtime by wifi_ap_load_config (MAC-based)
static wifi_ap_settings_t s_ap_cfg = {
    .enabled  = false,
    .ssid     = "BBB-000000",
    .password = "12345678",
};

// ── Event handlers ─────────────────────────────────────────────────────────────

static void on_wifi_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
    s_status = WIFI_STATUS_UP;
    xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    printf("WiFi IP: " IPSTR "\n", IP2STR(&event->ip_info.ip));
    // AP stopping is done from task context in wifi_connect() — safe to call esp_wifi_set_mode there
}

static void reconnect_timer_cb(TimerHandle_t t)
{
    if (s_status == WIFI_STATUS_DOWN || s_status == WIFI_STATUS_ERROR)
        esp_wifi_connect();
}

static void on_wifi_disconnected(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
    if (s_status == WIFI_STATUS_UP || s_status == WIFI_STATUS_ERROR) {
        s_status = WIFI_STATUS_DOWN;
        xTimerStart(s_reconnect_timer, 0);
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

// Connects to a network; blocks until IP acquired or timeout (10 s).
static void wifi_connect(const char *ssid, const char *pass)
{
    // Set DOWN before disconnect so on_wifi_disconnected won't restart the reconnect timer
    xTimerStop(s_reconnect_timer, 0);
    s_status = WIFI_STATUS_DOWN;
    xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
    esp_wifi_disconnect();            // no-op if not connected; ensures clean state
    vTaskDelay(pdMS_TO_TICKS(300));   // let the disconnect event settle

    wifi_config_t cfg = {};
    strncpy((char *)cfg.sta.ssid,     ssid, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT,
                                           false, true, pdMS_TO_TICKS(10000));
    if (bits & WIFI_CONNECTED_BIT) {
        printf("WiFi connected: %s\n", ssid);
        // Stop AP only if it was auto-started (not when user forced it always-on)
        if (s_ap_active && !s_ap_cfg.enabled) {
            wifi_stop_ap();
        }
    } else {
        s_status = WIFI_STATUS_ERROR;
        printf("WiFi: connection failed\n");
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void wifi_connect_api(const char *ssid, const char *pass)
{
    // If pass is empty, load the existing NVS password to keep it
    char pass_to_use[65] = {};
    bool keep_existing_pass = false;
    if (strlen(pass) == 0) {
        char saved_ssid[33] = {};
        char saved_pass[65] = {};
        if (load_credentials(saved_ssid, sizeof(saved_ssid), saved_pass, sizeof(saved_pass)) &&
                strcmp(saved_ssid, ssid) == 0 &&
                strlen(saved_pass) > 0) {
            keep_existing_pass = true;
            strlcpy(pass_to_use, saved_pass, sizeof(pass_to_use));
        }
    } else {
        strncpy(pass_to_use, pass, sizeof(pass_to_use) - 1);
    }

    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "ssid", ssid);
        if (strlen(pass) > 0) {
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

void wifi_disconnect(void)
{
    s_status = WIFI_STATUS_DISABLED;
    xTimerStop(s_reconnect_timer, 0);
    esp_wifi_disconnect();
}

void wifi_clean_credentials(void)
{
    s_status = WIFI_STATUS_NOT_CONFIG;
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
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    wifi_events = xEventGroupCreate();
    s_reconnect_timer = xTimerCreate("wifi_rc", pdMS_TO_TICKS(5000), pdFALSE, NULL, reconnect_timer_cb);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP,        on_wifi_got_ip,        NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi_disconnected,  NULL);

    // Application logic: load config, start AP and/or connect
    wifi_ap_load_config(&s_ap_cfg);

    char ssid[33] = {};
    char pass[65] = {};
    bool has_creds = load_credentials(ssid, sizeof(ssid), pass, sizeof(pass));

    if (s_ap_cfg.enabled || !has_creds) {
        wifi_start_ap();
    }

    if (has_creds) {
        printf("WiFi: connecting to %s\n", ssid);
        wifi_connect(ssid, pass);
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
        uint8_t resp[512];
        memcpy(resp, buf, len);
        resp[2] = 0x81; // QR=1, AA=1
        resp[3] = 0x80; // RA=1, RCODE=0
        resp[6] = 0; resp[7] = 1; // ANCOUNT=1

        // Skip question section: QNAME + QTYPE + QCLASS
        int pos = 12;
        while (pos < len && buf[pos] != 0) pos += buf[pos] + 1;
        pos += 5; // null label + QTYPE(2) + QCLASS(2)

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
    s_ap_active = true;
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    wifi_config_t ap_cfg = { 0 };
    strncpy((char *)ap_cfg.ap.ssid, s_ap_cfg.ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len      = (uint8_t)strlen(s_ap_cfg.ssid);
    ap_cfg.ap.channel       = 1;
    ap_cfg.ap.max_connection = 4;
    if (strlen(s_ap_cfg.password) >= 8) {
        strncpy((char *)ap_cfg.ap.password, s_ap_cfg.password, sizeof(ap_cfg.ap.password) - 1);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    xTaskCreate(dns_server_task, "dns_srv", 4096, NULL, 5, &s_dns_task_handle);
    printf("WiFi: AP '%s' (%s) started at 192.168.4.1\n",
           s_ap_cfg.ssid,
           strlen(s_ap_cfg.password) >= 8 ? "WPA2" : "open");
}

void wifi_stop_ap(void)
{
    if (!s_ap_active) return;
    s_ap_active = false; // signals DNS task to exit its loop
    esp_wifi_set_mode(WIFI_MODE_STA);
    printf("WiFi: AP stopped\n");
}

bool wifi_ap_is_active(void)
{
    return s_ap_active;
}

void wifi_ap_load_config(wifi_ap_settings_t *cfg)
{
    // Apply defaults first — SSID derived from last 3 bytes of AP MAC address
    cfg->enabled = false;
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) == ESP_OK) {
        snprintf(cfg->ssid, sizeof(cfg->ssid), "BBB-%02X%02X%02X",
                 mac[3], mac[4], mac[5]);
    } else {
        strncpy(cfg->ssid, "BBB-Config", sizeof(cfg->ssid) - 1);
    }
    strncpy(cfg->password, "12345678", sizeof(cfg->password) - 1);

    nvs_handle_t nvs;
    if (nvs_open("ap_cfg", NVS_READONLY, &nvs) != ESP_OK) return;

    char val[4] = {};
    size_t len  = sizeof(val);
    if (nvs_get_str(nvs, "en", val, &len) == ESP_OK)
        cfg->enabled = (val[0] == '1');

    len = sizeof(cfg->ssid);
    if (nvs_get_str(nvs, "ssid", cfg->ssid, &len) == ESP_OK && strlen(cfg->ssid) == 0)
        strncpy(cfg->ssid, "BBB-Config", sizeof(cfg->ssid) - 1); // guard empty

    len = sizeof(cfg->password);
    nvs_get_str(nvs, "pass", cfg->password, &len);

    nvs_close(nvs);
}

void wifi_ap_save_config(const wifi_ap_settings_t *cfg)
{
    memcpy(&s_ap_cfg, cfg, sizeof(*cfg));

    nvs_handle_t nvs;
    if (nvs_open("ap_cfg", NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_str(nvs, "en",   cfg->enabled ? "1" : "0");
    nvs_set_str(nvs, "ssid", cfg->ssid);
    nvs_set_str(nvs, "pass", cfg->password);
    nvs_commit(nvs);
    nvs_close(nvs);
}

bool wifi_get_ssid(char *buf, size_t len)
{
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) != ESP_OK) return false;
    bool ok = (nvs_get_str(nvs, "ssid", buf, &len) == ESP_OK && strlen(buf) > 0);
    nvs_close(nvs);
    return ok;
}

bool wifi_get_password_set(void)
{
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) != ESP_OK) return false;
    char pass[65] = {};
    size_t len = sizeof(pass);
    bool has = (nvs_get_str(nvs, "pass", pass, &len) == ESP_OK && strlen(pass) > 0);
    nvs_close(nvs);
    return has;
}

int wifi_scan_get_results(wifi_scan_entry_t *results, int max_count)
{
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) return -1;

    uint16_t count = (uint16_t)max_count;
    wifi_ap_record_t *aps = malloc(sizeof(wifi_ap_record_t) * max_count);
    if (!aps) return -1;

    if (esp_wifi_scan_get_ap_records(&count, aps) != ESP_OK) {
        free(aps);
        return -1;
    }

    for (int i = 0; i < (int)count; i++) {
        strncpy(results[i].ssid, (char *)aps[i].ssid, sizeof(results[i].ssid) - 1);
        results[i].ssid[sizeof(results[i].ssid) - 1] = '\0';
        results[i].rssi = aps[i].rssi;
    }

    free(aps);
    return (int)count;
}
