#pragma once

#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"

#define AUTH_MANAGER_USERNAME_MAX 32
#define AUTH_MANAGER_PASSWORD_MAX 64

void auth_manager_init(void);
esp_err_t auth_manager_status(void);
bool auth_manager_is_enabled(void);
bool auth_manager_verify_credentials(const char *username, const char *password);

cJSON *auth_manager_config_export(void);
esp_err_t auth_manager_config_update_from_json(const cJSON *root, const char **out_error);

bool auth_manager_backup_export(cJSON *root);
esp_err_t auth_manager_backup_import(const cJSON *root, const char **out_error);
