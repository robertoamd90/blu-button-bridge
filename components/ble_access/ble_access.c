#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "psa/crypto.h"
#include "mqtt_manager.h"
#include "gpio_manager.h"
#include "ble_access.h"

static const char *TAG = "ble_access";

#define NVS_NS          "ble_access"
#define BTHOME_UUID_LO  0xD2   // 0xFCD2 in little-endian
#define BTHOME_UUID_HI  0xFC
#define BTN_OBJ_ID      0x3A   // BTHome button object
#define DECRYPT_ERROR_THRESHOLD 3
#define BLE_ACCESS_MAX_PENDING_SERVICE_DATA 64
#define BLE_ACCESS_MAX_DECRYPTED_PAYLOAD    64

// ── State ─────────────────────────────────────────────────────────────────────

static ble_device_t         s_devices[BLE_ACCESS_MAX_DEVICES];
typedef struct {
    int64_t last_event_ms;
    uint8_t last_event;
} ble_button_runtime_t;
typedef struct {
    int64_t             last_seen_ms;
    uint8_t             battery_percent;
    bool                battery_known;
    ble_button_runtime_t buttons[BLE_ACCESS_MAX_BUTTONS];
} ble_device_runtime_t;
typedef struct {
    bool    has_battery_percent;
    uint8_t battery_percent;
    uint8_t button_count;
    uint8_t button_events[BLE_ACCESS_MAX_BUTTONS];
} ble_payload_parse_t;
typedef struct {
    uint8_t       device_info;
    const uint8_t *counter_bytes;
    const uint8_t *enc_data;
    const uint8_t *mic;
    size_t        enc_len;
    uint32_t      counter;
} ble_service_data_frame_t;
static ble_device_runtime_t s_runtime[BLE_ACCESS_MAX_DEVICES];
static psa_key_id_t         s_psa_keys[BLE_ACCESS_MAX_DEVICES]; // cached PSA key handles
static bool                 s_key_import_errors[BLE_ACCESS_MAX_DEVICES];
static bool                 s_decrypt_errors[BLE_ACCESS_MAX_DEVICES];
static uint8_t              s_decrypt_failures[BLE_ACCESS_MAX_DEVICES];
static bool                 s_nvs_dirty    = false;
static TimerHandle_t        s_nvs_timer    = NULL;
static int                  s_count        = 0;
static bool                 s_registering  = false;
static bool                 s_has_pending  = false;
static bool                 s_scan_enabled = true;
static bool                 s_ble_ready    = false;
static bool                 s_init_failed  = false;
static uint8_t              s_pending_mac[6];
static uint8_t              s_pending_service_data[BLE_ACCESS_MAX_PENDING_SERVICE_DATA];
static uint8_t              s_pending_service_data_len = 0;
static SemaphoreHandle_t    s_mutex;

// ── NVS ───────────────────────────────────────────────────────────────────────

static esp_err_t nvs_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "count", (uint8_t)s_count);
    for (int i = 0; i < s_count && err == ESP_OK; i++) {
        char key[16];
        snprintf(key, sizeof(key), "dev_%d", i);
        err = nvs_set_blob(h, key, &s_devices[i], sizeof(ble_device_t));
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(err));
    return err;
}

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t count = 0;
    nvs_get_u8(h, "count", &count);
    int n = count > BLE_ACCESS_MAX_DEVICES ? BLE_ACCESS_MAX_DEVICES : count;
    int loaded = 0;
    for (int i = 0; i < n; i++) {
        char key[16];
        snprintf(key, sizeof(key), "dev_%d", i);
        size_t sz = sizeof(ble_device_t);
        if (nvs_get_blob(h, key, &s_devices[loaded], &sz) == ESP_OK &&
                sz == sizeof(ble_device_t)) {
            loaded++;
        }
    }
    s_count = loaded;
    nvs_close(h);
}

