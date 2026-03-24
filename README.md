# BluButtonBridge

ESP-IDF v5.4 project for ESP32 DevKit V1 (ESP-WROOM-32).

Shelly Blu Button to MQTT bridge for ESP32, with local web configuration UI.

---

## Features

- **WiFi** — STA mode with SoftAP fallback; captive-portal DNS for first-time setup
- **MQTT** — Configurable broker plus named publish actions assignable to BLE events
- **BLE** — Passive scan for BTHome v2 encrypted devices (Shelly BLU Button); up to 8 registered devices; per-event MQTT actions (single/double/triple/long press)
- **Web UI** — Embedded single-page app served directly from the ESP32 (tabs: WiFi, MQTT, BLE, System)

---

## Hardware

### Components

- ESP32 DevKit V1 (ESP-WROOM-32)
- BLE devices: Shelly BLU Button (BTHome v2 encrypted)

---

## Project structure

```
blu-button-bridge/
├── main/
│   ├── app_main.c          # Entry point
│   └── CMakeLists.txt
├── components/
│   ├── wifi_manager/       # WiFi STA + SoftAP, captive portal
│   ├── mqtt_manager/       # MQTT client + named actions
│   ├── ble_access/         # BLE passive scan, BTHome v2 decryption
│   └── web_manager/        # HTTP server + embedded index.html
├── CMakeLists.txt
└── sdkconfig
```

NVS namespaces: `wifi`, `mqtt`, `ap_cfg`, `ble_access`.

---

## Environment setup (once)

```bash
mkdir -p ~/esp && cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
PYTHON=$(which python3.12) ./install.sh esp32
```

---

## Daily workflow

```bash
# Activate environment (every new terminal)
source ~/esp/esp-idf/export.sh

# Build + flash
idf.py -p /dev/cu.usbserial-0001 build flash

# Serial monitor (separate terminal)
idf.py -p /dev/cu.usbserial-0001 monitor
# Exit monitor: Ctrl+T then Ctrl+X
```

If flash hangs on `Connecting...`, hold the **BOOT** button on the ESP32 until it starts.

---

## BLE — Shelly BLU Button setup

1. Open the web UI -> **BLE** tab -> **Start registration**
2. Press the Shelly button once; the MAC address appears
3. Enter the AES-128 encryption key (from the Shelly app: Device -> Settings -> Keys), label, and configure MQTT actions per event
4. Save — the device is stored in NVS and active immediately

Supported events: single press, double press, triple press, long press.

> **Note:** Shelly BLU Button uses BTHome v2 with two deviations from the spec — the frame layout puts Counter before MIC, and the nonce uses MAC bytes in MSB-first order. This is handled internally by `ble_access`.

---

## Troubleshooting

| Problem | Likely cause | Fix |
|---------|-------------|-----|
| `idf.py` not found | export.sh not sourced | `source ~/esp/esp-idf/export.sh` |
| Flash stuck on `Connecting...` | ESP32 not in flash mode | Hold BOOT button during flash |
| Port busy | Monitor open elsewhere | Close monitor with Ctrl+T Ctrl+X |
| Build fails after fullclean | Target not set | `idf.py set-target esp32` then build |
| BLE device not decrypting | Wrong key or NVS corrupt | Re-register device; check key has no spaces |
| NVS data lost after firmware update | Struct size changed | Re-enter credentials via web UI |
