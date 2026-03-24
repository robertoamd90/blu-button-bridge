idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES wifi_manager mqtt_manager web_manager ble_access
    PRIV_REQUIRES nvs_flash
)
