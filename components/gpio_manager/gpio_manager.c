#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <inttypes.h>
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "board_config.h"
#include "gpio_manager.h"

static const char *TAG = "gpio_manager";
static const char *NVS_NS = "gpio";

static gpio_action_t      s_actions[GPIO_ACTION_MAX];
static bool               s_current_on[GPIO_ACTION_MAX];
static bool               s_actions_loaded = false;
static bool               s_initialized = false;
static SemaphoreHandle_t  s_mutex = NULL;
static TimerHandle_t      s_restore_timers[GPIO_ACTION_MAX];
static StaticTimer_t      s_restore_timer_bufs[GPIO_ACTION_MAX];
static TimerHandle_t      s_system_led_timer = NULL;
static StaticTimer_t      s_system_led_timer_buf;
static bool               s_system_led_level = false;
static uint8_t            s_system_led_phase = 0;
static gpio_manager_ap_request_cb_t s_boot_ap_callback = NULL;

static void gpio_manager_start_boot_button_monitor(void);

static system_led_mode_t s_system_led_mode = SYSTEM_LED_OFF;

static bool action_slot_used(const gpio_action_t *action)
{
    return action->name[0] != '\0';
}

static bool action_kind_valid(uint8_t action)
{
    return action <= GPIO_ACTION_TOGGLE;
}

static int allowed_gpio_index(uint8_t gpio_num)
{
    uint8_t allowed_gpios[BOARD_CONFIG_MAX_ACTION_GPIOS];
    int count = board_config_get_allowed_action_gpios(allowed_gpios, BOARD_CONFIG_MAX_ACTION_GPIOS);
    for (int i = 0; i < count; i++) {
        if (allowed_gpios[i] == gpio_num) return i;
    }
    return -1;
}

bool gpio_action_gpio_allowed(uint8_t gpio_num)
{
    return allowed_gpio_index(gpio_num) >= 0;
}

int gpio_action_get_allowed_gpios(uint8_t *out, int max_count)
{
    return board_config_get_allowed_action_gpios(out, max_count);
}

const char *gpio_action_kind_str(gpio_action_kind_t kind)
{
    switch (kind) {
        case GPIO_ACTION_SET_ON:  return "on";
        case GPIO_ACTION_SET_OFF: return "off";
        case GPIO_ACTION_TOGGLE:  return "toggle";
        default:                  return "?";
    }
}

bool gpio_action_kind_parse(const char *str, gpio_action_kind_t *out)
{
    if (!str) return false;
    if (strcasecmp(str, "on") == 0) {
        if (out) *out = GPIO_ACTION_SET_ON;
        return true;
    }
    if (strcasecmp(str, "off") == 0) {
        if (out) *out = GPIO_ACTION_SET_OFF;
        return true;
    }
    if (strcasecmp(str, "toggle") == 0) {
        if (out) *out = GPIO_ACTION_TOGGLE;
        return true;
    }
    return false;
}

static int logical_to_level(const gpio_action_t *action, bool on)
{
    return action->active_low ? !on : on;
}

static int system_led_level_for(bool on)
{
    return board_config_system_led_active_low() ? !on : on;
}

static void set_output_state_locked(int idx, bool on)
{
    gpio_set_level((gpio_num_t)s_actions[idx].gpio_num, logical_to_level(&s_actions[idx], on));
    s_current_on[idx] = on;
}

static void restore_timer_cb(TimerHandle_t timer)
{
    int idx = (int)(intptr_t)pvTimerGetTimerID(timer);
    if (!s_mutex) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (idx >= 0 && idx < GPIO_ACTION_MAX && action_slot_used(&s_actions[idx])) {
        set_output_state_locked(idx, s_actions[idx].idle_on);
    }
    xSemaphoreGive(s_mutex);
}

