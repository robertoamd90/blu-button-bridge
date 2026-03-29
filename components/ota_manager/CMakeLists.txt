idf_component_register(
    SRCS "ota_manager.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nvs_flash app_update esp_http_client esp-tls mbedtls esp_system esp_timer wifi_manager
)
