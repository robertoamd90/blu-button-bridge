idf_component_register(
    SRCS "mqtt_manager.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nvs_flash espressif__mqtt
)
