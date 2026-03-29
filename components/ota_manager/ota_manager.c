#include <string.h>
#include <inttypes.h>
#include <stdlib.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "mbedtls/md.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_manager.h"
#include "ota_manager.h"

static const char *TAG = "ota_manager";
static const char *OTA_NS = "ota_manager";

#define OTA_HTTP_BUFFER_SIZE    8192
#define OTA_HTTP_TX_BUFFER_SIZE 1024
#define OTA_HTTP_MAX_REDIRECTS  5
#define OTA_VERSION_LEN         40
#define OTA_URL_LEN             512
#define OTA_DIGEST_HEX_LEN      65
#define OTA_ERROR_LEN           64
#define OTA_WAIT_WIFI_MS        30000
#define OTA_MAX_ATTEMPTS        3
#define OTA_TASK_STACK_SIZE     12288

typedef enum {
    OTA_JOB_STATUS_NONE = 0,
    OTA_JOB_STATUS_PENDING = 1,
    OTA_JOB_STATUS_RUNNING = 2,
    OTA_JOB_STATUS_FAILED = 3,
} ota_job_status_t;

typedef struct {
    ota_job_status_t status;
    uint8_t attempts;
    char version[OTA_VERSION_LEN];
    char download_url[OTA_URL_LEN];
    char digest_hex[OTA_DIGEST_HEX_LEN];
    char last_error[OTA_ERROR_LEN];
} ota_job_t;

typedef struct {
    esp_ota_handle_t ota;
    mbedtls_md_context_t sha_ctx;
    bool sha_started;
    size_t bytes_written;
    esp_err_t stream_err;
} ota_download_ctx_t;

struct ota_upload_session {
    const esp_partition_t *partition;
    esp_ota_handle_t ota;
};

static bool s_boot_job_loaded = false;
static bool s_boot_job_valid = false;
static ota_job_t s_boot_job = {0};

