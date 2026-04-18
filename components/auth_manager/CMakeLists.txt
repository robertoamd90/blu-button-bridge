idf_component_register(
    SRCS "auth_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server espressif__cjson
    PRIV_REQUIRES freertos nvs_flash mbedtls
)
