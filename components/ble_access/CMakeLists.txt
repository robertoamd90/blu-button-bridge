idf_component_register(
    SRCS "ble_access.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nvs_flash bt mbedtls esp_security esp_timer mqtt_manager gpio_manager
)
