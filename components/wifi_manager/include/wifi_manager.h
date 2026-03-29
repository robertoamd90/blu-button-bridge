#pragma once

#include <stddef.h>
#include <stdbool.h>

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

// Loads AP configuration from NVS (applies defaults if not found).
void wifi_ap_load_config(wifi_ap_settings_t *cfg);

// Saves AP configuration to NVS and updates the in-memory copy.
void wifi_ap_save_config(const wifi_ap_settings_t *cfg);

// Starts the configured SoftAP in APSTA mode.
// Also starts the DNS server for the captive portal.
void wifi_start_ap(void);

// Stops the AP and switches back to STA mode.
void wifi_stop_ap(void);

// Returns true if the AP is active.
bool wifi_ap_is_active(void);

// Reads the SSID saved in NVS (returns false if not configured).
bool wifi_get_ssid(char *buf, size_t len);

// Returns true if a password is saved in NVS, without exposing it.
bool wifi_get_password_set(void);

// Performs a WiFi scan and fills results[0..max_count-1].
// Returns the number of networks found (>= 0), or -1 on error.
int wifi_scan_get_results(wifi_scan_entry_t *results, int max_count);
