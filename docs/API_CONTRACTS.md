# API and Config Contracts

This document captures the payload and persistence contracts that are easy to break by accident.
It is intentionally biased toward the endpoints and schemas that most often affect compatibility.

## 0. API inventory

All routes below require auth when HTTP Basic Auth is enabled.

| Method | Endpoint | Notes |
|--------|----------|-------|
| `GET` | `/wifi-handoff` | Transitional handoff page shown after AP-based WiFi setup |
| `GET` | `/api/console/stream` | SSE stream with backlog + live console lines |
| `GET` | `/api/status` | Status snapshot; response shape documented here |
| `GET` | `/api/wifi/config` | Response shape documented here |
| `GET` | `/api/wifi/scan` | Returns array of `{ssid,rssi}`; may return `409` while WiFi is reconnecting |
| `POST` | `/api/wifi/connect` | JSON body; asynchronous connect |
| `DELETE` | `/api/wifi` | Clear credentials and disconnect |
| `POST` | `/api/ap/start` | `{"ok":true}` on success |
| `POST` | `/api/ap/stop` | `{"ok":true}` on success |
| `POST` | `/api/ap/handoff/complete` | Completes the temporary AP handoff and closes the auto-managed AP |
| `GET` | `/api/ap/config` | Response shape documented here |
| `POST` | `/api/ap/config` | JSON body; shape documented here |
| `GET` | `/api/mqtt/config` | Response shape documented here |
| `POST` | `/api/mqtt/connect` | JSON body; asynchronous connect |
| `DELETE` | `/api/mqtt` | Clear broker config |
| `GET` | `/api/mqtt/actions` | Array contract documented here |
| `POST` | `/api/mqtt/actions` | JSON body documented here |
| `PUT` | `/api/mqtt/action` | JSON body documented here |
| `DELETE` | `/api/mqtt/action` | JSON body documented here |
| `POST` | `/api/mqtt/action/test` | JSON body documented here |
| `GET` | `/api/gpio/pins` | Returns allowed GPIO numbers |
| `GET` | `/api/gpio/actions` | Array contract documented here |
| `POST` | `/api/gpio/actions` | JSON body documented here |
| `PUT` | `/api/gpio/action` | JSON body documented here |
| `DELETE` | `/api/gpio/action` | JSON body documented here |
| `POST` | `/api/gpio/action/test` | JSON body documented here |
| `GET` | `/api/ble/devices` | Array contract documented here |
| `GET` | `/api/ble/register/status` | Response shape documented here |
| `POST` | `/api/ble/register/start` | `{"ok":true}` on success |
| `POST` | `/api/ble/register/cancel` | `{"ok":true}` on success |
| `POST` | `/api/ble/register/confirm` | JSON body documented here |
| `PATCH` | `/api/ble/device` | JSON body documented here |
| `POST` | `/api/ble/device/reimport` | JSON body documented here |
| `DELETE` | `/api/ble/device` | JSON body documented here |
| `GET` | `/api/system/auth` | Response shape documented here |
| `POST` | `/api/system/auth` | JSON body documented here |
| `GET` | `/api/system/update/check` | Response shape documented here |
| `POST` | `/api/system/update` | Stages OTA update job |
| `POST` | `/api/system/ota` | Raw binary upload |
| `GET` | `/api/system/config` | Backup export contract documented here |
| `POST` | `/api/system/config` | Restore contract documented here |
| `POST` | `/api/system/reboot` | `{"ok":true}` then delayed reboot |
| `POST` | `/api/system/factory-reset` | `{"ok":true}` then erase NVS + reboot |

## 1. General response contract

### Mutation endpoints

Most mutating endpoints reply with one of:

- success:
  - `{"ok":true}`
  - or `{"ok":true,"idx":N}` for newly created indexed actions
- failure:
  - `{"ok":false,"error":"..."}`

Do not silently change this shape unless the UI is updated in lockstep.

### Read endpoints

Read endpoints usually return either:

- a JSON object with named fields
- or a JSON array of objects

They do not always include an outer `ok` flag.

### Non-JSON transport notes

Not every HTTP endpoint in the web surface is JSON:

- `/api/console/stream` uses SSE
- `/api/system/ota` accepts a raw binary request body

Do not generalize JSON assumptions across the whole HTTP surface.

## 2. System and OTA contracts

### `GET /api/status`

Response shape:

```json
{
  "wifi": "up|connecting|error|not config|...",
  "wifi_error_latched": false,
  "mqtt": "up|connecting|error|not config|...",
  "ap": "up|down",
  "ble": "up|scanning|paused|...",
  "sta_ip": "192.168.1.23",
  "fw_version": "1.3.1"
}
```

`fw_version` comes from `esp_app_get_description()->version`.
`sta_ip` is present only when the STA interface currently has an IPv4 address.
`wifi_error_latched` stays `true` after a config-related join failure until the next manual connect attempt, disconnect, credential erase, or successful join.

### `GET /api/system/update/check`

Success response:

```json
{
  "ok": true,
  "current_version": "v1.3.1",
  "latest_version": "v1.3.2",
  "update_available": true,
  "release_url": "https://github.com/.../releases/tag/v1.3.2",
  "asset_name": "BluButtonBridge-esp32c3-supermini.bin",
  "asset_size": 1234567
}
```

Important behavior:

- this call also caches the latest checked release in RAM inside `web_manager`
- `POST /api/system/update` depends on that cached result
- the latest release metadata now comes from the site-hosted `https://robertoamd90.github.io/blu-button-bridge/ota-manifest.json`
- the staged OTA download installs the board-specific mirrored `.bin` from that same GitHub Pages site

### `POST /api/system/update`

Request body:

```json
{}
```

Behavior:

- requires a successful prior `GET /api/system/update/check`
- stages a board-specific OTA job in NVS through `ota_manager`
- does **not** download immediately inside the HTTP request
- reboots the device into OTA mode

Success response:

```json
{"ok":true}
```

### `POST /api/system/ota`

Request body:

- raw binary body, not JSON

Behavior:

- manual OTA upload stays immediate
- HTTP transport lives in `web_manager`
- OTA begin/write/end lives in `ota_manager`

Success response:

```json
{"ok":true}
```

Common failure semantics:

- empty image -> error
- image larger than OTA partition -> HTTP 413
- image validation failure -> HTTP 422

### `POST /api/system/reboot`

Request body:

```json
{}
```

Success response:

```json
{"ok":true}
```

### `POST /api/system/factory-reset`

Request body:

```json
{}
```

Success response:

```json
{"ok":true}
```

### OTA staged job persistence

Namespace:

- `ota_manager`

Persisted keys:

- `status`
- `pending`
- `attempts`
- `last_error`
- `version`
- `url`
- `digest`

Current status values:

- `PENDING`
- `RUNNING`
- `FAILED`

Current failure reasons written in practice:

- `max_attempts`
- `wifi_timeout`
- `install_failed`

Published OTA external contract:

- discovery source: `https://robertoamd90.github.io/blu-button-bridge/ota-manifest.json`
- download source: board-specific mirrored OTA binaries on the same GitHub Pages site
- asset selection is board-specific:
  - `BluButtonBridge-esp32-devkit-v1.bin`
  - `BluButtonBridge-esp32c3-supermini.bin`
- trust/check source: SHA-256 digests carried by the published OTA manifest and verified again after download
- success path:
  - stage job
  - reboot to OTA mode
  - wait for WiFi
  - download asset
  - verify SHA-256
  - switch boot partition
  - reboot into updated firmware
- failure path:
  - mark staged job `FAILED`
  - reboot
  - next boot returns to the normal firmware path

## 3. Auth contracts

### `GET /api/ap/config`

Response shape:

```json
{
  "enabled": false,
  "ssid": "BBB-XXXXXX",
  "password": "12345678"
}
```

### `POST /api/ap/config`

Request shape:

```json
{
  "enabled": false,
  "ssid": "BBB-XXXXXX",
  "password": "12345678"
}
```

Unset fields keep current values as base.

### `POST /api/ap/handoff/complete`

Request body:

```json
{}
```

Success response:

```json
{"ok":true}
```

Behavior:

