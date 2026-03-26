# BluButtonBridge

ESP-IDF v6.1 firmware for ESP32 that bridges **Shelly BLU Button** BLE events to **MQTT** publish actions and **GPIO** outputs, with a built-in web configuration UI.

Press a Shelly button → the ESP32 receives the encrypted BLE advertisement, decrypts it, and triggers any combination of MQTT publishes and GPIO toggles you've configured — no cloud required.

---

## Use cases

**Gate opener with a direct relay** — Wire a relay to a GPIO pin on the ESP32 and connect it to your gate motor. Configure a single press on your Shelly button to toggle the relay with a 1-second restore delay. Press the button from your car, the gate opens — no WiFi or cloud involved.

**Gate opener via MQTT** — Your gate already has a smart relay (Shelly, Sonoff, etc.) connected to your home automation system? Configure the button to publish an MQTT message (e.g. `cmnd/gate/POWER TOGGLE`) and let your existing infrastructure handle it.

**Gate + alarm in one press** — Map a single press to two MQTT actions: one to open the gate and one to arm/disarm the alarm system. Arriving home? One press on the Shelly button opens the gate and disarms the alarm at the same time.

**Gate + outdoor lights** — Combine GPIO and MQTT in the same event: a single press opens the gate via relay and publishes an MQTT message to turn on the outdoor lights. Or use double press for the gate alone and single press for gate + lights — each event type is independently configurable.

**Multiple buttons, multiple zones** — Register up to 8 Shelly buttons, each with its own action mapping. One button for the gate, another for the garage, a third on your nightstand to arm the alarm and kill all lights.

---

## Features

| Area | Highlights |
|------|-----------|
| **BLE** | Passive scan for BTHome v2 encrypted devices (Shelly BLU Button); AES-128-CCM decryption via PSA crypto; anti-replay counter; up to 8 devices; 4 event types (single / double / triple / long press) |
| **MQTT** | Configurable broker (host, port, user/pass, optional TLS); up to 16 named publish actions (topic + payload); QoS 1; auto-reconnect |
| **GPIO** | Up to 16 output actions; configurable pin, idle state, active level, action type (set on / set off / toggle), auto-restore delay |
| **WiFi** | STA + SoftAP coexistence; captive-portal DNS for first-time setup; auto-reconnect (5 s retry); WPA2-PSK on AP |
| **Web UI** | Single-page app served from the ESP32; tabs for WiFi, MQTT, GPIO, BLE, System; real-time status bar; mobile-friendly |
| **System** | OTA firmware update via file upload; full config backup/restore (JSON); reboot and factory-reset from the UI; NVS persistence across reboots |

---

## How it works

```
Shelly BLU Button  ─── BLE adv (encrypted BTHome v2) ──►  ESP32
                                                            │
                                              AES-128-CCM decrypt
                                              anti-replay check
                                              parse button event
                                                            │
                                         ┌──────────────────┴──────────────────┐
                                         ▼                                     ▼
                                    MQTT publish                         GPIO toggle
                                  (up to 16 actions)                 (up to 16 actions)
```

Each of the 4 button events (single, double, triple, long press) can independently trigger any combination of MQTT and GPIO actions via per-device bitmasks.

---

## Hardware

- **ESP32 DevKit V1** (ESP-WROOM-32) — 4 MB flash
- **Shelly BLU Button** (or any BTHome v2 encrypted BLE device)

### GPIO pin map

| Pin | Usage |
|-----|-------|
| **0** | BOOT button — hold 3 s to start AP, hold 10 s to factory reset |
| **2** | System LED (blue) — AP/BOOT feedback plus WiFi and MQTT warning patterns |
| 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33 | Available for user GPIO actions |

Boot-sensitive and flash pins are excluded from user configuration.

### BOOT button (field recovery)

If the device can't connect to WiFi (e.g. changed password, network down), use the physical BOOT button on the ESP32 board:

| Action | Hold time | LED feedback |
|--------|-----------|-------------|
| **Start AP** | 3 seconds | Slow blink |
| **Factory reset** | 10 seconds | Fast blink |

After releasing the button, the action triggers. The AP starts on `192.168.4.1` so you can reconfigure via the web UI from your phone.

### System LED patterns

The blue system LED also provides quick field diagnostics during normal operation:

| LED pattern | Meaning |
|-------------|---------|
| **Off** | Normal state: WiFi is connected and no higher-priority warning is active |
| **Slow continuous blink** | AP mode is active |
| **2 blinks + pause** | WiFi is configured but currently disconnected |
| **3 blinks + pause** | WiFi is up, MQTT is configured, but the broker is not connected |

Priority is intentional: AP mode overrides warning patterns, and WiFi-down overrides MQTT-down.

---

## Project structure

```
blu-button-bridge/
├── main/
│   └── app_main.c              # Entry point, init order
├── components/
│   ├── ble_access/             # BLE scan, BTHome v2 decrypt, device management
│   ├── wifi_manager/           # STA + SoftAP, captive portal DNS, credentials
│   ├── mqtt_manager/           # MQTT client, named publish actions
│   ├── gpio_manager/           # GPIO output actions, system LED
│   └── web_manager/            # HTTP server, REST API, embedded index.html
├── partitions.csv              # Custom partition table (NVS + OTA_0 + OTA_1)
├── sdkconfig                   # ESP-IDF configuration
└── CMakeLists.txt
```

### NVS namespaces

| Namespace | Content |
|-----------|---------|
| `wifi` | STA SSID + password |
| `ap_cfg` | AP SSID, password, enabled flag |
| `mqtt` | Broker host, port, credentials, TLS flag, publish actions |
| `ble_access` | Registered devices (MAC, key, label, event masks, counter) |
| `gpio` | GPIO output actions |

---

## Initialization order

```
app_main()
  ├── nvs_flash_init()          # NVS with auto-recovery on corruption
  ├── gpio_manager_init()       # System LED + load GPIO actions from NVS
  ├── wifi_init()               # Load credentials, start AP if unconfigured
  ├── mqtt_init()               # Auto-connect if broker configured
  ├── ble_access_init()         # NimBLE + PSA crypto, load devices, start scan
  └── web_manager_init()        # HTTP server on port 80
```

---

## BLE — Shelly BLU Button

### Encryption & decryption

Shelly BLU Button uses **BTHome v2 with AES-128-CCM** encryption. Each advertisement contains:

```
[UUID 2B][DevInfo 1B][Encrypted payload NB][Counter 4B][MIC 4B]
```

The firmware builds a 13-byte nonce (`MAC[6 MSB-first] + UUID[2 LE] + DevInfo[1] + Counter[4 LE]`), decrypts with PSA crypto, verifies the 4-byte MIC tag, and enforces a strictly-increasing 32-bit counter for anti-replay protection.

> **Note:** Shelly deviates from the BTHome spec in two ways: Counter comes before MIC (not after), and the nonce MAC bytes are in MSB-first (display) order. This is handled transparently.

### Shelly app setup (required)

Encryption **must** be enabled on the Shelly BLU Button before it can work with BluButtonBridge. In the Shelly BLE app:

1. Open the device → **Settings** → **Encryption**
2. Enable encryption — the app will generate an AES key
3. Copy the key from **Settings → Keys** — you will need it during registration

Without encryption enabled, the button sends unencrypted BTHome advertisements which this firmware does not support.

> **Battery replacement note:** after removing and reinserting the battery, the Shelly BLU Button stops sending encrypted advertisements until you reconnect it with the Shelly BLE app at least once. This is a device-level behaviour and cannot be resolved from the ESP32 side.

### Device registration

