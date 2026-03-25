#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
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

// ── State ─────────────────────────────────────────────────────────────────────

static ble_device_t         s_devices[BLE_ACCESS_MAX_DEVICES];
static psa_key_id_t         s_psa_keys[BLE_ACCESS_MAX_DEVICES]; // cached PSA key handles
static bool                 s_key_import_errors[BLE_ACCESS_MAX_DEVICES];
static bool                 s_decrypt_errors[BLE_ACCESS_MAX_DEVICES];
static uint8_t              s_decrypt_failures[BLE_ACCESS_MAX_DEVICES];
static int                  s_count        = 0;
static bool                 s_registering  = false;
static bool                 s_has_pending  = false;
static bool                 s_scan_enabled = true;
static bool                 s_ble_ready    = false;
static uint8_t              s_pending_mac[6];
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

// ── Helpers ───────────────────────────────────────────────────────────────────

static bool mac_equal(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) == 0;
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

static uint16_t get_mqtt_event_mask(const ble_device_t *dev, uint8_t event)
{
    switch (event) {
        case 1: return dev->single_press;
        case 2: return dev->double_press;
        case 3: return dev->triple_press;
        case 4: return dev->long_press;
        default: return 0;
    }
}

static uint16_t get_gpio_event_mask(const ble_device_t *dev, uint8_t event)
{
    switch (event) {
        case 1: return dev->gpio_single_press;
        case 2: return dev->gpio_double_press;
        case 3: return dev->gpio_triple_press;
        case 4: return dev->gpio_long_press;
        default: return 0;
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

// Import AES key into PSA for device slot idx. Destroys any existing key first.
static esp_err_t psa_key_import(int idx)
{
    if (s_psa_keys[idx] != PSA_KEY_ID_NULL) {
        psa_destroy_key(s_psa_keys[idx]);
        s_psa_keys[idx] = PSA_KEY_ID_NULL;
    }
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&a, PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, 4));
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, 128);
    psa_status_t ret = psa_import_key(&a, s_devices[idx].key, 16, &s_psa_keys[idx]);
    if (ret != PSA_SUCCESS) {
        ESP_LOGE(TAG, "PSA key import failed for slot %d: %d", idx, (int)ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Destroy PSA key for device slot idx.
static void psa_key_remove(int idx)
{
    if (s_psa_keys[idx] != PSA_KEY_ID_NULL) {
        psa_destroy_key(s_psa_keys[idx]);
        s_psa_keys[idx] = PSA_KEY_ID_NULL;
    }
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

// ── Advertisement handler ─────────────────────────────────────────────────────

// mac: NimBLE addr.val (little-endian, val[0]=LSB)
static void handle_adv(const uint8_t mac[6], const uint8_t *adv, uint8_t adv_len)
{
    const uint8_t *sd;
    uint8_t sd_len;
    if (!find_bthome_sd(adv, adv_len, &sd, &sd_len)) return;

    // Shelly BTHome v2 encrypted service data layout (from UUID start):
    //   [UUID 2B][DevInfo 1B][Encrypted objects N B][Counter 4B][MIC 4B]
    // Minimum: 2+1+0+4+4 = 11 bytes
    if (sd_len < 11) return;

    uint8_t device_info = sd[2];
    if (!(device_info & 0x01)) return;   // not encrypted — ignore unencrypted beacons

    // Shelly puts Counter THEN MIC (MIC is the last 4 bytes)
    const uint8_t *counter_bytes = &sd[sd_len - 8];   // 4 bytes before end
    const uint8_t *mic           = &sd[sd_len - 4];   // last 4 bytes
    const uint8_t *enc_data      = &sd[3];             // right after DevInfo

    int enc_len = (int)sd_len - 2 - 1 - 4 - 4;        // minus UUID, DevInfo, Counter, MIC
    if (enc_len <= 0) return;

    uint32_t counter;
    memcpy(&counter, counter_bytes, 4);

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    // Find matching whitelisted device
    ble_device_t *dev = NULL;
    int dev_idx = -1;
    for (int i = 0; i < s_count; i++) {
        if (s_devices[i].enabled && mac_equal(s_devices[i].mac, mac)) {
            dev = &s_devices[i];
            dev_idx = i;
            break;
        }
    }

    if (!dev) {
        // Unknown device: capture MAC if in registration mode
        if (s_registering && !s_has_pending) {
            memcpy(s_pending_mac, mac, 6);
            s_has_pending = true;
        }
        xSemaphoreGive(s_mutex);
        return;
    }

    if (s_psa_keys[dev_idx] == PSA_KEY_ID_NULL) {
        s_key_import_errors[dev_idx] = true;
        xSemaphoreGive(s_mutex);
        return;
    }

    // Anti-replay: counter must be strictly greater than last accepted
    if (counter <= dev->last_counter) {
        xSemaphoreGive(s_mutex);
        return;
    }

    // Build 13-byte nonce: MAC[6 MSB-first] + UUID[2 LE] + DevInfo[1] + Counter[4 LE]
    // Note: NimBLE addr.val is LSB-first; Shelly nonce uses display order (MSB-first = reversed)
    uint8_t nonce[13];
    for (int k = 0; k < 6; k++) nonce[k] = mac[5 - k];
    nonce[6] = BTHOME_UUID_LO;
    nonce[7] = BTHOME_UUID_HI;
    nonce[8] = device_info;
    memcpy(&nonce[9], counter_bytes, 4);

    // AES-128-CCM decrypt; PSA expects ciphertext = enc_data || MIC
    int dec_len = enc_len < 32 ? enc_len : 32;
    uint8_t ct_with_tag[36];
    memcpy(ct_with_tag, enc_data, dec_len);
    memcpy(ct_with_tag + dec_len, mic, 4);

    uint8_t plaintext[32];
    size_t  out_len = 0;
    psa_status_t psa_ret = psa_aead_decrypt(
        s_psa_keys[dev_idx], PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, 4),
        nonce, sizeof(nonce), NULL, 0,
        ct_with_tag, dec_len + 4,
        plaintext, sizeof(plaintext), &out_len);

    if (psa_ret != PSA_SUCCESS) {
        ESP_LOGD(TAG, "Decrypt failed for '%s' (psa=%d)", dev->label, (int)psa_ret);
        note_decrypt_failure(dev_idx, dev->label);
        xSemaphoreGive(s_mutex);
        return;
    }
    dec_len = (int)out_len;

    note_decrypt_success(dev_idx);
    dev->last_counter = counter;

    // Parse BTHome objects in decrypted payload
    int pi = 0;
    while (pi < dec_len) {
        uint8_t obj_id = plaintext[pi++];
        int dlen = bthome_obj_len(obj_id);
        if (dlen < 0 || pi + dlen > dec_len) break;

        if (obj_id == BTN_OBJ_ID) {
            uint8_t event_val = plaintext[pi];
            uint16_t mqtt_mask = get_mqtt_event_mask(dev, event_val);
            uint16_t gpio_mask = get_gpio_event_mask(dev, event_val);
            char label[32];
            strlcpy(label, dev->label, sizeof(label));
            xSemaphoreGive(s_mutex);
            ESP_LOGI(TAG,
                     "'%s' button event=%u mqtt_mask=0x%04X gpio_mask=0x%04X",
                     label, event_val, mqtt_mask, gpio_mask);
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
            return;
        }
        pi += dlen;
    }

    xSemaphoreGive(s_mutex);
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
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ── Public API ────────────────────────────────────────────────────────────────

void ble_access_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "mutex allocation failed");
        return;
    }
    memset(s_psa_keys, 0, sizeof(s_psa_keys));
    memset(s_key_import_errors, 0, sizeof(s_key_import_errors));
    memset(s_decrypt_errors, 0, sizeof(s_decrypt_errors));
    memset(s_decrypt_failures, 0, sizeof(s_decrypt_failures));
    psa_status_t crypto_err = psa_crypto_init();
    if (crypto_err != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)crypto_err);
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
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Registration mode started");
    return ESP_OK;
}

