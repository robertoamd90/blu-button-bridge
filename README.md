# BluButtonBridge

ESP-IDF v6.1 firmware for ESP32 that bridges **Shelly BLU Button** BLE events to **MQTT** publish actions and **GPIO** outputs, with a built-in web configuration UI.

Press a Shelly button, the ESP32 receives the encrypted BLE advertisement, and triggers the MQTT publishes and GPIO toggles you configured. No cloud required.

## What It Can Do

- Register up to 8 Shelly BLU Button devices
- Map single, double, triple, and long press events to MQTT and GPIO actions
- Serve a built-in web UI for WiFi, MQTT, GPIO, BLE, and system settings
- Support manual OTA uploads and staged OTA installs from GitHub releases
- Protect the UI and API with optional browser-native HTTP Basic Auth
- Expose lightweight BLE telemetry such as last seen, last event, and battery when available

## Typical Uses

- Open a gate through a relay connected to an ESP32 GPIO
- Publish MQTT commands to an existing home automation setup
- Trigger multiple actions from the same button event
- Use different buttons for different zones or automations

## Hardware

- **ESP32 DevKit V1** (ESP-WROOM-32, 4 MB flash)
- **Shelly BLU Button**

### GPIO pin map

| Pin | Usage |
|-----|-------|
| **0** | BOOT button, hold 3 s to start AP, hold 10 s to factory reset |
| **2** | System LED |
| 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33 | Available for user GPIO actions |

Boot-sensitive and flash pins are excluded from user configuration.

### Recovery shortcuts

| Action | Hold time | LED feedback |
|--------|-----------|-------------|
| **Start AP** | 3 seconds | Slow blink |
| **Factory reset** | 10 seconds | Fast blink |

The AP starts on `192.168.4.1`.

### System LED patterns

| LED pattern | Meaning |
|-------------|---------|
| **Off** | Normal state |
| **Slow continuous blink** | AP mode is active |
| **2 blinks + pause** | WiFi is configured but disconnected |
| **3 blinks + pause** | WiFi is up, MQTT is configured, but broker is disconnected |

## First-Time Setup

1. Flash the firmware to the ESP32.
2. Open the web UI. If WiFi is not configured yet, the device starts its AP automatically.
3. Configure WiFi and, if needed, MQTT.
4. In the **BLE** tab, start registration and press the Shelly button.
5. Paste the 32-character encryption key from the Shelly app and save the device.
6. Assign MQTT and GPIO actions to the events you want.

## Shelly BLU Button Notes

- Encryption must be enabled in the Shelly app before the button can work with BluButtonBridge.
- You need to copy the AES key from the Shelly app during registration.
- After a battery reinsertion, the button may stop sending encrypted advertisements until it is reconnected once from the Shelly app.

## Web UI And API

The device serves a built-in web UI on port 80 with tabs for **WiFi**, **MQTT**, **GPIO**, **BLE**, and **System**.

The HTTP surface includes:

- JSON configuration and control endpoints
- raw binary upload for manual OTA
- SSE log streaming for the web console

Detailed request and response contracts live in [docs/API_CONTRACTS.md](docs/API_CONTRACTS.md).

## Documentation

- [docs/API_CONTRACTS.md](docs/API_CONTRACTS.md): detailed API payloads, config schema, and compatibility-sensitive contracts
- [docs/CONSOLE_STREAM_CONTRACT.md](docs/CONSOLE_STREAM_CONTRACT.md): SSE event model and ownership rules for the live web console
- [docs/PAGES_INSTALLER_CONTRACT.md](docs/PAGES_INSTALLER_CONTRACT.md): GitHub Pages installer and release-asset contract
- [docs/RUNTIME_FLOW.md](docs/RUNTIME_FLOW.md): boot flow, runtime ownership, OTA takeover, and cross-module behavior
- [docs/WORKFLOW.md](docs/WORKFLOW.md): development workflow, release flow, and testing expectations
- [docs/VALIDATION.md](docs/VALIDATION.md): validation ladder and fallback rules
- [docs/WHERE_TO_START.md](docs/WHERE_TO_START.md): entry points for common changes in the repo

## Limits

| Resource | Max |
|----------|-----|
| BLE devices | 8 |
| MQTT publish actions | 16 |
| GPIO output actions | 16 |
| WiFi scan results | 20 |
| AP clients | 4 |
| MQTT action payload | 31 chars |
| MQTT action topic | 63 chars |
| Labels / names | 32 chars |

## Build And Flash

### Environment setup

```bash
mkdir -p ~/esp && cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
PYTHON=$(which python3.12) ./install.sh esp32
```

### Build

```bash
source ~/esp/esp-idf/export.sh
idf.py -p /dev/cu.usbserial-0001 build flash
```

### Monitor

```bash
source ~/esp/esp-idf/export.sh
idf.py -p /dev/cu.usbserial-0001 monitor
```

If flash hangs on `Connecting...`, hold the **BOOT** button until flashing starts.

## Browser Installer

The repository also publishes a GitHub Pages installer based on `ESP Web Tools`.

- Installer site: [robertoamd90.github.io/blu-button-bridge](https://robertoamd90.github.io/blu-button-bridge/)
- Installer source: [site/index.html](site/index.html)
- Pages workflow: [.github/workflows/pages.yml](.github/workflows/pages.yml)

The installer uses the latest public release and expects `BluButtonBridge-full.bin` to be present in the release assets.

## Troubleshooting

| Problem | Likely cause | Fix |
|---------|-------------|-----|
| `idf.py` not found | `export.sh` not sourced | `source ~/esp/esp-idf/export.sh` |
| Flash stuck on `Connecting...` | ESP32 not in flash mode | Hold **BOOT** during flash |
| Port busy | Monitor open elsewhere | Close monitor with `Ctrl+T`, then `Ctrl+X` |
| Build fails after fullclean | Target not set | `idf.py set-target esp32` then build |
| Can't connect to AP WiFi | BLE scan is active during AP transition | Reboot and retry, AP should pause BLE scanning automatically |
| GitHub OTA fails | No internet access or release metadata/assets are unavailable | Check STA connectivity and the latest release assets |
| BLE device does not decrypt | Wrong key or key out of sync | Check the key in the Shelly app, then update or re-register the device |

## License

This project is licensed under the **GNU General Public License v3.0**. See [LICENSE](LICENSE).