1. Web UI → **BLE** tab → **Start registration**
2. Press the Shelly button; the pending MAC appears
3. Enter the 32-hex-char AES key (copied from the Shelly app), a label, and assign MQTT/GPIO actions per event
4. **Save** — the device is stored in NVS and active immediately

### Button events

| Event value | Meaning |
|-------------|---------|
| 0 | None / release (no action triggered) |
| 1 | Single press |
| 2 | Double press |
| 3 | Triple press |
| 4 | Long press |

Each event independently maps to a 16-bit bitmask for MQTT actions and a 16-bit bitmask for GPIO actions.

### Crypto error handling

- **Key import error** — PSA rejected the key; shown in the UI with a retry button
- **Decrypt error** — 3+ consecutive decrypt failures flag a likely key mismatch; the UI shows a warning

---

## WiFi

### STA mode

- Connects to saved SSID/password from NVS
- Auto-reconnect with 5-second retry timer on disconnect or failure
- 10-second connection timeout

### AP mode (SoftAP)

- Default SSID: `BBB-XXXXXX` (last 3 octets of MAC)
- Default password: `12345678` (WPA2-PSK; open if < 8 chars)
- IP: `192.168.4.1`, channel 1, max 4 clients
- Captive-portal DNS server redirects all domains to `192.168.4.1`
- Starts automatically on boot if no STA credentials are saved
- Stops automatically when STA obtains an IP (unless AP is set to always-on)

### BLE / WiFi coexistence

The ESP32 shares a single radio for WiFi and BLE. BLE passive scanning can interfere with the WPA2 4-way handshake on the AP. To work around this:

- **BLE scan stops** when the AP starts
- **BLE scan resumes** when the AP stops

A disclaimer in the web UI reminds users that BLE scanning is paused while the AP is active.

---

## MQTT

- Configurable broker: host, port, username, password, optional TLS/SSL
- Auto-reconnect on disconnect (2 s timeout)
- **Up to 16 publish actions**, each with a name, topic, and payload
- Actions are triggered by BLE button events through per-device bitmasks
- QoS 1 for reliable delivery

---

## GPIO output actions

- **Up to 16 actions**, each bound to a GPIO pin
- Action types: `set_on`, `set_off`, `toggle`
- **Active level**: HIGH (normal) or LOW (inverted for active-low relays)
- **Idle state**: default pin level when no action is active
- **Restore delay**: auto-restore to idle state after N ms (0 = permanent)

---

## Web UI

Served directly from the ESP32 on port 80 — no external dependencies.

### Tabs

| Tab | Functions |
|-----|-----------|
| **WiFi** | Connect to a network, scan nearby APs, manage AP settings, clear credentials |
| **MQTT** | Configure broker, manage publish actions (add / edit / delete / test) |
| **GPIO** | Manage GPIO output actions (add / edit / delete / test), pin validation |
| **BLE** | List devices, register new devices, assign events to actions, manage keys |
| **System** | OTA firmware update, config backup & restore, reboot, factory reset |

### Status bar

Auto-refreshes every ~2 s with color-coded indicators for WiFi, MQTT, and AP status.

---

## REST API

All endpoints are served on port 80 with JSON payloads.

### Status
| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/status` | WiFi, MQTT, AP status |

### WiFi
| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/wifi/config` | Saved SSID, password-set flag |
| POST | `/api/wifi/connect` | Connect to `{ssid, password}` |
| DELETE | `/api/wifi` | Clear credentials and disconnect |
| GET | `/api/wifi/scan` | Scan nearby networks |