static void nvs_flush_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_nvs_dirty) {
        if (nvs_save() == ESP_OK) {
            s_nvs_dirty = false;
        } else if (s_nvs_timer) {
            xTimerReset(s_nvs_timer, 0);
        }
    }
    xSemaphoreGive(s_mutex);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

static bool mac_equal(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) == 0;
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static int find_device_index_locked(const uint8_t mac[6])
{
    for (int i = 0; i < s_count; i++) {
        if (mac_equal(s_devices[i].mac, mac)) return i;
    }
    return -1;
}

// Returns the data length for known BTHome v2 object IDs, -1 for unknown.
static int bthome_obj_len(uint8_t obj_id)
{
    switch (obj_id) {
        case 0x00: return 1;  // Packet ID
        case 0x01: return 1;  // Battery %
        case 0x02: return 2;  // Temperature (0.01 °C)
        case 0x03: return 2;  // Humidity (0.01 %)
        case 0x0C: return 2;  // Voltage (0.001 V)
        case 0x12: return 2;  // CO2 (ppm)
        case 0x2D: return 1;  // Motion (bool)
        case 0x3A: return 1;  // Button event
        case 0x3C: return 1;  // Dimmer
        default:   return -1;
    }
}

static uint16_t action_map_get_mask(const ble_button_action_map_t *map, uint8_t event)
{
    if (!map) return 0;
    switch (event) {
        case BLE_BUTTON_EVENT_PRESS:        return map->single_press;
        case BLE_BUTTON_EVENT_DOUBLE_PRESS: return map->double_press;
        case BLE_BUTTON_EVENT_TRIPLE_PRESS: return map->triple_press;
        case BLE_BUTTON_EVENT_LONG_PRESS:   return map->long_press;
        default: return 0;
    }
}

static uint16_t get_mqtt_event_mask(const ble_device_t *dev, uint8_t button_idx, uint8_t event)
{
    if (!dev || button_idx >= BLE_ACCESS_MAX_BUTTONS) return 0;
    return action_map_get_mask(&dev->buttons[button_idx].mqtt, event);
}

static uint16_t get_gpio_event_mask(const ble_device_t *dev, uint8_t button_idx, uint8_t event)
{
    if (!dev || button_idx >= BLE_ACCESS_MAX_BUTTONS) return 0;
    return action_map_get_mask(&dev->buttons[button_idx].gpio, event);
}

const char *ble_button_event_str(uint8_t event)
{
    switch (event) {
        case BLE_BUTTON_EVENT_PRESS:             return "press";
        case BLE_BUTTON_EVENT_DOUBLE_PRESS:      return "double_press";
        case BLE_BUTTON_EVENT_TRIPLE_PRESS:      return "triple_press";
        case BLE_BUTTON_EVENT_LONG_PRESS:        return "long_press";
        case BLE_BUTTON_EVENT_BUTTON_HOLD:
        case BLE_BUTTON_EVENT_BUTTON_HOLD_LEGACY:return "button_hold";
        default:                                 return "unknown";
    }
}

// Locates the BTHome service data in the raw advertisement payload.
// Returns pointer to the service data content (starting at UUID bytes) and sets *out_len.
static bool find_bthome_sd(const uint8_t *adv, uint8_t adv_len,
                            const uint8_t **out_sd, uint8_t *out_len)
{
    uint8_t i = 0;
    while (i < adv_len) {
        uint8_t ad_len = adv[i];
        if (ad_len == 0 || i + ad_len >= adv_len) break;
        uint8_t ad_type = adv[i + 1];
        // Service Data — 16-bit UUID (0x16), needs at least UUID(2) + device_info(1)
        if (ad_type == 0x16 && ad_len >= 3) {
            if (adv[i + 2] == BTHOME_UUID_LO && adv[i + 3] == BTHOME_UUID_HI) {
                *out_sd  = &adv[i + 2];     // starts at UUID
                *out_len = ad_len - 1;       // excludes the AD type byte
                return true;
            }
        }
        i += ad_len + 1;
    }
    return false;
}