esp_err_t ble_access_register_confirm(const uint8_t mac[6], const uint8_t key[16], const char *label)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_count >= BLE_ACCESS_MAX_DEVICES) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < s_count; i++) {
        if (mac_equal(s_devices[i].mac, mac)) {
            xSemaphoreGive(s_mutex);
            return ESP_ERR_INVALID_ARG;   // duplicate
        }
    }
    int idx = s_count;
    ble_device_t *dev = &s_devices[s_count++];
    memset(dev, 0, sizeof(*dev));
    memcpy(dev->mac, mac, 6);
    memcpy(dev->key, key, 16);
    strlcpy(dev->label, label ? label : "Device", sizeof(dev->label));
    dev->enabled = true;

    esp_err_t err = psa_key_import(idx);
    if (err != ESP_OK) {
        clear_crypto_status(idx);
        s_key_import_errors[idx] = true;
        memset(dev, 0, sizeof(*dev));
        s_count--;
        xSemaphoreGive(s_mutex);
        return err;
    }
    clear_crypto_status(idx);

    err = nvs_save();
    if (err != ESP_OK) {
        psa_key_remove(idx);
        clear_crypto_status(idx);
        memset(dev, 0, sizeof(*dev));
        s_count--;
        xSemaphoreGive(s_mutex);
        return err;
    }

    s_registering = false;
    s_has_pending = false;
    xSemaphoreGive(s_mutex);
    if (err == ESP_OK) ESP_LOGI(TAG, "Device '%s' registered", dev->label);
    return err;
}

void ble_access_register_cancel(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_registering = false;
    s_has_pending = false;
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

esp_err_t ble_access_device_update(const uint8_t mac[6], const ble_device_t *updated)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (mac_equal(s_devices[i].mac, mac)) {
            // Only update fields modifiable via API; mac, key, counter are immutable
            strlcpy(s_devices[i].label, updated->label, sizeof(s_devices[i].label));
            s_devices[i].enabled      = updated->enabled;
            s_devices[i].single_press = updated->single_press;  // bitmask
            s_devices[i].double_press = updated->double_press;
            s_devices[i].triple_press = updated->triple_press;
            s_devices[i].long_press   = updated->long_press;
            s_devices[i].gpio_single_press = updated->gpio_single_press;
            s_devices[i].gpio_double_press = updated->gpio_double_press;
            s_devices[i].gpio_triple_press = updated->gpio_triple_press;
            s_devices[i].gpio_long_press   = updated->gpio_long_press;
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
            memmove(&s_key_import_errors[i], &s_key_import_errors[i + 1],
                    (s_count - i - 1) * sizeof(bool));
            memmove(&s_decrypt_errors[i], &s_decrypt_errors[i + 1],
                    (s_count - i - 1) * sizeof(bool));
            memmove(&s_decrypt_failures[i], &s_decrypt_failures[i + 1],
                    (s_count - i - 1) * sizeof(uint8_t));
            s_count--;
            s_psa_keys[s_count] = PSA_KEY_ID_NULL;
            clear_crypto_status(s_count);
            esp_err_t err = nvs_save();
            xSemaphoreGive(s_mutex);
            return err;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}
