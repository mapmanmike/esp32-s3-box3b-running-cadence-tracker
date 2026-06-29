---
name: esp-idf-box3-builder
description: Build, patch, and flash native ESP-IDF apps for the ESP32-S3-BOX-3 / BOX-3B (LCD + touch + IMU + audio via the espressif/esp-box-3 BSP and LVGL). Use when developing ESP-IDF firmware for these boards on macOS, or when hitting BOX-3B-specific build/flash/peripheral failures (touch abort, octal PSRAM, I2C driver conflict, color swap, USB-JTAG wedge).
---

# ESP-IDF ESP32-S3-BOX-3 / BOX-3B builder

Native **ESP-IDF** (not ESPHome) workflow for the ESP32-S3-BOX-3 and the basic
**BOX-3B** variant. Covers environment setup, the `espressif/esp-box-3` BSP +
LVGL, and the board-specific fixes needed to get the screen, touch, and IMU
working. All gotchas here were validated on real BOX-3B hardware.

> The BOX-3B is the basic variant of the BOX-3: same ESP32-S3 SoC + mainboard,
> fewer accessories. Software target is `esp32s3`; BSP is `espressif/esp-box-3`.
> Treat BOX-3 and BOX-3B as the same target for firmware.

## When to use this skill
- Setting up ESP-IDF for the BOX-3/3B on a new (esp. Apple Silicon) Mac.
- Bringing up LCD / touch / IMU via the esp-box-3 BSP + LVGL.
- Debugging a BOX-3B that boot-loops, shows a black/purple screen, won't flash,
  or whose touch/IMU "isn't found".

## Quick start (verified)
```bash
# 0. Deps (Apple Silicon): brew install cmake ninja dfu-util ccache
# 1. ESP-IDF v5.3.x
mkdir -p ~/esp && cd ~/esp
git clone -b v5.3.1 --recursive https://github.com/espressif/esp-idf.git
cd ~/esp/esp-idf && ./install.sh esp32s3
# 1b. FIX the Python env install.sh leaves broken on py3.9 (see references):
PYENV=~/.espressif/python_env/idf5.3_py3.9_env/bin/python
$PYENV -m pip install "setuptools<81" "ruamel.yaml<0.18" "ruamel.yaml.clib==0.2.8"
# 2. Per shell:
. ~/esp/esp-idf/export.sh
# 3. In your project:
idf.py set-target esp32s3          # fetches esp-box-3 BSP
python3 patches/apply_bsp_patches.py   # re-apply BOX-3B BSP fixes
idf.py build && idf.py -p /dev/cu.usbmodem101 flash monitor
```

## Connection (first, because it blocks everything)
- Plug a **data** USB-C cable **directly into the device**, not the dock/base
  (the base has a USB hub the S3 native USB doesn't reliably enumerate through).
- Verify: `ls /dev/cu.usbmodem*` and
  `ioreg -p IOUSB -l -w0 | grep -i "JTAG\|303a"` → "USB JTAG_serial debug unit",
  idVendor 12346 (0x303A Espressif).

## Minimal project config that actually works
`main/idf_component.yml` — pin to the known-good set:
```yaml
dependencies:
  espressif/esp-box-3: "1.1.3"
  espressif/button: "3.5.0"
  lvgl/lvgl: "8.4.0"
  espressif/esp_codec_dev: "1.1.0"   # 1.5.x -> new i2c_master -> conflict abort
  idf: { version: ">=5.1" }
```
`sdkconfig.defaults` — non-negotiable bits:
```ini
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y     # logs over USB-C
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y                  # BOX-3 is OCTAL psram, not quad
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_LV_COLOR_16_SWAP=y                 # SPI panel wants byte-swapped RGB565
```

## Display + touch bring-up
- Call `bsp_display_start()` (brings up LCD + LVGL **and** drives the shared
  GPIO48 reset that the GT911 touch needs). Then `bsp_display_backlight_on()`.
- Wrap all LVGL calls in `bsp_display_lock(0)` / `bsp_display_unlock()`.
- Apply the two BSP patches (see `references/box3b-idf-gotchas.md` and the
  `apply_bsp_patches.py` shipped with the cadence firmware) or touch aborts.

## IMU
- ICM-4267x on I²C **0x68**: `icm42670_create(BSP_I2C_NUM, ICM42670_I2C_ADDRESS)`,
  `icm42670_config()`, `icm42670_acce_set_pwr(ACCE_PWR_LOWNOISE)`,
  `icm42670_get_acce_value()`. Add `icm42670` to your component REQUIRES.
- WHO_AM_I reports 0x60 on the 3B (a compatible variant); the driver works anyway.

## References
- `references/box3b-idf-gotchas.md` — the full failure→cause→fix table (touch
  scl_speed_hz, octal PSRAM, I2C conflict, color swap, broken py env, USB-JTAG
  wedge, dock-hub USB) with the exact BSP patches.
- `references/box3-pinout.md` — GPIO/I²C address map (see the sibling
  esphome-box3-builder skill for the hardware pinout; addresses confirmed by
  live I²C scan: touch GT911 0x5D, IMU 0x68, mic ES7210 0x40, spkr ES8311 0x18).