static bool parse_service_data_frame(const uint8_t *sd, size_t sd_len, ble_service_data_frame_t *out)
{
    if (!sd || !out || sd_len < 11) return false;
    if (sd[0] != BTHOME_UUID_LO || sd[1] != BTHOME_UUID_HI) return false;

    uint8_t device_info = sd[2];
    if (!(device_info & 0x01)) return false;   // only encrypted Shelly BTHome packets

    memset(out, 0, sizeof(*out));
    out->device_info   = device_info;
    out->counter_bytes = &sd[sd_len - 8];
    out->mic           = &sd[sd_len - 4];
    out->enc_data      = &sd[3];
    out->enc_len       = sd_len - 2 - 1 - 4 - 4;
    if (out->enc_len == 0 || out->enc_len > BLE_ACCESS_MAX_DECRYPTED_PAYLOAD) return false;

    memcpy(&out->counter, out->counter_bytes, sizeof(out->counter));
    return true;
}

static void build_bthome_nonce(const uint8_t mac[6],
                               uint8_t device_info,
                               const uint8_t counter_bytes[4],
                               uint8_t nonce[13])
{
    for (int k = 0; k < 6; k++) nonce[k] = mac[5 - k];
    nonce[6] = BTHOME_UUID_LO;
    nonce[7] = BTHOME_UUID_HI;
    nonce[8] = device_info;
    memcpy(&nonce[9], counter_bytes, 4);
}

static esp_err_t psa_import_key_bytes(const uint8_t key[16], psa_key_id_t *out_key)
{
    if (!key || !out_key) return ESP_ERR_INVALID_ARG;

    *out_key = PSA_KEY_ID_NULL;
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&a, PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, 4));
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, 128);
    psa_status_t ret = psa_import_key(&a, key, 16, out_key);
    if (ret != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void psa_destroy_key_handle(psa_key_id_t *key_id)
{
    if (key_id && *key_id != PSA_KEY_ID_NULL) {
        psa_destroy_key(*key_id);
        *key_id = PSA_KEY_ID_NULL;
    }
}

// Import AES key into PSA for device slot idx. Destroys any existing key first.
static esp_err_t psa_key_import(int idx)
{
    psa_destroy_key_handle(&s_psa_keys[idx]);
    esp_err_t err = psa_import_key_bytes(s_devices[idx].key, &s_psa_keys[idx]);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PSA key import failed for slot %d", idx);
    }
    return err;
}

// Destroy PSA key for device slot idx.
static void psa_key_remove(int idx)
{
    psa_destroy_key_handle(&s_psa_keys[idx]);
}

static void clear_crypto_status(int idx)
{
    s_key_import_errors[idx] = false;
    s_decrypt_errors[idx] = false;
    s_decrypt_failures[idx] = 0;
}

static void mark_import_result(int idx, esp_err_t err)
{
    s_key_import_errors[idx] = (err != ESP_OK);
    if (err != ESP_OK) {
        s_decrypt_errors[idx] = false;
        s_decrypt_failures[idx] = 0;
    }
}

static void note_decrypt_failure(int idx, const char *label)
{
    if (s_key_import_errors[idx]) return;
    if (s_decrypt_failures[idx] < UINT8_MAX) s_decrypt_failures[idx]++;
    if (!s_decrypt_errors[idx] && s_decrypt_failures[idx] >= DECRYPT_ERROR_THRESHOLD) {
        s_decrypt_errors[idx] = true;
        ESP_LOGW(TAG,
                 "Repeated decrypt failures for '%s' — BLE key may be out of sync",
                 label);
    }
}

static void note_decrypt_success(int idx)
{
    s_decrypt_errors[idx] = false;
    s_decrypt_failures[idx] = 0;
}

