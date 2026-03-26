idf_component_register(
    SRCS "gpio_manager.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nvs_flash esp_driver_gpio esp_system
)
