#pragma once

#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

#define AUTH_MANAGER_USERNAME_MAX 32
#define AUTH_MANAGER_PASSWORD_MAX 64

esp_err_t auth_manager_init(void);
bool auth_manager_require(httpd_req_t *req);

cJSON *auth_manager_config_export(void);
esp_err_t auth_manager_config_update_from_json(const cJSON *root, const char **out_error);

bool auth_manager_backup_export(cJSON *root);
esp_err_t auth_manager_backup_import(const cJSON *root, const char **out_error);