static esp_err_t decrypt_service_data(psa_key_id_t key_id,
                                      const uint8_t mac[6],
                                      const uint8_t *sd,
                                      size_t sd_len,
                                      uint8_t plaintext[BLE_ACCESS_MAX_DECRYPTED_PAYLOAD],
                                      size_t *out_len,
                                      uint32_t *out_counter)
{
    ble_service_data_frame_t frame;
    if (!parse_service_data_frame(sd, sd_len, &frame)) return ESP_ERR_INVALID_ARG;

    uint8_t nonce[13];
    build_bthome_nonce(mac, frame.device_info, frame.counter_bytes, nonce);

    uint8_t ct_with_tag[BLE_ACCESS_MAX_DECRYPTED_PAYLOAD + 4];
    memcpy(ct_with_tag, frame.enc_data, frame.enc_len);
    memcpy(ct_with_tag + frame.enc_len, frame.mic, 4);

    size_t decrypted_len = 0;
    psa_status_t psa_ret = psa_aead_decrypt(
        key_id, PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, 4),
        nonce, sizeof(nonce), NULL, 0,
        ct_with_tag, frame.enc_len + 4,
        plaintext, BLE_ACCESS_MAX_DECRYPTED_PAYLOAD, &decrypted_len);
    if (psa_ret != PSA_SUCCESS) return ESP_FAIL;

    if (out_len) *out_len = decrypted_len;
    if (out_counter) *out_counter = frame.counter;
    return ESP_OK;
}

static void parse_decrypted_payload(const uint8_t *plaintext, size_t len, ble_payload_parse_t *out)
{
    if (!out) return;

    memset(out, 0, sizeof(*out));
    if (!plaintext || len == 0) return;

    size_t pi = 0;
    while (pi < len) {
        uint8_t obj_id = plaintext[pi++];
        int dlen = bthome_obj_len(obj_id);
        if (dlen < 0 || pi + (size_t)dlen > len) break;

        if (obj_id == 0x01) {
            out->has_battery_percent = true;
            out->battery_percent = plaintext[pi];
        } else if (obj_id == BTN_OBJ_ID) {
            if (out->button_count < BLE_ACCESS_MAX_BUTTONS) {
                out->button_events[out->button_count] = plaintext[pi];
            }
            if (out->button_count < UINT8_MAX) out->button_count++;
        }
        pi += (size_t)dlen;
    }
}

static void capture_pending_device_locked(const uint8_t mac[6], const uint8_t *sd, uint8_t sd_len)
{
    if (!mac || !sd || sd_len == 0 || sd_len > sizeof(s_pending_service_data)) return;
    memcpy(s_pending_mac, mac, 6);
    memcpy(s_pending_service_data, sd, sd_len);
    s_pending_service_data_len = sd_len;
    s_has_pending = true;
}

// ── Advertisement handler ─────────────────────────────────────────────────────

