# ISDT 608PD ESP32-S3 HMI

Single-firmware PlatformIO project for the **LCDWIKI ES3N28P / ES3C28P** (ESP32-S3 + 2.8" ILI9341).

Replicates the native ISDT 608PD display style with LVGL, adds full HMI navigation, a hidden-style System Settings screen, opcode logging, and a WebUI mirror.

## Features
- BLE client for ISDT 608PD (bind + Electric/WorkState polling)
- LVGL HMI with 608PD-style colored Status screen
- Screens: Status, Menu, Opcode Log (table), Control, System Settings
- System Settings mimics the charger's hidden menu (long-press center in standby)
- WebUI SoftAP mirror (live status + log)
- Dual-core (BLE on Core 1), PSRAM LVGL buffer
- Auto-reconnect, timeout recovery, watchdog

## Hardware
| Function | GPIO |
|----------|------|
| TFT CS   | 10   |
| TFT DC   | 46   |
| TFT SCK  | 12   |
| TFT MOSI | 11   |
| TFT MISO | 13   |
| TFT BL   | 45   |

Board: ES3N28P (no touch) or ES3C28P (with touch). Rotation is landscape (320×240).

## LVGL Visuals
The Status screen uses color coding matching the 608PD:
- Yellow: chemistry (LiPo)
- Cyan: cell count (6S) and capacity (Ah)
- Orange: current (A) and power (W)
- White: pack voltage
- Pink: temperature
- Dark background, minimal chrome

Other screens use simple high-contrast buttons and tables for reliability.

## Hidden System Settings
Accessible from Menu → System Settings (or intended as long-press equivalent on Confirm when idle).

Options:
- Backlight: Low / Med / High
- Save / Cancel
- About (firmware version)

Matches the charger's standby long-press center button behavior for system defaults.

## Detailed ESP32 / PlatformIO Instructions

### Prerequisites
- VS Code + PlatformIO extension
- USB cable to the ES3N28P/ES3C28P
- ISDT 608PD powered and in range

### Build & Flash
```bash
# Clone
git clone https://github.com/wilcox101-maker/ISDT-608PD-ESP32-HMI.git
cd ISDT-608PD-ESP32-HMI

# Build
pio run

# Flash
pio run -t upload

# Monitor
pio device monitor -b 115200
```

### First Boot
1. SoftAP starts: `ISDT-608PD` / password `isdt608pd`
2. Status screen appears and begins BLE scan
3. Connect a phone/laptop to the SoftAP and open `http://192.168.4.1` for the WebUI mirror
4. Use MENU button to navigate

### Troubleshooting
- No display: verify pins and rotation; check Serial for PSRAM buffer message
- No BLE: ensure 608PD is on and advertising (company ID 0xABBA)
- Compile errors: clean with `pio run -t clean` then rebuild
- WebUI blank: confirm SoftAP connection and refresh

## Screen Workflow
```
Power On → Status (608PD style)
   └─[MENU]─→ Menu
         ├─ Status
         ├─ Opcode Log (table)
         ├─ Control (Adjust − / Confirm / Adjust + / Start / Stop)
         ├─ System Settings (Backlight / Save / Cancel / About)
         └─ Reconnect
```

## Protocol Reference
- Service UUID: `0000af00-0000-1000-8000-00805f9b34fb`
- AF01: data polling
- AF02: bind (`0x18` + 16-byte client UUID)
- Electric (`0xE4`/`0xE5`): short and long formats (pack V, current, cells)
- WorkState (`0xE6`/`0xE7`): state, capacity %, error

Full details: https://github.com/mtheli/isdt_air_ble/blob/master/PROTOCOL.md

## Notes
- Control opcodes in code are placeholders. Capture real commands from the official ISDT Link app and replace the values in `sendControl()`.
- Backlight level is stored in RAM (add NVS for persistence if desired).
- Temperature and exact Ah values are derived where protocol data is available.

## License
MIT