- this is used by `/wifi-handoff`
- when the AP is auto-managed and a WiFi handoff is pending, it closes the AP immediately
- when the AP is always on or no handoff is pending, it is a no-op

### `GET /api/wifi/config`

Response shape:

```json
{
  "ssid": "OpenWrt-iot",
  "ssid_set": true,
  "password_set": true
}
```

### `POST /api/wifi/connect`

Request shape:

```json
{
  "ssid": "OpenWrt-iot",
  "password": "secret"
}
```

`ssid` is required. The connection attempt continues in background.

Success response:

```json
{
  "ok": true,
  "handoff": true,
  "redirect": "/wifi-handoff"
}
```

`handoff` and `redirect` are present only when the request was started from the auto-managed setup AP and the UI should move to the handoff page.

### `GET /api/mqtt/config`

Response shape:

```json
{
  "configured": true,
  "host": "broker.local",
  "port": 8883,
  "username": "user",
  "tls": true,
  "password_set": true
}
```

### `POST /api/mqtt/connect`

Request shape:

```json
{
  "host": "broker.local",
  "port": 8883,
  "username": "user",
  "password": "secret",
  "tls": true
}
```

`host` is required. The connection attempt continues in background.

### `GET /api/system/auth`

Response shape:

```json
{
  "enabled": true,
  "username": "admin",
  "password_set": true
}
```

### `POST /api/system/auth`

Request shape:

```json
{
  "enabled": true,
  "username": "admin",
  "password": "secret"
}
```

Rules:

- when auth is enabled, username and password are required
- `username` must not contain `:`
- empty password clears the stored password only when auth is not being enabled with it
- backup/restore persists the password hash, not the cleartext password

## 4. MQTT action contracts

### `GET /api/mqtt/actions`

Array items:

```json
{
  "idx": 0,
  "name": "Gate",
  "topic": "cmnd/gate/POWER",
  "payload": "TOGGLE"
}
```

### `POST /api/mqtt/actions`

Request:

```json
{
  "name": "Gate",
  "topic": "cmnd/gate/POWER",
  "payload": "TOGGLE"
}
```

Success:

```json
{"ok":true,"idx":0}
```

### `PUT /api/mqtt/action`

Request:

```json
{
  "idx": 0,
  "name": "Gate",
  "topic": "cmnd/gate/POWER",
  "payload": "TOGGLE"
}
```

`idx` and `name` are required.

### `POST /api/mqtt/action/test`

Request:

```json
{"idx":0}
```

### `DELETE /api/mqtt/action`

Request:

```json
{"idx":0}
```

## 5. GPIO action contracts

### `GET /api/gpio/actions`

Array items:

```json
{
  "idx": 0,
  "name": "Relay",
  "gpio": 23,
  "idle_on": false,
  "active_low": true,
  "action": "toggle",
  "restore_delay_ms": 1000
}
```

### `POST /api/gpio/actions`

Request:

```json
{
  "name": "Relay",
  "gpio": 23,
  "idle_on": false,
  "active_low": true,
  "action": "toggle",
  "restore_delay_ms": 1000
}
```

Rules:

- `name`, `gpio`, and `action` are required
- valid `action` strings are:
  - `set_on`
  - `set_off`
  - `toggle`

### `PUT /api/gpio/action`

Same as add, plus required `idx`.

### `POST /api/gpio/action/test`

Request:

```json
{"idx":0}
```

### `DELETE /api/gpio/action`

Request:

```json
{"idx":0}
```

## 6. BLE device contracts

### `GET /api/ble/devices`

Array items include:

- identity:
  - `mac`
  - `label`
  - `enabled`
  - `button_count`
- health:
  - `key_import_error`
  - `decrypt_error`
- device runtime telemetry:
  - `battery_percent`
  - `last_seen_age_s`
- nested button telemetry + mappings:
  - `buttons[]`
  - each button item includes:
    - `idx`
    - `last_event`
    - `last_event_age_s`
    - `mqtt`
    - `gpio`

Representative shape:

