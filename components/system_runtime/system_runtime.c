#include <stdbool.h>
#include "system_runtime.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "gpio_manager.h"

static wifi_status_t s_wifi_status = WIFI_STATUS_NOT_CONFIG;
static mqtt_status_t s_mqtt_status = MQTT_STATUS_NOT_CONFIG;
static bool s_ap_active = false;

static void refresh_system_led(void)
{
    if (s_ap_active) {
        gpio_manager_set_system_led_mode(SYSTEM_LED_AP_BLINK);
        return;
    }

    if (s_wifi_status == WIFI_STATUS_CONNECTING || s_wifi_status == WIFI_STATUS_ERROR) {
        gpio_manager_set_system_led_mode(SYSTEM_LED_WIFI_DISCONNECTED_HINT);
        return;
    }

    if (s_wifi_status == WIFI_STATUS_UP &&
            (s_mqtt_status == MQTT_STATUS_CONNECTING || s_mqtt_status == MQTT_STATUS_ERROR)) {
        gpio_manager_set_system_led_mode(SYSTEM_LED_MQTT_DISCONNECTED_HINT);
        return;
    }

    gpio_manager_set_system_led_mode(SYSTEM_LED_OFF);
}

static void on_wifi_status_changed(wifi_status_t status)
{
    s_wifi_status = status;
    mqtt_set_network_available(status == WIFI_STATUS_UP);
    refresh_system_led();
}

static void on_wifi_ap_status_changed(bool active)
{
    s_ap_active = active;
    refresh_system_led();
}

static void on_mqtt_status_changed(mqtt_status_t status)
{
    s_mqtt_status = status;
    refresh_system_led();
}

void system_runtime_init(void)
{
    s_wifi_status = wifi_get_status();
    s_mqtt_status = mqtt_get_status();
    s_ap_active = wifi_ap_is_active();
    mqtt_set_status_callback(on_mqtt_status_changed);
    wifi_set_ap_status_callback(on_wifi_ap_status_changed);
    wifi_set_status_callback(on_wifi_status_changed);
    refresh_system_led();
}