// mac: NimBLE addr.val (little-endian, val[0]=LSB)
static void handle_adv(const uint8_t mac[6], const uint8_t *adv, uint8_t adv_len)
{
    const uint8_t *sd;
    uint8_t sd_len;
    if (!find_bthome_sd(adv, adv_len, &sd, &sd_len)) return;
    ble_service_data_frame_t frame;
    if (!parse_service_data_frame(sd, sd_len, &frame)) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int dev_idx = find_device_index_locked(mac);
    ble_device_t *dev = (dev_idx >= 0) ? &s_devices[dev_idx] : NULL;

    if (!dev) {
        // Unknown device: capture the encrypted service data so confirm can validate the key.
        if (s_registering && (!s_has_pending || mac_equal(s_pending_mac, mac))) {
            capture_pending_device_locked(mac, sd, sd_len);
        }
        xSemaphoreGive(s_mutex);
        return;
    }

    if (!dev->enabled) {
        xSemaphoreGive(s_mutex);
        return;
    }

    if (s_psa_keys[dev_idx] == PSA_KEY_ID_NULL) {
        s_key_import_errors[dev_idx] = true;
        xSemaphoreGive(s_mutex);
        return;
    }

    // Anti-replay: counter must be strictly greater than last accepted
    if (frame.counter <= dev->last_counter) {
        xSemaphoreGive(s_mutex);
        return;
    }

    uint8_t plaintext[BLE_ACCESS_MAX_DECRYPTED_PAYLOAD];
    size_t plaintext_len = 0;
    if (decrypt_service_data(s_psa_keys[dev_idx], mac, sd, sd_len,
                             plaintext, &plaintext_len, NULL) != ESP_OK) {
        ESP_LOGD(TAG, "Decrypt failed for '%s'", dev->label);
        note_decrypt_failure(dev_idx, dev->label);
        xSemaphoreGive(s_mutex);
        return;
    }

    note_decrypt_success(dev_idx);
    dev->last_counter = frame.counter;
    s_nvs_dirty = true;
    if (s_nvs_timer) {
        if (xTimerIsTimerActive(s_nvs_timer) == pdFALSE) {
            xTimerStart(s_nvs_timer, 0);
        }
    } else if (nvs_save() == ESP_OK) {
        s_nvs_dirty = false;
    }

    ble_payload_parse_t parsed;
    parse_decrypted_payload(plaintext, plaintext_len, &parsed);

    int64_t seen_ms = now_ms();
    bool saw_button_events = false;
    uint8_t active_buttons = 0;
    uint16_t mqtt_mask = 0;
    uint16_t gpio_mask = 0;
    s_runtime[dev_idx].last_seen_ms = seen_ms;
    if (parsed.has_battery_percent) {
        s_runtime[dev_idx].battery_known = true;
        s_runtime[dev_idx].battery_percent = parsed.battery_percent;
    }

    for (uint8_t button_idx = 0;
         button_idx < parsed.button_count && button_idx < BLE_ACCESS_MAX_BUTTONS;
         button_idx++) {
        uint8_t button_event = parsed.button_events[button_idx];
        if (button_event == BLE_BUTTON_EVENT_NONE) continue;

        saw_button_events = true;
        if (active_buttons < UINT8_MAX) active_buttons++;
        s_runtime[dev_idx].buttons[button_idx].last_event = button_event;
        s_runtime[dev_idx].buttons[button_idx].last_event_ms = seen_ms;
        mqtt_mask |= get_mqtt_event_mask(dev, button_idx, button_event);
        gpio_mask |= get_gpio_event_mask(dev, button_idx, button_event);
    }

    char label[32] = {0};
    if (saw_button_events) {
        strlcpy(label, dev->label, sizeof(label));
    }

    xSemaphoreGive(s_mutex);

    if (saw_button_events) {
        ESP_LOGI(TAG,
                 "'%s' packet with %u active button(s) mqtt_mask=0x%04X gpio_mask=0x%04X",
                 label, active_buttons, mqtt_mask, gpio_mask);
        for (int b = 0; b < MQTT_MAX_ACTIONS; b++) {
            if (mqtt_mask & (1u << b)) {
                mqtt_action_trigger(b);
            }
        }
        for (int b = 0; b < GPIO_ACTION_MAX; b++) {
            if (gpio_mask & (1u << b)) {
                gpio_action_trigger(b);
            }
        }
    }
}

// ── NimBLE callbacks ──────────────────────────────────────────────────────────

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    if (event->type == BLE_GAP_EVENT_DISC) {
        handle_adv(event->disc.addr.val,
                   event->disc.data,
                   event->disc.length_data);
    }
    return 0;
}

static void start_scan(void)
{
    struct ble_gap_disc_params dp = {0};
    dp.passive           = 1;
    dp.filter_duplicates = 0;
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &dp, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_disc failed: %d", rc);
    }
}

