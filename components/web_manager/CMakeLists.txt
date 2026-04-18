idf_component_register(
    SRCS "web_manager.c"
    INCLUDE_DIRS "include"
    EMBED_FILES "index.html" "console.html" "wifi-handoff.html"
    PRIV_REQUIRES esp_http_server esp_http_client esp-tls wifi_manager mqtt_manager gpio_manager board_config nvs_flash esp_system app_update espressif__cjson ble_access console_manager ota_manager auth_manager
)