### AP
| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/ap/config` | AP settings |
| POST | `/api/ap/config` | Save AP settings `{enabled, ssid, password}` |
| POST | `/api/ap/start` | Start AP |
| POST | `/api/ap/stop` | Stop AP |

### MQTT
| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/mqtt/config` | Broker settings (password hidden) |
| POST | `/api/mqtt/connect` | Connect `{host, port, username, password, tls}` |
| DELETE | `/api/mqtt` | Clear broker credentials |
| GET | `/api/mqtt/actions` | List publish actions |
| POST | `/api/mqtt/actions` | Add action `{name, topic, payload}` |
| PUT | `/api/mqtt/action` | Update action `{idx, name, topic, payload}` |
| DELETE | `/api/mqtt/action` | Delete action `{idx}` |
| POST | `/api/mqtt/action/test` | Trigger action manually `{idx}` |

### GPIO
| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/gpio/pins` | Allowed GPIO pin list |
| GET | `/api/gpio/actions` | List GPIO actions |
| POST | `/api/gpio/actions` | Add action |
| PUT | `/api/gpio/action` | Update action |
| DELETE | `/api/gpio/action` | Delete action `{idx}` |
| POST | `/api/gpio/action/test` | Trigger action manually `{idx}` |

### BLE
| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/ble/devices` | List registered devices |
| POST | `/api/ble/register/start` | Enter registration mode |
| GET | `/api/ble/register/poll` | Poll for pending MAC |
| POST | `/api/ble/register/confirm` | Confirm registration `{mac, key, label}` |
| POST | `/api/ble/register/cancel` | Cancel registration |
| PUT | `/api/ble/device` | Update device settings |
| POST | `/api/ble/device/key` | Update encryption key `{mac, key}` |
| POST | `/api/ble/device/reimport` | Re-import key into PSA |
| DELETE | `/api/ble/device` | Delete device `{mac}` |

### System
| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/api/system/reboot` | Restart the ESP32 |
| POST | `/api/system/factory-reset` | Erase all NVS and restart |
| POST | `/api/system/ota` | Upload firmware binary (OTA update) |
| GET | `/api/system/config` | Download full configuration (JSON) |
| POST | `/api/system/config` | Restore configuration from JSON and reboot |

---

## Limits

| Resource | Max |
|----------|-----|
| BLE devices | 8 |
| MQTT publish actions | 16 |
| GPIO output actions | 16 |
| WiFi scan results | 20 |
| AP clients | 4 |
| MQTT payload | 1024 bytes |
| MQTT topic | 128 chars |
| Labels / names | 32 chars |

---

## Environment setup

```bash
mkdir -p ~/esp && cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
PYTHON=$(which python3.12) ./install.sh esp32
```

## Build & flash

```bash
# Activate IDF environment (every new terminal)
source ~/esp/esp-idf/export.sh

# Build and flash
idf.py -p /dev/cu.usbserial-0001 build flash

# Serial monitor (Ctrl+T then Ctrl+X to exit)
idf.py -p /dev/cu.usbserial-0001 monitor
```

If flash hangs on `Connecting...`, hold the **BOOT** button on the ESP32 until it starts.

---

## Troubleshooting

| Problem | Likely cause | Fix |
|---------|-------------|-----|
| `idf.py` not found | `export.sh` not sourced | `source ~/esp/esp-idf/export.sh` |
| Flash stuck on `Connecting...` | ESP32 not in flash mode | Hold **BOOT** button during flash |
| Port busy | Monitor open elsewhere | Close monitor with Ctrl+T Ctrl+X |
| Build fails after fullclean | Target not set | `idf.py set-target esp32` then build |
| Can't connect to AP WiFi | BLE scan running | AP stops BLE scan automatically; if issue persists, reboot |
| BLE device not decrypting | Wrong key or key out of sync | Check key in Shelly app; re-register or update key via UI |
| `event=0` in BLE log | Normal button release beacon | No action expected — this is idle/release state |
| WiFi stays in ERROR | Connection failed, no retry | Fixed in firmware; should auto-retry every 5 s |
| AP won't stop after STA connects | AP set to always-on | Check AP settings in web UI (`enabled` flag) |

---

## License

This project is licensed under the **GNU General Public License v3.0** — see the [LICENSE](LICENSE) file for details.