```json
{
  "mac": "AA:BB:CC:DD:EE:FF",
  "label": "Wall switch",
  "enabled": true,
  "button_count": 4,
  "battery_percent": 88,
  "last_seen_age_s": 12,
  "key_import_error": false,
  "decrypt_error": false,
  "buttons": [
    {
      "idx": 0,
      "last_event": "double_press",
      "last_event_age_s": 30,
      "mqtt": {
        "single_press": 1,
        "double_press": 0,
        "triple_press": 0,
        "long_press": 2
      },
      "gpio": {
        "single_press": 0,
        "double_press": 4,
        "triple_press": 0,
        "long_press": 0
      }
    }
  ]
}
```

### `POST /api/ble/register/confirm`

Request:

```json
{
  "mac": "AA:BB:CC:DD:EE:FF",
  "key": "00112233445566778899AABBCCDDEEFF",
  "label": "Gate remote"
}
```

Rules:

- `mac` and `key` are required
- `key` must be exactly 32 hex chars
- label defaults to `Device` if omitted
- registration confirmation validates the provided key against the captured encrypted packet before saving
- registration derives `button_count` from that same packet
- if decryption fails, the device is not saved

### `PATCH /api/ble/device`

Partial update shape:

```json
{
  "mac": "AA:BB:CC:DD:EE:FF",
  "label": "Gate remote",
  "enabled": true,
  "buttons": [
    {
      "idx": 0,
      "mqtt": {
        "single_press": 1,
        "double_press": 0,
        "triple_press": 0,
        "long_press": 2
      },
      "gpio": {
        "single_press": 1,
        "double_press": 0,
        "triple_press": 0,
        "long_press": 0
      }
    }
  ],
  "key": "00112233445566778899AABBCCDDEEFF"
}
```

Rules:

- `mac` is required
- `buttons` replaces the per-button mapping set when present
- each mapping field is a bitmask referencing action slots
- optional `key` replaces the stored key only when present and non-empty

### `POST /api/ble/device/reimport`

Request:

```json
{"mac":"AA:BB:CC:DD:EE:FF"}
```

### `DELETE /api/ble/device`

Request:

```json
{"mac":"AA:BB:CC:DD:EE:FF"}
```

### `GET /api/ble/register/status`

Response shape:

```json
{
  "registering": true,
  "pending_mac": "AA:BB:CC:DD:EE:FF"
}
```

When no device is pending, `pending_mac` is `null`.

## 7. Backup / restore contract

### `GET /api/system/config`

Current exported schema version:

```json
{"version":3}
```

Top-level sections:

- `version`
- `wifi`
- `ap`
- `auth`
- `mqtt`
- `mqtt_actions`
- `gpio_actions`
- `ble_devices`

Notable details:

- `auth.password_sha256` is exported, not the cleartext password
- BLE runtime telemetry is **not** exported
- BLE device mappings now use the nested `button_count` + `buttons[]` schema
- OTA staged job state is **not** part of backup/restore

### `POST /api/system/config`

Restore rules:

- restore request body limit is currently 12 KB
- restore is designed for device-generated backups produced by this firmware family
- restore is not treated as a hardened general-purpose import path for arbitrary third-party or hand-edited JSON
- restore is tolerant of missing sections
- array entries with invalid or out-of-range indexes are skipped
- auth restore is stricter than many other sections:
  - invalid username or invalid password hash rejects restore
  - enabling auth without username or password hash rejects restore
- restore rejects any backup whose top-level `version` is not exactly `3`
- BLE restore is strict about schema:
  - each BLE device entry must provide `button_count` and `buttons`
  - legacy flat BLE event fields are ignored and are not a migration path; the required v3 nested fields must still be present
- successful restore triggers reboot

## 8. Compatibility rule

If changing:

- JSON response shape
- accepted request fields
- config export/import schema
- OTA staged job persistence keys

update both:

- this document
- the UI and any dependent code paths in the same change

Repository policy for destructive compatibility changes:

- backward compatibility is not guaranteed for intentionally destructive persisted-state or schema changes
- when a change intentionally breaks restore/import or on-device persisted state compatibility, document that break clearly in the release notes
- do not describe a destructive schema change here as migratable unless the code actually implements the migration path