static void log_heap_snapshot(const char *label)
{
    ESP_LOGI(TAG, "%s: free_heap=%" PRIu32 " largest_block=%" PRIu32,
             label ? label : "heap",
             (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void sha256_digest_to_hex(const unsigned char digest[32], char *out_hex, size_t out_len)
{
    if (!out_hex || out_len < OTA_DIGEST_HEX_LEN) return;
    for (int i = 0; i < 32; i++) {
        snprintf(out_hex + (i * 2), out_len - (size_t)(i * 2), "%02x", digest[i]);
    }
}

static bool load_job(ota_job_t *job)
{
    if (!job) return false;
    memset(job, 0, sizeof(*job));

    nvs_handle_t nvs;
    if (nvs_open(OTA_NS, NVS_READONLY, &nvs) != ESP_OK) return false;

    uint8_t status = OTA_JOB_STATUS_NONE;
    esp_err_t err = nvs_get_u8(nvs, "status", &status);
    if (err == ESP_OK) {
        job->status = (ota_job_status_t)status;
    } else {
        uint8_t pending = 0;
        err = nvs_get_u8(nvs, "pending", &pending);
        if (err != ESP_OK || pending == 0) {
            nvs_close(nvs);
            return false;
        }
        job->status = OTA_JOB_STATUS_PENDING;
    }

    if (job->status == OTA_JOB_STATUS_NONE) {
        nvs_close(nvs);
        return false;
    }

    size_t len = sizeof(job->version);
    if (nvs_get_str(nvs, "version", job->version, &len) != ESP_OK) goto fail;
    len = sizeof(job->download_url);
    if (nvs_get_str(nvs, "url", job->download_url, &len) != ESP_OK) goto fail;
    len = sizeof(job->digest_hex);
    if (nvs_get_str(nvs, "digest", job->digest_hex, &len) != ESP_OK) goto fail;
    nvs_get_u8(nvs, "attempts", &job->attempts);
    len = sizeof(job->last_error);
    if (nvs_get_str(nvs, "last_error", job->last_error, &len) != ESP_OK) {
        job->last_error[0] = '\0';
    }

    nvs_close(nvs);
    return true;

fail:
    nvs_close(nvs);
    return false;
}

static void ensure_boot_job_loaded(void)
{
    if (s_boot_job_loaded) return;
    s_boot_job_valid = load_job(&s_boot_job);
    s_boot_job_loaded = true;
}

static bool has_pending_boot_job(void)
{
    ensure_boot_job_loaded();
    if (!s_boot_job_valid) return false;
    return s_boot_job.status == OTA_JOB_STATUS_PENDING ||
           s_boot_job.status == OTA_JOB_STATUS_RUNNING;
}

static esp_err_t persist_job_runtime_state(const ota_job_t *job)
{
    if (!job) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(OTA_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    bool pending = (job->status == OTA_JOB_STATUS_PENDING || job->status == OTA_JOB_STATUS_RUNNING);
    err = nvs_set_u8(nvs, "status", (uint8_t)job->status);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "pending", pending ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "attempts", job->attempts);
    if (err == ESP_OK) err = nvs_set_str(nvs, "last_error", job->last_error);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t ota_manager_stage_github_job(const char *version_label,
                                       const char *download_url,
                                       const char *digest_hex)
{
    if (!version_label || !download_url || !digest_hex) return ESP_ERR_INVALID_ARG;

    ota_job_t job = {
        .status = OTA_JOB_STATUS_PENDING,
        .attempts = 0,
    };
    strlcpy(job.version, version_label, sizeof(job.version));
    strlcpy(job.download_url, download_url, sizeof(job.download_url));
    strlcpy(job.digest_hex, digest_hex, sizeof(job.digest_hex));
    job.last_error[0] = '\0';

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(OTA_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    err = nvs_set_str(nvs, "version", job.version);
    if (err == ESP_OK) err = nvs_set_str(nvs, "url", job.download_url);
    if (err == ESP_OK) err = nvs_set_str(nvs, "digest", job.digest_hex);
    if (err == ESP_OK) err = nvs_set_str(nvs, "last_error", job.last_error);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "attempts", job.attempts);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "status", (uint8_t)job.status);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "pending", 1);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) {
        s_boot_job_loaded = false;
        s_boot_job_valid = false;
        memset(&s_boot_job, 0, sizeof(s_boot_job));
    }
    return err;
}

static void clear_pending_job(void)
{
    nvs_handle_t nvs;
    if (nvs_open(OTA_NS, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_erase_key(nvs, "status");
    nvs_set_u8(nvs, "pending", 0);
    nvs_set_u8(nvs, "attempts", 0);
    nvs_set_str(nvs, "last_error", "");
    nvs_commit(nvs);
    nvs_close(nvs);
    s_boot_job_loaded = false;
    s_boot_job_valid = false;
    memset(&s_boot_job, 0, sizeof(s_boot_job));
}

static esp_err_t mark_job_failure(ota_job_t *job, const char *reason)
{
    if (!job) return ESP_ERR_INVALID_ARG;

    if (reason) strlcpy(job->last_error, reason, sizeof(job->last_error));
    job->status = OTA_JOB_STATUS_FAILED;
    esp_err_t err = persist_job_runtime_state(job);
    if (err == ESP_OK) {
        s_boot_job = *job;
        s_boot_job_loaded = true;
        s_boot_job_valid = true;
    }
    return err;
}

esp_err_t ota_manager_upload_begin(size_t image_size, ota_upload_session_t **out_session)
{
    if (!out_session) return ESP_ERR_INVALID_ARG;
    *out_session = NULL;
    if (image_size == 0) return ESP_ERR_INVALID_SIZE;

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return ESP_ERR_NOT_FOUND;
    if (image_size > part->size) return ESP_ERR_INVALID_SIZE;

    ota_upload_session_t *session = calloc(1, sizeof(*session));
    if (!session) return ESP_ERR_NO_MEM;

    session->partition = part;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &session->ota);
    if (err != ESP_OK) {
        free(session);
        return err;
    }

    *out_session = session;
    return ESP_OK;
}

esp_err_t ota_manager_upload_write(ota_upload_session_t *session, const void *data, size_t len)
{
    if (!session || !data || len == 0) return ESP_ERR_INVALID_ARG;
    return esp_ota_write(session->ota, data, len);
}

esp_err_t ota_manager_upload_finish(ota_upload_session_t *session)
{
    if (!session) return ESP_ERR_INVALID_ARG;

    esp_err_t err = esp_ota_end(session->ota);
    if (err == ESP_OK) err = esp_ota_set_boot_partition(session->partition);
    free(session);
    return err;
}

void ota_manager_upload_abort(ota_upload_session_t *session)
{
    if (!session) return;
    esp_ota_abort(session->ota);
    free(session);
}

static esp_err_t ota_download_event_handler(esp_http_client_event_t *evt)
{
    ota_download_ctx_t *ctx = (ota_download_ctx_t *)evt->user_data;
    if (!ctx) return ESP_FAIL;

    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    if (evt->data_len <= 0) return ESP_OK;
    if (esp_http_client_get_status_code(evt->client) != 200) return ESP_OK;

    if (!ctx->sha_started) {
        mbedtls_md_init(&ctx->sha_ctx);
        const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        if (!info || mbedtls_md_setup(&ctx->sha_ctx, info, 0) != 0 ||
                mbedtls_md_starts(&ctx->sha_ctx) != 0) {
            ctx->stream_err = ESP_FAIL;
            return ESP_FAIL;
        }
        ctx->sha_started = true;
    }

    if (mbedtls_md_update(&ctx->sha_ctx, (const unsigned char *)evt->data,
                          (size_t)evt->data_len) != 0) {
        ctx->stream_err = ESP_FAIL;
        return ESP_FAIL;
    }

    esp_err_t err = esp_ota_write(ctx->ota, evt->data, evt->data_len);
    if (err != ESP_OK) {
        ctx->stream_err = err;
        return err;
    }

    ctx->bytes_written += (size_t)evt->data_len;
    return ESP_OK;
}

static esp_err_t ota_install_from_job(const ota_job_t *job)
{
    if (!job || !job->download_url[0] || !job->digest_hex[0]) return ESP_ERR_INVALID_ARG;

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return ESP_ERR_NOT_FOUND;

    log_heap_snapshot("OTA mode start");

    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) return err;

    ota_download_ctx_t ctx = {
        .ota = ota,
        .stream_err = ESP_OK,
    };

    esp_http_client_config_t cfg = {
        .url = job->download_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .buffer_size = OTA_HTTP_BUFFER_SIZE,
        .buffer_size_tx = OTA_HTTP_TX_BUFFER_SIZE,
        .max_redirection_count = OTA_HTTP_MAX_REDIRECTS,
        .event_handler = ota_download_event_handler,
        .user_data = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        esp_ota_abort(ota);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "User-Agent", "BluButtonBridge");

    err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    log_heap_snapshot("OTA mode after download");

    if (err != ESP_OK || ctx.stream_err != ESP_OK || status != 200 || ctx.bytes_written == 0) {
        if (ctx.sha_started) mbedtls_md_free(&ctx.sha_ctx);
        esp_ota_abort(ota);
        return (ctx.stream_err != ESP_OK) ? ctx.stream_err : ESP_FAIL;
    }

    unsigned char digest[32];
    char actual_hex[OTA_DIGEST_HEX_LEN];
    if (!ctx.sha_started || mbedtls_md_finish(&ctx.sha_ctx, digest) != 0) {
        if (ctx.sha_started) mbedtls_md_free(&ctx.sha_ctx);
        esp_ota_abort(ota);
        return ESP_FAIL;
    }
    mbedtls_md_free(&ctx.sha_ctx);

    sha256_digest_to_hex(digest, actual_hex, sizeof(actual_hex));
    if (strcmp(actual_hex, job->digest_hex) != 0) {
        ESP_LOGW(TAG, "OTA digest mismatch: expected %s got %s", job->digest_hex, actual_hex);
        esp_ota_abort(ota);
        return ESP_ERR_INVALID_CRC;
    }

    err = esp_ota_end(ota);
    if (err != ESP_OK) return err;

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) return err;

    return ESP_OK;
}

