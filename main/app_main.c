#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "web_manager.h"
#include "ble_access.h"
#include "gpio_manager.h"
#include "system_runtime.h"
#include "console_manager.h"
#include "ota_manager.h"

// ── app_main ──────────────────────────────────────────────────────────────────

static void configure_log_levels(void)
{
    // Silence noisy library logs to keep the serial monitor readable.
    // To re-enable for debugging, change ESP_LOG_NONE → ESP_LOG_DEBUG
    // and ESP_LOG_WARN → ESP_LOG_DEBUG, then rebuild.
    esp_log_level_set("mqtt_client",    ESP_LOG_NONE);
    esp_log_level_set("transport_base", ESP_LOG_NONE);
    esp_log_level_set("esp-tls",        ESP_LOG_NONE);
    esp_log_level_set("NimBLE",         ESP_LOG_WARN);
    esp_log_level_set("NimBLE_host",    ESP_LOG_WARN);
    esp_log_level_set("ble_att",        ESP_LOG_WARN);
    esp_log_level_set("ble_gap",        ESP_LOG_WARN);
    esp_log_level_set("ble_hs",         ESP_LOG_WARN);
    esp_log_level_set("ble_store",      ESP_LOG_WARN);
}

static void init_nvs_or_die(void)
{
    // NVS is required by WiFi; if it is corrupted, erase and reinitialize it.
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
}

void app_main(void)
{
    configure_log_levels();
    init_nvs_or_die();

    console_manager_init();
    gpio_manager_init();
    wifi_init();

    if (ota_manager_start_pending_job()) {
        return;
    }

    mqtt_init();
    system_runtime_init();
    ble_access_init();
    web_manager_init();
}