static void create_restore_timers(void)
{
    for (int i = 0; i < GPIO_ACTION_MAX; i++) {
        if (!s_restore_timers[i]) {
            s_restore_timers[i] = xTimerCreateStatic("gpio_rst",
                                                     1, /* placeholder; real period set via xTimerChangePeriod */
                                                     pdFALSE,
                                                     (void *)(intptr_t)i,
                                                     restore_timer_cb,
                                                     &s_restore_timer_bufs[i]);
        }
    }
}

static void release_output_locked(int idx)
{
    if (idx < 0 || idx >= GPIO_ACTION_MAX || !action_slot_used(&s_actions[idx])) return;
    if (s_restore_timers[idx]) xTimerStop(s_restore_timers[idx], 0);
    gpio_reset_pin((gpio_num_t)s_actions[idx].gpio_num);
    s_current_on[idx] = false;
}

static void apply_output_config_locked(int idx)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << s_actions[idx].gpio_num,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    if (s_restore_timers[idx]) xTimerStop(s_restore_timers[idx], 0);
    set_output_state_locked(idx, s_actions[idx].idle_on);
}

static void actions_load_locked(void)
{
    memset(s_actions, 0, sizeof(s_actions));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = sizeof(s_actions);
    nvs_get_blob(h, "actions", s_actions, &sz);
    nvs_close(h);
}

static esp_err_t actions_save_blob(const gpio_action_t actions[GPIO_ACTION_MAX])
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, "actions", actions, sizeof(gpio_action_t) * GPIO_ACTION_MAX);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t actions_save_locked(void)
{
    return actions_save_blob(s_actions);
}

static void snapshot_actions_locked(gpio_action_t out[GPIO_ACTION_MAX])
{
    memcpy(out, s_actions, sizeof(s_actions));
}

static cJSON *action_export_json(const gpio_action_t *action, int idx)
{
    cJSON *item = cJSON_CreateObject();
    if (!item) return NULL;

    cJSON_AddNumberToObject(item, "idx", idx);
    cJSON_AddStringToObject(item, "name", action->name);
    cJSON_AddNumberToObject(item, "gpio", action->gpio_num);
    cJSON_AddBoolToObject(item, "idle_on", action->idle_on);
    cJSON_AddBoolToObject(item, "active_low", action->active_low);
    cJSON_AddStringToObject(item, "action", gpio_action_kind_str((gpio_action_kind_t)action->action));
    cJSON_AddNumberToObject(item, "restore_delay_ms", action->restore_delay_ms);
    return item;
}