static esp_err_t wait_for_wifi_up(void)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(OTA_WAIT_WIFI_MS);
    while (xTaskGetTickCount() < deadline) {
        if (wifi_get_status() == WIFI_STATUS_UP) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t run_pending_github_job(void)
{
    ensure_boot_job_loaded();
    if (!s_boot_job_valid) return ESP_ERR_NOT_FOUND;

    ota_job_t job = s_boot_job;

    if (job.attempts >= OTA_MAX_ATTEMPTS) {
        ESP_LOGW(TAG, "Discarding staged OTA job %s after %u failed attempts",
                 job.version, (unsigned)job.attempts);
        strlcpy(job.last_error, "max_attempts", sizeof(job.last_error));
        job.status = OTA_JOB_STATUS_FAILED;
        persist_job_runtime_state(&job);
        s_boot_job = job;
        s_boot_job_loaded = true;
        s_boot_job_valid = true;
        return ESP_ERR_INVALID_STATE;
    }

    job.attempts++;
    job.status = OTA_JOB_STATUS_RUNNING;
    job.last_error[0] = '\0';
    esp_err_t err = persist_job_runtime_state(&job);
    if (err != ESP_OK) return err;
    s_boot_job = job;
    s_boot_job_loaded = true;
    s_boot_job_valid = true;

    ESP_LOGI(TAG, "Entering OTA mode for %s (attempt %u/%u)",
             job.version, (unsigned)job.attempts, (unsigned)OTA_MAX_ATTEMPTS);
    err = wait_for_wifi_up();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi did not come up in OTA mode");
        mark_job_failure(&job, "wifi_timeout");
        return err;
    }

    ESP_LOGI(TAG, "Installing staged GitHub update %s", job.version);
    err = ota_install_from_job(&job);
    if (err != ESP_OK) {
        mark_job_failure(&job, "install_failed");
        return err;
    }

    clear_pending_job();
    ESP_LOGI(TAG, "OTA mode completed, rebooting into updated firmware");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

static void ota_mode_task(void *arg)
{
    (void)arg;
    esp_err_t err = run_pending_github_job();
    ESP_LOGE(TAG, "OTA mode failed: %s", esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    vTaskDelete(NULL);
}

bool ota_manager_start_pending_job(void)
{
    if (!has_pending_boot_job()) return false;

    if (xTaskCreate(ota_mode_task, "ota_mode", OTA_TASK_STACK_SIZE, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start OTA mode task");
        return false;
    }

    return true;
}
