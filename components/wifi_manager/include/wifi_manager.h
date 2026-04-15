#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

struct cJSON;

#define WIFI_STATUS_LIST \
    X(NOT_CONFIG, "not config") \
    X(DISABLED,   "disabled")   \
    X(CONNECTING, "connecting") \
    X(ERROR,      "error")      \
    X(UP,         "up")

typedef enum {
#define X(name, str) WIFI_STATUS_##name,
    WIFI_STATUS_LIST
#undef X
} wifi_status_t;

typedef void (*wifi_status_cb_t)(wifi_status_t status);
typedef void (*wifi_ap_status_cb_t)(bool active);

const char *wifi_status_str(wifi_status_t s);

// Initializes the WiFi stack and starts any saved STA/AP connectivity in the background.
// Call once from app_main.
void wifi_init(void);

// Returns the current WiFi state.
wifi_status_t wifi_get_status(void);

// Returns the current STA IPv4 address when one is assigned.
bool wifi_get_sta_ip(char *buf, size_t len);

// Returns true when the most recent join attempt failed with a config-related error.
bool wifi_get_error_latched(void);

// Returns true when the AP is active and auto-managed, so the web UI can offer the temporary handoff page.
bool wifi_should_offer_ap_handoff(void);

// Arms the temporary AP handoff grace window used by the web UI.
// Returns true when the next successful join should delay AP shutdown briefly.
bool wifi_arm_ap_handoff(void);

// Completes an in-progress AP handoff and closes the auto-managed AP immediately.
void wifi_complete_ap_handoff(void);

// Registers a callback invoked whenever the WiFi connection status changes.
void wifi_set_status_callback(wifi_status_cb_t cb);

// Registers a callback invoked whenever the SoftAP active state changes.
void wifi_set_ap_status_callback(wifi_ap_status_cb_t cb);

// Disconnects from WiFi (credentials remain stored in NVS).
void wifi_disconnect(void);

// Erases WiFi credentials from NVS.
void wifi_clean_credentials(void);

// Saves credentials to NVS and starts connecting in the background (for programmatic use, e.g. web API).
// If password_provided is false, preserves the password already stored in NVS when possible.
// If password_provided is true, pass may be empty to configure an open network.
void wifi_connect_api(const char *ssid, const char *pass, bool password_provided);

// Result entry for a WiFi scan.
typedef struct {
    char ssid[33];
    int  rssi;
} wifi_scan_entry_t;

// Access Point configuration.
typedef struct {
    bool  enabled;       // true = AP always on; false = automatic management
    char  ssid[33];      // network name (default "BBB-XXXXXX")
    char  password[65];  // WPA2 password (min 8 chars); empty = open network
} wifi_ap_settings_t;

typedef enum {
    WIFI_EXPORT_VIEW_FE = 0,
    WIFI_EXPORT_VIEW_BACKUP = 1,
} wifi_export_view_t;

// Loads AP configuration from NVS (applies defaults if not found).
void wifi_ap_load_config(wifi_ap_settings_t *cfg);

// Saves AP configuration to NVS and updates the in-memory copy.
esp_err_t wifi_ap_save_config(const wifi_ap_settings_t *cfg);

// Starts the configured SoftAP in APSTA mode.
// Also starts the DNS server for the captive portal.
void wifi_start_ap(void);

// Stops the AP and switches back to STA mode.
void wifi_stop_ap(void);

// Returns true if the AP is active.
bool wifi_ap_is_active(void);

// Exports the STA WiFi config payload for the requested consumer view.
// `WIFI_EXPORT_VIEW_FE` redacts the password and adds FE-only flags.
// `WIFI_EXPORT_VIEW_BACKUP` includes the full persisted backup fields.
struct cJSON *wifi_config_export(wifi_export_view_t view);

// Exports the AP config payload for the requested consumer view.
struct cJSON *wifi_ap_config_export(wifi_export_view_t view);

// Exports the WiFi scan result array used by the web API.
// Returns ESP_OK and stores a caller-owned cJSON array in *out on success.
esp_err_t wifi_scan_export(struct cJSON **out);

// Exports the full WiFi module backup payload (`wifi` and `ap`).
// Returns a cJSON object owned by the caller, or NULL on failure.
struct cJSON *wifi_backup_export(void);

// Applies the WiFi module backup payload from a top-level backup object.
esp_err_t wifi_backup_import(const struct cJSON *root);

// Performs a WiFi scan and fills results[0..max_count-1].
// Returns ESP_OK on success and writes the number of networks to out_count.
esp_err_t wifi_scan_get_results(wifi_scan_entry_t *results, int max_count, int *out_count);
