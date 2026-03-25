#include "nvs_flash.h"
#include "esp_log.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "web_manager.h"
#include "ble_access.h"
#include "gpio_manager.h"

// ── app_main ──────────────────────────────────────────────────────────────────

void app_main(void)
{
    esp_log_level_set("mqtt_client",    ESP_LOG_NONE);
    esp_log_level_set("transport_base", ESP_LOG_NONE);
    esp_log_level_set("esp-tls",        ESP_LOG_NONE);
    // Suppress NimBLE host stack verbosity
    esp_log_level_set("NimBLE",         ESP_LOG_WARN);
    esp_log_level_set("NimBLE_host",    ESP_LOG_WARN);
    esp_log_level_set("ble_att",        ESP_LOG_WARN);
    esp_log_level_set("ble_gap",        ESP_LOG_WARN);
    esp_log_level_set("ble_hs",         ESP_LOG_WARN);
    esp_log_level_set("ble_store",      ESP_LOG_WARN);

    // NVS is required by WiFi; if it is corrupted, erase and reinitialize it
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    gpio_manager_init();
    wifi_init();
    mqtt_init();
    ble_access_init();
    web_manager_init();
}
