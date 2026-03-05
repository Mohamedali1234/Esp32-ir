# ESP32 IR Remote + Dazzler

WiFi IR remote control system for generic ESP32 with web UI, IR learning, and IR flood (Dazzler).

---

## ⚡ Get the .bin files (no coding needed)

### Step 1 — Create a GitHub repo

1. Go to [github.com](https://github.com) → sign in or create a free account
2. Click **+** → **New repository**
3. Name it `esp32-ir-remote`, set it to **Public**, click **Create repository**

### Step 2 — Upload the project files

Drag and drop ALL these files into the repo (maintain folder structure):

```
esp32_ir_unified.ino
platformio.ini
data/
  index.html
.github/
  workflows/
    build.yml
```

Or use Git:
```bash
git init
git add .
git commit -m "Initial commit"
git remote add origin https://github.com/YOURUSERNAME/esp32-ir-remote.git
git push -u origin main
```

### Step 3 — Download your .bin files

1. Go to your repo on GitHub
2. Click the **Actions** tab
3. Click the latest **Build ESP32 IR Remote** run
4. Wait ~3 minutes for it to finish (green checkmark ✓)
5. Either:
   - Scroll down to **Artifacts** → click **ESP32-IR-Remote-xxxxx** to download a zip
   - OR click the **Releases** tab on the repo main page → download files directly

You get two files:
| File | What it is |
|------|-----------|
| `esp32_ir_remote_firmware.bin` | The main firmware |
| `esp32_ir_remote_spiffs.bin` | The web UI |

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
