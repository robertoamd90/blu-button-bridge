# Runtime Event Flow

This document captures the important cross-module runtime flows.
Use it before changing state handling, reconnect logic, LED behavior, or OTA boot takeover.

## 1. Normal boot

High-level boot path:

1. `app_main()`
2. `nvs_flash_init()`
3. `console_manager_init()`
4. `gpio_manager_init()`
5. `wifi_init()`
6. `ota_manager_start_pending_job()`
   - if a staged GitHub OTA job exists, OTA mode takes ownership of the boot
7. if OTA mode did not take over:
   - `mqtt_init()`
   - `system_runtime_init()`
   - `ble_access_init()`
   - `web_manager_init()`

## 2. WiFi / MQTT / LED wiring

Owner:

- `system_runtime`

Responsibilities:

- observe WiFi status changes
- observe AP active/inactive changes
- observe MQTT status changes
- push network availability into `mqtt_manager`
- compute the composite system LED policy

Current wiring:

- WiFi status callback:
  - updates runtime cache
  - calls `mqtt_set_network_available(status == WIFI_STATUS_UP)`
  - refreshes LED
- AP status callback:
  - updates runtime cache
  - refreshes LED
- MQTT status callback:
  - updates runtime cache
  - refreshes LED

## 3. System LED policy

Owner:

- `system_runtime` decides policy
- `gpio_manager` applies the raw LED mode

Priority order:

1. AP active
   - `SYSTEM_LED_AP_BLINK`
2. WiFi connecting or WiFi error
   - `SYSTEM_LED_WIFI_DISCONNECTED_HINT`
3. WiFi up and MQTT connecting or MQTT error
   - `SYSTEM_LED_MQTT_DISCONNECTED_HINT`
4. otherwise
   - `SYSTEM_LED_OFF`

Important rule:

- do not move this composite policy into `wifi_manager` or `mqtt_manager`
- it depends on multiple module states and belongs in the composition layer

## 4. AP / BLE coexistence

Relevant behavior:

- BLE scanning pauses while SoftAP is active
- BLE scanning resumes when SoftAP stops

Reason:

- AP and BLE share one radio
- BLE scan can interfere with AP association/handshake reliability

If changing AP behavior, treat BLE pause/resume as part of the contract.

## 5. Console streaming flow

Owner:

- `web_manager`

Current model:

- `/api/console/stream` uses async HTTP handling
- stream work runs in a dedicated task, not in the `httpd` request handler loop
- backlog comes from `console_manager`
- live stream uses SSE

Single-client rule:

- last client wins
- each new console stream increments a generation counter
- the previous stream receives a `replaced` event and closes

Important rule:

- do not reintroduce infinite streaming loops directly inside the HTTP server task

## 6. Manual OTA flow

Owners:

- `web_manager`: receives raw HTTP upload body
- `ota_manager`: owns OTA write/finalize/abort domain logic

Flow:

1. client uploads raw `.bin` to `/api/system/ota`
2. `web_manager` takes OTA mutex
3. `ota_manager_upload_begin()`
4. repeated `ota_manager_upload_write()`
5. `ota_manager_upload_finish()`
6. response `{"ok":true}`
7. reboot task triggers reboot

Important rule:

- manual OTA is immediate
- it does not use staged OTA mode

## 7. GitHub OTA staged flow

Owners:

- `web_manager`: release check + staging request
- `ota_manager`: staged job persistence + OTA mode execution

HTTP/UI phase:

1. UI calls `GET /api/system/update/check`
2. `web_manager` fetches latest GitHub release metadata
3. selected release info is cached in RAM
4. UI calls `POST /api/system/update`
5. `web_manager` stages the OTA job in NVS via `ota_manager_stage_github_job(...)`
6. device reboots

Boot takeover phase:

1. `app_main()` calls `ota_manager_start_pending_job()`
2. if staged job exists, `ota_manager` starts `ota_mode` task and takes ownership of the boot
3. OTA mode waits for WiFi up
4. OTA mode downloads the staged asset
5. OTA mode verifies SHA-256 against staged digest
6. OTA mode switches boot partition
7. OTA mode reboots into updated firmware

Failure semantics:

- job state is updated in NVS
- failures mark job `FAILED`
- OTA mode reboots
- next boot returns to the normal firmware path

## 8. OTA job state flow

Persisted states:

- `PENDING`
- `RUNNING`
- `FAILED`

Key transitions:

- `stage` -> `PENDING`
- OTA mode start -> `RUNNING`
- success -> clear job
- failure -> `FAILED`

Current retry behavior:

- max attempts: 3
- exceeding attempts marks the job failed and discards further OTA takeover

## 9. Design guardrails for runtime changes

Before changing a runtime flow, ask:

- which module emits the event?
- which module owns the reaction?
- is this a domain rule or composition rule?
- does this block `httpd` or `main`?
- does it change persisted state or only runtime state?

If a change depends on multiple module states at once, it probably belongs in `system_runtime` or another composition-layer component, not in one of the leaf managers.
