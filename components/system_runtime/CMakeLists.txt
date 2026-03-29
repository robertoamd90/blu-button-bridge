idf_component_register(
    SRCS "system_runtime.c"
    INCLUDE_DIRS "include"
    REQUIRES wifi_manager mqtt_manager gpio_manager
)