static esp_err_t validate_action_candidate(const gpio_action_t actions[GPIO_ACTION_MAX],
                                           const gpio_action_t *action,
                                           int idx)
{
    if (idx < 0 || idx >= GPIO_ACTION_MAX) return ESP_ERR_INVALID_ARG;
    if (!action || action->name[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (!gpio_action_gpio_allowed(action->gpio_num)) return ESP_ERR_INVALID_ARG;
    if (!action_kind_valid(action->action)) return ESP_ERR_INVALID_ARG;

    for (int i = 0; i < GPIO_ACTION_MAX; i++) {
        if (i == idx || !action_slot_used(&actions[i])) continue;
        if (actions[i].gpio_num == action->gpio_num) return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

static bool sanitize_action_array(gpio_action_t actions[GPIO_ACTION_MAX])
{
    bool changed = false;

    for (int i = 0; i < GPIO_ACTION_MAX; i++) {
        if (!action_slot_used(&actions[i])) continue;
        if (validate_action_candidate(actions, &actions[i], i) != ESP_OK) {
            memset(&actions[i], 0, sizeof(actions[i]));
            changed = true;
        }
    }

    return changed;
}

static esp_err_t validate_action_locked(const gpio_action_t *action, int idx)
{
    return validate_action_candidate(s_actions, action, idx);
}

static void sanitize_loaded_actions_locked(void)
{
    if (sanitize_action_array(s_actions)) {
        actions_save_locked();
    }
}

static void apply_loaded_outputs_locked(void)
{
    for (int i = 0; i < GPIO_ACTION_MAX; i++) {
        s_current_on[i] = false;
        if (action_slot_used(&s_actions[i])) {
            apply_output_config_locked(i);
        }
    }
}

static void system_led_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    if (s_system_led_mode == SYSTEM_LED_OFF) return;

    TickType_t next_period = pdMS_TO_TICKS(300);

    switch (s_system_led_mode) {
        case SYSTEM_LED_AP_BLINK:
        case SYSTEM_LED_BOOT_AP_HINT:
            s_system_led_level = !s_system_led_level;
            next_period = pdMS_TO_TICKS(300);
            break;

        case SYSTEM_LED_BOOT_RESET_HINT:
            s_system_led_level = !s_system_led_level;
            next_period = pdMS_TO_TICKS(100);
            break;

        case SYSTEM_LED_WIFI_DISCONNECTED_HINT:
            switch (s_system_led_phase) {
                case 0:
                    s_system_led_level = true;
                    next_period = pdMS_TO_TICKS(150);
                    break;
                case 1:
                    s_system_led_level = false;
                    next_period = pdMS_TO_TICKS(150);
                    break;
                case 2:
                    s_system_led_level = true;
                    next_period = pdMS_TO_TICKS(150);
                    break;
                default:
                    s_system_led_level = false;
                    next_period = pdMS_TO_TICKS(900);
                    break;
            }
            s_system_led_phase = (s_system_led_phase + 1) % 4;
            break;

        case SYSTEM_LED_MQTT_DISCONNECTED_HINT:
            switch (s_system_led_phase) {
                case 0:
                case 2:
                case 4:
                    s_system_led_level = true;
                    next_period = pdMS_TO_TICKS(150);
                    break;
                case 1:
                case 3:
                    s_system_led_level = false;
                    next_period = pdMS_TO_TICKS(150);
                    break;
                default:
                    s_system_led_level = false;
                    next_period = pdMS_TO_TICKS(900);
                    break;
            }
            s_system_led_phase = (s_system_led_phase + 1) % 6;
            break;

        case SYSTEM_LED_OFF:
        default:
            return;
    }

    gpio_set_level((gpio_num_t)board_config_system_led_gpio(), system_led_level_for(s_system_led_level));
    xTimerChangePeriod(timer, next_period, 0);
}

static void system_led_apply_mode(system_led_mode_t mode)
{
    if (!s_system_led_timer) return;

    if (mode == s_system_led_mode) {
        if (mode == SYSTEM_LED_OFF) {
            xTimerStop(s_system_led_timer, 0);
            s_system_led_level = false;
            s_system_led_phase = 0;
            gpio_set_level((gpio_num_t)board_config_system_led_gpio(), system_led_level_for(false));
            return;
        }

        // Boot can replay the same composite LED mode several times in quick
        // succession as callbacks are registered. Avoid stop/start churn for
        // the same mode, but recover if the timer is not currently running.
        if (xTimerIsTimerActive(s_system_led_timer) == pdFALSE) {
            s_system_led_level = false;
            s_system_led_phase = 0;
            gpio_set_level((gpio_num_t)board_config_system_led_gpio(), system_led_level_for(false));
            xTimerChangePeriod(s_system_led_timer, pdMS_TO_TICKS(150), 0);
            xTimerStart(s_system_led_timer, 0);
        }
        return;
    }

    xTimerStop(s_system_led_timer, 0);

    s_system_led_mode = mode;
    s_system_led_level = false;
    s_system_led_phase = 0;
    gpio_set_level((gpio_num_t)board_config_system_led_gpio(), system_led_level_for(false));

    if (mode != SYSTEM_LED_OFF) {
        xTimerChangePeriod(s_system_led_timer, pdMS_TO_TICKS(150), 0);
        xTimerStart(s_system_led_timer, 0);
    }
}

void gpio_manager_init(void)
{
    if (s_initialized) return;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "mutex allocation failed");
        return;
    }

    create_restore_timers();

    gpio_config_t led_cfg = {
        .pin_bit_mask = 1ULL << board_config_system_led_gpio(),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_cfg);
    s_system_led_mode = SYSTEM_LED_OFF;
    gpio_set_level((gpio_num_t)board_config_system_led_gpio(), system_led_level_for(false));
    s_system_led_level = false;
    s_system_led_timer = xTimerCreateStatic("sys_led",
                                            pdMS_TO_TICKS(300),
                                            pdFALSE,
                                            NULL,
                                            system_led_timer_cb,
                                            &s_system_led_timer_buf);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_actions_loaded) {
        actions_load_locked();
        sanitize_loaded_actions_locked();
        apply_loaded_outputs_locked();
        s_actions_loaded = true;
    }
    xSemaphoreGive(s_mutex);

    s_initialized = true;
    gpio_manager_start_boot_button_monitor();
}