static void on_sync(void)
{
    s_ble_ready = true;
    if (s_scan_enabled) {
        start_scan();
    }
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset: %d", reason);
    s_ble_ready = false;
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ── Public API ────────────────────────────────────────────────────────────────

const char *ble_status_str(ble_status_t s)
{
    static const char *names[] = {
#define X(name, str) str,
        BLE_STATUS_LIST
#undef X
    };
    return (s < sizeof(names)/sizeof(*names)) ? names[s] : "?";
}

ble_status_t ble_get_status(void)
{
    if (s_init_failed)   return BLE_STATUS_ERROR;
    if (!s_ble_ready)    return BLE_STATUS_STARTING;
    if (!s_scan_enabled) return BLE_STATUS_PAUSED;
    return BLE_STATUS_SCANNING;
}

void ble_access_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "mutex allocation failed");
        s_init_failed = true;
        return;
    }
    memset(s_psa_keys, 0, sizeof(s_psa_keys));
    memset(s_runtime, 0, sizeof(s_runtime));
    memset(s_key_import_errors, 0, sizeof(s_key_import_errors));
    memset(s_decrypt_errors, 0, sizeof(s_decrypt_errors));
    memset(s_decrypt_failures, 0, sizeof(s_decrypt_failures));
    s_nvs_timer = xTimerCreate("ble_nvs", pdMS_TO_TICKS(2000), pdFALSE,
                               NULL, nvs_flush_timer_cb);
    if (!s_nvs_timer) {
        ESP_LOGW(TAG, "NVS flush timer creation failed; BLE counter saves stay synchronous");
    }
    psa_status_t crypto_err = psa_crypto_init();
    if (crypto_err != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)crypto_err);
        s_init_failed = true;
        return;
    }
    nvs_load();
    for (int i = 0; i < s_count; i++) {
        esp_err_t err = psa_key_import(i);
        mark_import_result(i, err);
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "Device '%s' kept configured but local key import failed",
                     s_devices[i].label);
        }
    }
    ESP_LOGI(TAG, "Loaded %d device(s) from NVS", s_count);

    nimble_port_init();
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
}

void ble_access_scan_stop(void)
{
    s_scan_enabled = false;
    if (s_ble_ready) {
        ble_gap_disc_cancel();
        ESP_LOGI(TAG, "BLE scan stopped");
    }
}

void ble_access_scan_start(void)
{
    s_scan_enabled = true;
    if (s_ble_ready) {
        start_scan();
        ESP_LOGI(TAG, "BLE scan started");
    }
}

esp_err_t ble_access_register_start(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_registering = true;
    s_has_pending = false;
    s_pending_service_data_len = 0;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Registration mode started");
    return ESP_OK;
}

esp_err_t ble_access_register_confirm(const uint8_t mac[6], const uint8_t key[16], const char *label)
{
    esp_err_t err = ESP_OK;
    psa_key_id_t temp_key = PSA_KEY_ID_NULL;
    uint8_t plaintext[BLE_ACCESS_MAX_DECRYPTED_PAYLOAD];
    size_t plaintext_len = 0;
    uint32_t counter = 0;
    ble_payload_parse_t parsed;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_has_pending ||
            s_pending_service_data_len == 0 ||
            !mac_equal(s_pending_mac, mac)) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_count >= BLE_ACCESS_MAX_DEVICES) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    if (find_device_index_locked(mac) >= 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_ARG;   // duplicate
    }

    err = psa_import_key_bytes(key, &temp_key);
    if (err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        return err;
    }

    err = decrypt_service_data(temp_key, mac,
                               s_pending_service_data, s_pending_service_data_len,
                               plaintext, &plaintext_len, &counter);
    if (err != ESP_OK) {
        psa_destroy_key_handle(&temp_key);
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }
    parse_decrypted_payload(plaintext, plaintext_len, &parsed);
    if (parsed.button_count == 0) {
        psa_destroy_key_handle(&temp_key);
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }
    if (parsed.button_count > BLE_ACCESS_MAX_BUTTONS) {
        psa_destroy_key_handle(&temp_key);
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_SUPPORTED;
    }

    int idx = s_count;
    ble_device_t *dev = &s_devices[s_count++];
    memset(dev, 0, sizeof(*dev));
    memcpy(dev->mac, mac, 6);
    memcpy(dev->key, key, 16);
    dev->last_counter = counter;
    strlcpy(dev->label, label ? label : "Device", sizeof(dev->label));
    dev->enabled = true;
    dev->button_count = parsed.button_count;
    memset(&s_runtime[idx], 0, sizeof(s_runtime[idx]));
    s_psa_keys[idx] = temp_key;
    temp_key = PSA_KEY_ID_NULL;
    clear_crypto_status(idx);

    err = nvs_save();
    if (err != ESP_OK) {
        psa_key_remove(idx);
        clear_crypto_status(idx);
        memset(dev, 0, sizeof(*dev));
        memset(&s_runtime[idx], 0, sizeof(s_runtime[idx]));
        s_count--;
        xSemaphoreGive(s_mutex);
        return err;
    }

    char saved_label[32];
    uint8_t saved_button_count = dev->button_count;
    strlcpy(saved_label, dev->label, sizeof(saved_label));
    s_registering = false;
    s_has_pending = false;
    s_pending_service_data_len = 0;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Device '%s' registered with %u button(s)",
             saved_label, (unsigned)saved_button_count);
    return ESP_OK;
}

