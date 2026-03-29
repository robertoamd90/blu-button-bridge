#pragma once

#include <stddef.h>
#include "esp_err.h"

typedef struct ota_upload_session ota_upload_session_t;

// Stores a staged GitHub OTA job for execution after reboot.
esp_err_t ota_manager_stage_github_job(const char *version_label,
                                       const char *download_url,
                                       const char *digest_hex);

// Starts OTA mode in a dedicated task when a staged GitHub OTA job is pending.
// Returns true when OTA mode took ownership of the current boot.
bool ota_manager_start_pending_job(void);

// Starts a manual OTA upload session against the next OTA partition.
// `image_size` is used to validate that the image fits in the target partition.
esp_err_t ota_manager_upload_begin(size_t image_size, ota_upload_session_t **out_session);

// Writes one firmware chunk into an active manual OTA upload session.
esp_err_t ota_manager_upload_write(ota_upload_session_t *session, const void *data, size_t len);

// Finalizes a manual OTA upload session and sets the new boot partition.
esp_err_t ota_manager_upload_finish(ota_upload_session_t *session);

// Aborts a manual OTA upload session and releases its resources.
void ota_manager_upload_abort(ota_upload_session_t *session);
