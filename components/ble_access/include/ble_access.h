#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define BLE_ACCESS_MAX_DEVICES 8

// Registered BTHome v2 device.
// Per-event fields are bitmasks: bit N set means trigger the action at slot N.
typedef struct {
    uint8_t  mac[6];           // BLE address, little-endian (NimBLE format)
    uint8_t  key[16];          // AES-128 decryption key
    uint32_t last_counter;     // Anti-replay: last accepted counter value
    char     label[32];
    bool     enabled;
    uint16_t single_press;     // bitmask of MQTT action slots to trigger
    uint16_t double_press;
    uint16_t triple_press;
    uint16_t long_press;
    uint16_t gpio_single_press; // bitmask of GPIO action slots to trigger
    uint16_t gpio_double_press;
    uint16_t gpio_triple_press;
    uint16_t gpio_long_press;
} ble_device_t;

// Initialises NimBLE and starts passive scanning. Call once from app_main.
void      ble_access_init(void);

// Stops BLE scanning. Safe to call before ble_access_init().
void      ble_access_scan_stop(void);

// Resumes BLE scanning. Safe to call before ble_access_init().
void      ble_access_scan_start(void);

// Enters registration mode: the next unseen BTHome device is captured as pending.
esp_err_t ble_access_register_start(void);

// Confirms registration with the given MAC, AES key, and label.
// Saves to NVS and exits registration mode.
esp_err_t ble_access_register_confirm(const uint8_t mac[6], const uint8_t key[16], const char *label);

// Exits registration mode without adding a device.
void      ble_access_register_cancel(void);

// Returns true if registration mode is active.
bool      ble_access_is_registering(void);

// Returns true if a BTHome device was seen during registration.
// If true, out_mac (6 bytes) receives its address in NimBLE format.
bool      ble_access_has_pending_mac(uint8_t out_mac[6]);

// Copies up to max_count registered devices into out[]. Returns actual count.
int       ble_access_get_devices(ble_device_t *out, int max_count);

// Copies a single device by MAC into *out. Returns ESP_OK or ESP_ERR_NOT_FOUND.
esp_err_t ble_access_get_device_by_mac(const uint8_t mac[6], ble_device_t *out);

// Updates label, enabled flag, and event bitmasks for the device with the given MAC.
// The key field in *updated is ignored — use ble_access_device_update_key() to change it.
esp_err_t ble_access_device_update(const uint8_t mac[6], const ble_device_t *updated);

// Replaces the AES-128 key for the device with the given MAC.
// Re-imports the PSA key. On any failure the old key is fully restored (rollback).
// Returns ESP_OK, ESP_ERR_NOT_FOUND, or ESP_FAIL (bad key / PSA error).
esp_err_t ble_access_device_update_key(const uint8_t mac[6], const uint8_t new_key[16]);

// Re-imports the currently stored key into PSA for the device with the given MAC.
// Useful when the persisted key is still correct but the local PSA state is missing.
esp_err_t ble_access_device_reimport_key(const uint8_t mac[6]);

// Returns true if the device exists but its key could not be imported into the
// local PSA crypto engine. This is a controller-side problem, not proof that the
// remote BLE key is wrong.
bool      ble_access_has_key_import_error(const uint8_t mac[6]);

// Returns true if the device has a valid PSA key imported locally, but repeated
// decrypt attempts failed for received advertisements. This usually indicates
// the configured BLE key is out of sync with the remote device.
bool      ble_access_has_decrypt_error(const uint8_t mac[6]);

// Removes the device with the given MAC and saves to NVS.
esp_err_t ble_access_device_delete(const uint8_t mac[6]);