void ble_access_register_cancel(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_registering = false;
    s_has_pending = false;
    s_pending_service_data_len = 0;
    xSemaphoreGive(s_mutex);
}

bool ble_access_is_registering(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool reg = s_registering;
    xSemaphoreGive(s_mutex);
    return reg;
}

bool ble_access_has_pending_mac(uint8_t out_mac[6])
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool has = s_has_pending;
    if (has && out_mac) memcpy(out_mac, s_pending_mac, 6);
    xSemaphoreGive(s_mutex);
    return has;
}

int ble_access_get_devices(ble_device_t *out, int max_count)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = s_count < max_count ? s_count : max_count;
    memcpy(out, s_devices, n * sizeof(ble_device_t));
    xSemaphoreGive(s_mutex);
    return n;
}

esp_err_t ble_access_get_device_by_mac(const uint8_t mac[6], ble_device_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (mac_equal(s_devices[i].mac, mac)) {
            *out = s_devices[i];
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ble_access_get_device_telemetry(const uint8_t mac[6], ble_device_telemetry_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int64_t current_ms = now_ms();
    for (int i = 0; i < s_count; i++) {
        if (mac_equal(s_devices[i].mac, mac)) {
            memset(out, 0, sizeof(*out));
            if (s_runtime[i].last_seen_ms > 0) {
                out->has_last_seen = true;
                out->last_seen_age_s = (uint32_t)((current_ms - s_runtime[i].last_seen_ms) / 1000);
            }
            if (s_runtime[i].battery_known) {
                out->has_battery_percent = true;
                out->battery_percent = s_runtime[i].battery_percent;
            }
            for (int button_idx = 0; button_idx < BLE_ACCESS_MAX_BUTTONS; button_idx++) {
                if (s_runtime[i].buttons[button_idx].last_event_ms <= 0) continue;
                out->buttons[button_idx].has_last_event = true;
                out->buttons[button_idx].last_event = s_runtime[i].buttons[button_idx].last_event;
                out->buttons[button_idx].last_event_age_s =
                    (uint32_t)((current_ms - s_runtime[i].buttons[button_idx].last_event_ms) / 1000);
            }
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ble_access_device_update(const uint8_t mac[6], const ble_device_t *updated)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (mac_equal(s_devices[i].mac, mac)) {
            // Only update fields modifiable via API; mac, key, counter are immutable
            strlcpy(s_devices[i].label, updated->label, sizeof(s_devices[i].label));
            s_devices[i].enabled = updated->enabled;
            memcpy(s_devices[i].buttons, updated->buttons, sizeof(s_devices[i].buttons));
            esp_err_t err = nvs_save();
            xSemaphoreGive(s_mutex);
            return err;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ble_access_device_update_key(const uint8_t mac[6], const uint8_t new_key[16])
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (mac_equal(s_devices[i].mac, mac)) {
            uint8_t old_key[16];
            bool old_key_import_error = s_key_import_errors[i];
            bool old_decrypt_error = s_decrypt_errors[i];
            uint8_t old_decrypt_failures = s_decrypt_failures[i];
            memcpy(old_key, s_devices[i].key, 16);

            memcpy(s_devices[i].key, new_key, 16);
            if (psa_key_import(i) != ESP_OK) {
                // New key rejected by PSA — restore old key (best effort)
                memcpy(s_devices[i].key, old_key, 16);
                esp_err_t restore_err = psa_key_import(i);
                if (restore_err == ESP_OK) {
                    s_key_import_errors[i] = old_key_import_error;
                    s_decrypt_errors[i] = old_decrypt_error;
                    s_decrypt_failures[i] = old_decrypt_failures;
                } else {
                    mark_import_result(i, restore_err);
                }
                xSemaphoreGive(s_mutex);
                ESP_LOGW(TAG, "Key update rejected for '%s': PSA import failed", s_devices[i].label);
                return ESP_FAIL;
            }
            clear_crypto_status(i);
            esp_err_t err = nvs_save();
            if (err != ESP_OK) {
                // NVS save failed — restore old key
                memcpy(s_devices[i].key, old_key, 16);
                esp_err_t restore_err = psa_key_import(i);
                if (restore_err == ESP_OK) {
                    s_key_import_errors[i] = old_key_import_error;
                    s_decrypt_errors[i] = old_decrypt_error;
                    s_decrypt_failures[i] = old_decrypt_failures;
                } else {
                    mark_import_result(i, restore_err);
                }
                xSemaphoreGive(s_mutex);
                return err;
            }
            ESP_LOGI(TAG, "Key updated for '%s'", s_devices[i].label);
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ble_access_device_reimport_key(const uint8_t mac[6])
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (mac_equal(s_devices[i].mac, mac)) {
            esp_err_t err = psa_key_import(i);
            mark_import_result(i, err);
            if (err == ESP_OK) {
                note_decrypt_success(i);
                ESP_LOGI(TAG, "Key re-imported for '%s'", s_devices[i].label);
            }
            xSemaphoreGive(s_mutex);
            return err;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

bool ble_access_has_key_import_error(const uint8_t mac[6])
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (mac_equal(s_devices[i].mac, mac)) {
            bool err = s_key_import_errors[i];
            xSemaphoreGive(s_mutex);
            return err;
        }
    }
    xSemaphoreGive(s_mutex);
    return false;
}

bool ble_access_has_decrypt_error(const uint8_t mac[6])
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (mac_equal(s_devices[i].mac, mac)) {
            bool err = s_decrypt_errors[i];
            xSemaphoreGive(s_mutex);
            return err;
        }
    }
    xSemaphoreGive(s_mutex);
    return false;
}

esp_err_t ble_access_device_delete(const uint8_t mac[6])
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (mac_equal(s_devices[i].mac, mac)) {
            psa_key_remove(i);
            memmove(&s_devices[i], &s_devices[i + 1],
                    (s_count - i - 1) * sizeof(ble_device_t));
            memmove(&s_psa_keys[i], &s_psa_keys[i + 1],
                    (s_count - i - 1) * sizeof(psa_key_id_t));
            memmove(&s_runtime[i], &s_runtime[i + 1],
                    (s_count - i - 1) * sizeof(ble_device_runtime_t));
            memmove(&s_key_import_errors[i], &s_key_import_errors[i + 1],
                    (s_count - i - 1) * sizeof(bool));
            memmove(&s_decrypt_errors[i], &s_decrypt_errors[i + 1],
                    (s_count - i - 1) * sizeof(bool));
            memmove(&s_decrypt_failures[i], &s_decrypt_failures[i + 1],
                    (s_count - i - 1) * sizeof(uint8_t));
            s_count--;
            s_psa_keys[s_count] = PSA_KEY_ID_NULL;
            memset(&s_runtime[s_count], 0, sizeof(s_runtime[s_count]));
            clear_crypto_status(s_count);
            esp_err_t err = nvs_save();
            xSemaphoreGive(s_mutex);
            return err;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}
