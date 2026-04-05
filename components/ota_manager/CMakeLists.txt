set(OTA_RELEASE_PROFILE_GENERATOR "${PROJECT_DIR}/scripts/generate_ota_release_profile_c.py")
set(OTA_RELEASE_PROFILE_INPUT "${PROJECT_DIR}/config/boards.json")
set(OTA_RELEASE_PROFILE_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(OTA_RELEASE_PROFILE_OUTPUT "${OTA_RELEASE_PROFILE_OUTPUT_DIR}/ota_release_profile_generated.c")

set(OTA_RELEASE_PROFILE_SOURCES)

if(NOT CMAKE_SCRIPT_MODE_FILE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${OTA_RELEASE_PROFILE_INPUT}"
        "${OTA_RELEASE_PROFILE_GENERATOR}"
    )
    file(MAKE_DIRECTORY "${OTA_RELEASE_PROFILE_OUTPUT_DIR}")
    execute_process(
        COMMAND python3 "${OTA_RELEASE_PROFILE_GENERATOR}" "${OTA_RELEASE_PROFILE_INPUT}" "${OTA_RELEASE_PROFILE_OUTPUT}"
        RESULT_VARIABLE OTA_RELEASE_PROFILE_RC
    )
    if(NOT OTA_RELEASE_PROFILE_RC EQUAL 0)
        message(FATAL_ERROR "Failed to generate OTA release profile source")
    endif()
    list(APPEND OTA_RELEASE_PROFILE_SOURCES "${OTA_RELEASE_PROFILE_OUTPUT}")
endif()

idf_component_register(
    SRCS "ota_manager.c" ${OTA_RELEASE_PROFILE_SOURCES}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nvs_flash app_update esp_http_client esp-tls mbedtls esp_system esp_timer wifi_manager
)
