# ESP32 IR Remote + Dazzler

WiFi IR remote control system for generic ESP32 with web UI, IR learning, and IR flood (Dazzler).

---

## 🔌 Flash to ESP32

### Option A — ESP Flash Download Tool (Windows, easiest GUI)

1. Download from: https://www.espressif.com/en/support/download/other-tools
2. Select chip: **ESP32**
3. Add both files:
   - `esp32_ir_remote_firmware.bin` → address `0x10000`
   - `esp32_ir_remote_spiffs.bin`   → address `0x290000`
4. Select your COM port, baud `921600`
5. Click **START**

### Option B — esptool.py (command line)

```bash
pip install esptool

# Flash firmware
esptool.py --chip esp32 --port COM3 --baud 921600 write_flash 0x10000 esp32_ir_remote_firmware.bin

# Flash web UI
esptool.py --chip esp32 --port COM3 --baud 921600 write_flash 0x290000 esp32_ir_remote_spiffs.bin
```

> Replace `COM3` with your port:
> - Windows: `COM3`, `COM4`, etc. (check Device Manager)
> - Linux: `/dev/ttyUSB0` or `/dev/ttyACM0`
> - Mac: `/dev/cu.usbserial-*`

### Option C — Flash both at once

```bash
esptool.py --chip esp32 --port COM3 --baud 921600 write_flash \
  0x1000  bootloader.bin \
  0x8000  partitions.bin \
  0x10000 esp32_ir_remote_firmware.bin \
  0x290000 esp32_ir_remote_spiffs.bin
```

---

## 📱 First Boot

1. Connect to WiFi network: **`IR Remote`**
2. Password: **`IRREMOTE123`**
3. Open browser: **`http://192.168.4.1`**

---

## 🔌 Wiring (Default Pins)

| Function | GPIO | Notes |
|----------|------|-------|
| IR TX (LED) | 4 | Use transistor for max range |
| IR RX (Receiver) | 14 | TSOP4838 or VS1838 |
| SD CS | 5 | |
| SD MOSI | 23 | VSPI |
| SD MISO | 19 | VSPI |
| SD SCK | 18 | VSPI |

All pins are changeable from the web UI → Settings tab.

---

## 📦 Libraries Used

| Library | Author |
|---------|--------|
| IRremoteESP8266 | crankyoldgit |
| ArduinoJson v6 | Benoit Blanchon |
| ESPAsyncWebServer | lacamera |
| AsyncTCP | me-no-dev |
