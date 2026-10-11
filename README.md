# ISDT 608PD ESP32-S3 HMI

Single-firmware PlatformIO project for the **ES3N28P / ES3C28P** (ESP32-S3 + 2.8" ILI9341).

Mimics the native ISDT 608PD display style and adds full HMI control, logging, and a WebUI mirror.

## Features
- BLE client for ISDT 608PD (Electric + WorkState polling, bind handshake)
- LVGL HMI with 608PD-style Status screen (colored LiPo / 6S / V / A / Ah / W / temp)
- Screens: Status, Menu, Opcode Log (table), Control (Adjust − / Confirm / Adjust + / Start / Stop), System Settings (Backlight / Save / Cancel / About)
- WebUI screen mirror over SoftAP (live status + opcode log)
- Dual-core: BLE task on Core 1, UI/Web on Core 0
- PSRAM LVGL buffer with internal fallback
- Auto-reconnect, data timeout recovery, watchdog

## Hardware
- Board: LCDWIKI ES3N28P or ES3C28P
- Display: 2.8" ILI9341 (240×320), rotation landscape
- Pins: CS=10, DC=46, SCK=12, MOSI=11, MISO=13, BL=45

## Build & Flash
```bash
pio run -t upload
pio device monitor
```

SoftAP: `ISDT-608PD` / `isdt608pd`  
WebUI: `http://192.168.4.1`

## Screen Layouts

### Status (608PD style)
```
LiPo          6S          2.5 A
9.99 Ah               22.5 V
20.0 V    90.1 W
42 °C     107 °F
Searching...                 [MENU]
```

### Menu
Status → Opcode Log → Control → System Settings → Reconnect

### Control
```
Adjust -    Confirm    Adjust +
     Start Task
     Stop Task
```

### System Settings
Backlight: Low / Med / High  
Save / Cancel  
About

### Opcode Log
Table: timestamp | Op | Len | Key | Note

## Workflow
1. Power on → Status screen (auto BLE scan)
2. MENU → navigate to Control / Log / System Settings
3. Control buttons send commands (placeholders — replace with captured opcodes)
4. WebUI mirrors live data

## Protocol Notes
Based on the ISDT Air BLE protocol:
- Service: `0000af00-...`
- AF01: polling (Electric `0x12 0xE4`, WorkState `0x13 0xE6`)
- AF02: bind handshake (`0x18` + 16-byte UUID)
- Electric response (`0xE5`): short and long formats supported
- WorkState (`0xE7`): state, capacity %, error code

Reference: https://github.com/mtheli/isdt_air_ble/blob/master/PROTOCOL.md

## Control Opcodes
Current values in `sendControl()` are placeholders. Capture real commands from the official ISDT Link app and update them.

## License
MIT