void gpio_manager_set_system_led_mode(system_led_mode_t mode)
{
    system_led_apply_mode(mode);
}

void gpio_manager_set_boot_ap_callback(gpio_manager_ap_request_cb_t cb)
{
    s_boot_ap_callback = cb;
}

esp_err_t gpio_action_add(const gpio_action_t *action, int *out_idx)
{
    if (!s_initialized || !s_mutex) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = ESP_ERR_NO_MEM;

    for (int i = 0; i < GPIO_ACTION_MAX; i++) {
        if (action_slot_used(&s_actions[i])) continue;

        err = validate_action_locked(action, i);
        if (err != ESP_OK) break;

        s_actions[i] = *action;
        apply_output_config_locked(i);
        err = actions_save_locked();
        if (err == ESP_OK) {
            if (out_idx) *out_idx = i;
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }

        release_output_locked(i);
        memset(&s_actions[i], 0, sizeof(s_actions[i]));
        break;
    }

    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t gpio_action_update(int idx, const gpio_action_t *action)
{
    if (!s_initialized || !s_mutex) return ESP_ERR_INVALID_STATE;
    if (idx < 0 || idx >= GPIO_ACTION_MAX) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!action_slot_used(&s_actions[idx])) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = validate_action_locked(action, idx);
    if (err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        return err;
    }

    gpio_action_t previous = s_actions[idx];
    release_output_locked(idx);
    s_actions[idx] = *action;
    apply_output_config_locked(idx);
    err = actions_save_locked();
    if (err != ESP_OK) {
        release_output_locked(idx);
        s_actions[idx] = previous;
        apply_output_config_locked(idx);
    }

    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t gpio_action_delete(int idx)
{
    if (!s_initialized || !s_mutex) return ESP_ERR_INVALID_STATE;
    if (idx < 0 || idx >= GPIO_ACTION_MAX) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!action_slot_used(&s_actions[idx])) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    gpio_action_t previous = s_actions[idx];
    release_output_locked(idx);
    memset(&s_actions[idx], 0, sizeof(s_actions[idx]));

    esp_err_t err = actions_save_locked();
    if (err != ESP_OK) {
        s_actions[idx] = previous;
        apply_output_config_locked(idx);
    }

    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t gpio_action_get(int idx, gpio_action_t *out)
{
    if (!s_initialized || !s_mutex) return ESP_ERR_INVALID_STATE;
    if (idx < 0 || idx >= GPIO_ACTION_MAX || !out) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!action_slot_used(&s_actions[idx])) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    *out = s_actions[idx];
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t gpio_action_trigger(int idx)
{
    if (!s_initialized || !s_mutex) return ESP_ERR_INVALID_STATE;
    if (idx < 0 || idx >= GPIO_ACTION_MAX) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!action_slot_used(&s_actions[idx])) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    bool target_on = s_current_on[idx];
    switch ((gpio_action_kind_t)s_actions[idx].action) {
        case GPIO_ACTION_SET_ON:
            target_on = true;
            break;
        case GPIO_ACTION_SET_OFF:
            target_on = false;
            break;
        case GPIO_ACTION_TOGGLE:
            target_on = !s_current_on[idx];
            break;
        default:
            xSemaphoreGive(s_mutex);
            return ESP_ERR_INVALID_ARG;
    }

    set_output_state_locked(idx, target_on);
    if (s_restore_timers[idx]) {
        xTimerStop(s_restore_timers[idx], 0);
        if (s_actions[idx].restore_delay_ms > 0 && target_on != s_actions[idx].idle_on) {
            TickType_t ticks = pdMS_TO_TICKS(s_actions[idx].restore_delay_ms);
            if (ticks == 0) ticks = 1;
            xTimerChangePeriod(s_restore_timers[idx], ticks, 0);
            xTimerStart(s_restore_timers[idx], 0);
        }
    }

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

struct cJSON *gpio_actions_export(void)
{
    if (!s_initialized || !s_mutex) return NULL;

    cJSON *actions = cJSON_CreateArray();
    if (!actions) return NULL;
    gpio_action_t snapshot[GPIO_ACTION_MAX];
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snapshot_actions_locked(snapshot);
    xSemaphoreGive(s_mutex);
    for (int i = 0; i < GPIO_ACTION_MAX; i++) {
        if (!action_slot_used(&snapshot[i])) continue;

        cJSON *item = action_export_json(&snapshot[i], i);
        if (!item || !cJSON_AddItemToArray(actions, item)) {
            if (item) cJSON_Delete(item);
            cJSON_Delete(actions);
            return NULL;
        }
    }

    return actions;
}

struct cJSON *gpio_pins_export(void)
{
    uint8_t gpios[BOARD_CONFIG_MAX_ACTION_GPIOS];
    int count = gpio_action_get_allowed_gpios(gpios, BOARD_CONFIG_MAX_ACTION_GPIOS);

    cJSON *pins = cJSON_CreateArray();
    if (!pins) return NULL;
    for (int i = 0; i < count; i++) {
        cJSON *pin = cJSON_CreateNumber(gpios[i]);
        if (!pin || !cJSON_AddItemToArray(pins, pin)) {
            if (pin) cJSON_Delete(pin);
            cJSON_Delete(pins);
            return NULL;
        }
    }
    return pins;
}

struct cJSON *gpio_backup_export(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON *actions = gpio_actions_export();
    if (!actions) {
        cJSON_Delete(root);
        return NULL;
    }
    if (!cJSON_AddItemToObject(root, "gpio_actions", actions)) {
        if (actions) cJSON_Delete(actions);
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

esp_err_t gpio_backup_import(const struct cJSON *root_obj)
{
    cJSON *root = (cJSON *)root_obj;
    if (!root) return ESP_ERR_INVALID_ARG;
    if (!s_initialized || !s_mutex) return ESP_ERR_INVALID_STATE;

    cJSON *actions = cJSON_GetObjectItem(root, "gpio_actions");
    if (!cJSON_IsArray(actions)) return ESP_OK;

    gpio_action_t next[GPIO_ACTION_MAX];
    memset(next, 0, sizeof(next));

    cJSON *item;
    cJSON_ArrayForEach(item, actions) {
        cJSON *idx = cJSON_GetObjectItem(item, "idx");
        cJSON *name = cJSON_GetObjectItem(item, "name");
        if (!cJSON_IsNumber(idx) || !cJSON_IsString(name) || name->valuestring[0] == '\0') continue;

        int action_idx = (int)idx->valuedouble;
        if (action_idx < 0 || action_idx >= GPIO_ACTION_MAX) continue;

        cJSON *gpio = cJSON_GetObjectItem(item, "gpio");
        cJSON *idle_on = cJSON_GetObjectItem(item, "idle_on");
        cJSON *active_low = cJSON_GetObjectItem(item, "active_low");
        cJSON *kind = cJSON_GetObjectItem(item, "action");
        cJSON *restore_delay_ms = cJSON_GetObjectItem(item, "restore_delay_ms");
        if (!cJSON_IsNumber(gpio) || !cJSON_IsString(kind)) continue;

        gpio_action_t *action = &next[action_idx];
        memset(action, 0, sizeof(*action));
        strlcpy(action->name, name->valuestring, sizeof(action->name));

        action->gpio_num = (uint8_t)gpio->valuedouble;
        if (cJSON_IsBool(idle_on)) action->idle_on = cJSON_IsTrue(idle_on);
        if (cJSON_IsBool(active_low)) action->active_low = cJSON_IsTrue(active_low);
        gpio_action_kind_t parsed_kind;
        if (gpio_action_kind_parse(kind->valuestring, &parsed_kind)) {
            action->action = (uint8_t)parsed_kind;
        } else {
            memset(action, 0, sizeof(*action));
            continue;
        }
        if (cJSON_IsNumber(restore_delay_ms) && restore_delay_ms->valuedouble > 0) {
            action->restore_delay_ms = (uint32_t)restore_delay_ms->valuedouble;
        }
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    sanitize_action_array(next);
    esp_err_t err = actions_save_blob(next);
    if (err != ESP_OK) {
        actions_save_locked();
    }
    xSemaphoreGive(s_mutex);
    return err;
}

// ── BOOT button monitor (board-defined GPIO) ───────────────────────────────
// Hold 3 s → start AP;  hold 10 s → factory reset + reboot.
// LED feedback: slow blink at 3 s, fast blink at 10 s.

#define BOOT_POLL_MS        100
#define BOOT_AP_THRESHOLD_MS    3000
#define BOOT_RESET_THRESHOLD_MS 10000

static bool boot_button_pressed(void)
{
    int level = gpio_get_level((gpio_num_t)board_config_boot_button_gpio());
    return board_config_boot_button_active_low() ? (level == 0) : (level != 0);
}

static void boot_button_task(void *arg)
{
    (void)arg;

    gpio_config_t btn_cfg = {
        .pin_bit_mask = 1ULL << board_config_boot_button_gpio(),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = board_config_boot_button_active_low() ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = board_config_boot_button_active_low() ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    bool ap_triggered = false;

    while (true) {
        if (!boot_button_pressed()) {
            ap_triggered = false;
            vTaskDelay(pdMS_TO_TICKS(BOOT_POLL_MS));
            continue;
        }

        uint32_t held_ms = 0;
        system_led_mode_t restore_mode = s_system_led_mode;
        system_led_mode_t feedback_mode = SYSTEM_LED_OFF;
        while (boot_button_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(BOOT_POLL_MS));
            held_ms += BOOT_POLL_MS;

            // Visual feedback: slow blink after 3 s
            if (held_ms >= BOOT_AP_THRESHOLD_MS && held_ms < BOOT_RESET_THRESHOLD_MS &&
                    feedback_mode != SYSTEM_LED_BOOT_AP_HINT) {
                feedback_mode = SYSTEM_LED_BOOT_AP_HINT;
                system_led_apply_mode(feedback_mode);
            }
            // Visual feedback: fast blink after 10 s
            if (held_ms >= BOOT_RESET_THRESHOLD_MS) {
                if (feedback_mode != SYSTEM_LED_BOOT_RESET_HINT) {
                    feedback_mode = SYSTEM_LED_BOOT_RESET_HINT;
                    system_led_apply_mode(feedback_mode);
                }
            }
        }

        system_led_apply_mode(restore_mode);

        if (held_ms >= BOOT_RESET_THRESHOLD_MS) {
            ESP_LOGW(TAG, "BOOT held %"PRIu32" ms — factory reset", held_ms);
            esp_err_t err = nvs_flash_erase();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "factory reset failed: %s", esp_err_to_name(err));
                continue;
            }
            esp_restart();
        } else if (held_ms >= BOOT_AP_THRESHOLD_MS && !ap_triggered) {
            ESP_LOGI(TAG, "BOOT held %"PRIu32" ms — starting AP", held_ms);
            if (s_boot_ap_callback) {
                s_boot_ap_callback();
            } else {
                ESP_LOGW(TAG, "BOOT AP request ignored: no callback registered");
            }
            ap_triggered = true;
        }
    }
}

static void gpio_manager_start_boot_button_monitor(void)
{
    xTaskCreate(boot_button_task, "boot_btn", 2048, NULL, 2, NULL);
}
