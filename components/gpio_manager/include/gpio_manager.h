#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define GPIO_ACTION_MAX 16
#define GPIO_SYSTEM_LED_GPIO 2

typedef enum {
    SYSTEM_LED_OFF = 0,
    SYSTEM_LED_AP_BLINK,
    SYSTEM_LED_WIFI_DISCONNECTED_HINT,
    SYSTEM_LED_MQTT_DISCONNECTED_HINT,
    SYSTEM_LED_BOOT_AP_HINT,
    SYSTEM_LED_BOOT_RESET_HINT,
} system_led_mode_t;

typedef void (*gpio_manager_ap_request_cb_t)(void);

typedef enum {
    GPIO_ACTION_SET_ON = 0,
    GPIO_ACTION_SET_OFF,
    GPIO_ACTION_TOGGLE,
} gpio_action_kind_t;

typedef struct {
    char     name[32];
    uint8_t  gpio_num;
    bool     idle_on;
    bool     active_low;
    uint8_t  action;             // gpio_action_kind_t
    uint32_t restore_delay_ms;   // 0 = persistent change
} gpio_action_t;

// Initializes the GPIO action subsystem, system LED handling, and BOOT monitor.
// Call once from app_main during startup.
void gpio_manager_init(void);

// Sets the reserved blue system LED mode.
void gpio_manager_set_system_led_mode(system_led_mode_t mode);

// Registers a callback invoked when the BOOT-button recovery flow requests AP mode.
void gpio_manager_set_boot_ap_callback(gpio_manager_ap_request_cb_t cb);

// Returns the list of GPIOs allowed for user-configured output actions.
// The returned GPIOs exclude the system LED and unsafe boot/flash pins.
int gpio_action_get_allowed_gpios(uint8_t *out, int max_count);

// Returns true if gpio_num is part of the allowed output whitelist.
bool gpio_action_gpio_allowed(uint8_t gpio_num);

// Returns the string form of a GPIO action kind: "on", "off", or "toggle".
const char *gpio_action_kind_str(gpio_action_kind_t kind);

// Parses "on", "off", or "toggle" into a gpio_action_kind_t.
bool gpio_action_kind_parse(const char *str, gpio_action_kind_t *out);

// Adds an output action in the first free slot.
// Returns ESP_OK and writes the slot index to out_idx on success.
esp_err_t gpio_action_add(const gpio_action_t *action, int *out_idx);

// Updates an existing output action slot.
esp_err_t gpio_action_update(int idx, const gpio_action_t *action);

// Deletes an output action slot and releases its GPIO.
esp_err_t gpio_action_delete(int idx);

// Reads an output action slot.
esp_err_t gpio_action_get(int idx, gpio_action_t *out);

// Triggers an output action slot.
esp_err_t gpio_action_trigger(int idx);
