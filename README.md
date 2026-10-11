# ISDT 608PD ESP32-S3 HMI

Single-firmware PlatformIO project for the ES3N28P / ES3C28P (ESP32-S3 + 2.8" ILI9341).

## Features
- BLE client for ISDT 608PD (monitoring + control placeholders)
- LVGL HMI mimicking the native 608PD display style
- Status, Menu, Opcode Log, Control, and System Settings screens
- WebUI screen mirror (SoftAP)
- Dual-core BLE task + PSRAM LVGL buffer
- Auto-reconnect and recovery

## Build & Flash
```bash
pio run -t upload
```

SoftAP: `ISDT-608PD` / `isdt608pd`  
WebUI: `http://192.168.4.1`

## Notes
Control opcodes are placeholders. Capture real commands from the official app and update `sendControl` calls.
