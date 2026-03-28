#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define MQTT_STATUS_LIST \
    X(NOT_CONFIG,  "not config")  \
    X(DISABLED,    "disabled")    \
    X(WAITING_NET, "waiting net") \
    X(CONNECTING,  "connecting")  \
    X(UP,          "up")

typedef enum {
#define X(name, str) MQTT_STATUS_##name,
    MQTT_STATUS_LIST
#undef X
} mqtt_status_t;

const char *mqtt_status_str(mqtt_status_t s);

// Callback invoked on every message received on a subscribed topic.
// topic and payload are valid only during the call — copy them if needed later.
typedef void (*mqtt_message_cb_t)(const char *topic, const char *payload, int payload_len);
typedef void (*mqtt_status_cb_t)(mqtt_status_t status);

// Connects to a broker with explicit parameters and continues reconnecting in the background.
// Requires WiFi to be configured; connectivity may come up later.
void mqtt_connect_broker(const char *host, uint32_t port, const char *username,
                         const char *password, bool use_tls);

// Initializes MQTT: auto-connects to the broker using saved NVS credentials.
// Loads runtime state and saved configuration without blocking boot.
// Call once from app_main.
void mqtt_init(void);

// Informs MQTT whether the network is currently available.
// When the network comes up, MQTT may start or resume a background connection attempt.
void mqtt_set_network_available(bool available);

// Disconnects from the broker (credentials remain in NVS).
void mqtt_disconnect(void);

// Clears all MQTT credentials from NVS.
void mqtt_clean_credentials(void);

// Publishes payload to topic with QoS 1.
// Returns 0 on success, -1 if not connected or error.
int mqtt_publish(const char *topic, const char *payload);

// Subscribes to a topic (QoS 1) and registers a message callback.
// Updates the callback if the topic is already registered. Max 8 subscriptions.
void mqtt_subscribe(const char *topic, mqtt_message_cb_t cb);

// Returns the current MQTT connection status.
mqtt_status_t mqtt_get_status(void);

// Registers a callback invoked whenever the MQTT connection status changes.
void mqtt_set_status_callback(mqtt_status_cb_t cb);

// Saves credentials to NVS and starts connecting in the background (for programmatic use, e.g. web API).
// If password_provided is false, keeps the existing NVS password.
// If password_provided is true, password may be empty to clear the saved password.
void mqtt_connect_api(const char *host, uint32_t port,
                      const char *username, const char *password, bool use_tls,
                      bool password_provided);

// Reads saved MQTT config from NVS without exposing the password.
// has_pass is true if a password is stored in NVS.
// Returns false if no credentials are configured.
bool mqtt_get_saved_config(char *host, size_t host_len, uint32_t *port,
                           char *user, size_t user_len, bool *use_tls, bool *has_pass);

// ── MQTT Actions ─────────────────────────────────────────────────────────────
// Named MQTT publish actions that can be triggered by BLE button events.
// Up to 16 slots (0–15); slot is free when name[0] == '\0'.

#define MQTT_MAX_ACTIONS 16

typedef struct {
    char name[32];     // human-readable label; empty = slot is free
    char topic[64];
    char payload[32];
} mqtt_action_t;

// Finds the first free slot, writes the action, saves to NVS.
// Returns the slot index (0–15) on success, -1 if full or name is empty.
int       mqtt_action_add(const mqtt_action_t *a);

// Overwrites slot idx. Returns ESP_ERR_NOT_FOUND if the slot is free.
esp_err_t mqtt_action_update(int idx, const mqtt_action_t *a);

// Clears slot idx (marks it free) and saves. Returns ESP_ERR_NOT_FOUND if already free.
esp_err_t mqtt_action_delete(int idx);

// Copies slot idx into *out. Returns ESP_ERR_NOT_FOUND if the slot is free.
esp_err_t mqtt_action_get(int idx, mqtt_action_t *out);

// Publishes the action at slot idx. Silently skips if the slot is free or MQTT is down.
esp_err_t mqtt_action_trigger(int idx);
