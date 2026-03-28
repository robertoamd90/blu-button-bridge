idf_component_register(
    SRCS "web_manager.c"
    INCLUDE_DIRS "include"
    EMBED_FILES "index.html"
    PRIV_REQUIRES esp_http_server esp_http_client esp-tls wifi_manager mqtt_manager gpio_manager nvs_flash esp_system app_update mbedtls espressif__cjson ble_access
)
