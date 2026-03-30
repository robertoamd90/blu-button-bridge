# Where To Start

This is a short map for common changes so agents do not have to rediscover entry points every time.

## New or changed web/API endpoint

Start with:

- `components/web_manager/web_manager.c`
- `components/web_manager/index.html`
- `docs/API_CONTRACTS.md`

Look for:

- request parsing
- response shape
- auth requirement
- UI call site

## WiFi or AP behavior

Start with:

- `components/wifi_manager/wifi_manager.c`
- `components/wifi_manager/include/wifi_manager.h`
- `components/system_runtime/system_runtime.c`
- `docs/RUNTIME_FLOW.md`

Look for:

- status transitions
- reconnect behavior
- AP side effects
- runtime callbacks

## MQTT behavior or publish actions

Start with:

- `components/mqtt_manager/mqtt_manager.c`
- `components/mqtt_manager/include/mqtt_manager.h`
- `components/system_runtime/system_runtime.c`
- `docs/API_CONTRACTS.md`
- `docs/RUNTIME_FLOW.md`

Look for:

- status callbacks
- saved config contract
- action indexing
- network availability handoff

## BLE registration, telemetry, or event handling

Start with:

- `components/ble_access/ble_access.c`
- `components/web_manager/web_manager.c`
- `components/web_manager/index.html`
- `docs/API_CONTRACTS.md`

Look for:

- registration state
- device struct fields
- telemetry exposure
- event mask semantics

## GPIO actions or system LED behavior

Start with:

- `components/gpio_manager/gpio_manager.c`
- `components/system_runtime/system_runtime.c`
- `docs/RUNTIME_FLOW.md`

Look for:

- action validation
- allowed GPIOs
- LED policy vs LED application

## OTA behavior

Start with:

- `components/ota_manager/ota_manager.c`
- `components/ota_manager/include/ota_manager.h`
- `components/web_manager/web_manager.c`
- `main/app_main.c`
- `docs/API_CONTRACTS.md`
- `docs/RUNTIME_FLOW.md`

Look for:

- staged job persistence
- manual upload contract
- boot takeover
- failure recovery

## Backup / restore or persisted config

Start with:

- `components/web_manager/web_manager.c`
- module-specific NVS code in:
  - `components/wifi_manager/wifi_manager.c`
  - `components/mqtt_manager/mqtt_manager.c`
  - `components/gpio_manager/gpio_manager.c`
  - `components/ble_access/ble_access.c`
  - `components/ota_manager/ota_manager.c`
- `docs/API_CONTRACTS.md`

Look for:

- exported JSON shape
- restore tolerance vs strict validation
- namespace and key compatibility

## Boot / init order / cross-module composition

Start with:

- `main/app_main.c`
- `components/system_runtime/system_runtime.c`
- `docs/RUNTIME_FLOW.md`

Look for:

- what is boot orchestration only
- what belongs to a domain manager
- what belongs to runtime composition

## Release or versioning changes

Start with:

- `CMakeLists.txt`
- `docs/WORKFLOW.md`
- `README.md`

Look for:

- `git describe --tags --always --dirty`
- tag-before-build rule
- artifact naming and checksum reporting
