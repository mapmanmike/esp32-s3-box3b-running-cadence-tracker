# ESP32-S3-BOX-3B — ESP-IDF Setup Guide (Apple Silicon macOS)

Phase 1 setup: from a clean M4 Mac to a flashed LCD "hello world" on the
ESP32-S3-BOX-3B, using native **ESP-IDF v5.3.1** + the official `esp-box` BSP.

> The **BOX-3B** is the basic variant of the ESP32-S3-BOX-3 — same SoC/mainboard,
> fewer accessories. Software target is `esp32s3`; BSP is `espressif/esp-box-3`.

Verified working end-to-end on 2026-06-29 (LCD renders an LVGL UI).

---

## 0. Prerequisites

```bash
# Apple Silicon build deps (native arm64). Xcode CLT + Homebrew assumed present.
brew install cmake ninja dfu-util ccache
```

No external USB driver needed — the ESP32-S3 has a **native USB** peripheral and
enumerates as a standard CDC-ACM device (macOS handles it). No CP210x/CH340 kext.

---

## 1. Connect the board (read this — it bit us)

- **Plug the USB-C cable directly into the DEVICE, not the dock/base.** The BOX-3
  dock has a built-in USB hub; cabling through it means the ESP never enumerates
  (you'll see a Realtek hub chain instead, and Espressif VID `0x303A` is absent).
- **Use a real data cable** (not charge-only). Plug straight into the Mac (no hub).

Verify it mounted:
```bash
ls /dev/cu.usbmodem*          # e.g. /dev/cu.usbmodem1101  -> use as $PORT
# Cross-check it's really the ESP (Espressif VID 0x303A == decimal 12346):
ioreg -p IOUSB -l -w 0 | grep -iE '"USB Product Name"|"idVendor"'
# Expect: "USB JTAG_serial debug unit", idVendor 12346
```

---

## 2. Install ESP-IDF v5.3.1 (esp32s3 only)

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.3.1 --recursive https://github.com/espressif/esp-idf.git
cd ~/esp/esp-idf
./install.sh esp32s3
```

### ⚠️ Fix the Python env (install.sh leaves it broken on py3.9)
`install.sh` upgrades `setuptools` to 82 (which drops `pkg_resources`) and pulls
`ruamel.yaml` 0.19 + clib 0.2.15 whose dist-info names py3.9's `importlib.metadata`
can't resolve. Result: every `idf.py` command fails with "requirements not satisfied".

```bash
PYENV=~/.espressif/python_env/idf5.3_py3.9_env/bin/python
$PYENV -m pip install "setuptools<81" "ruamel.yaml<0.18" "ruamel.yaml.clib==0.2.8"
```

---

## 3. Environment + esp-box BSP

```bash
# Source IDF env — REQUIRED in every new shell before idf.py
cd ~/esp/esp-idf && . ./export.sh

# Official examples / BSP repo
cd ~/esp
git clone --recursive https://github.com/espressif/esp-box.git
```

---

## 4. Quick toolchain sanity check (bundled hello_world)

```bash
cd ~/esp/esp-idf/examples/get-started/hello_world
idf.py set-target esp32s3
idf.py -p /dev/cu.usbmodem1101 flash monitor   # exit monitor: Ctrl+]
```
Build + flash should succeed and hashes verify. (Note: BOX-3/3B default console is
UART0 pins; set `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` to see logs over USB-C.)

---

## 5. The real proof: light up the LCD

The stock `esp-box/examples/factory_demo` **boot-loops on the BOX-3B** — its BSP
aborts when the touch controller isn't detected (see Gotcha #3), which also takes
down the display. Use a **minimal LCD-only app** instead.

### 5a. Project files (`~/esp/lcd_test`)

`main/idf_component.yml` — pin versions to match the BSP (critical, see Gotcha #5):
```yaml
dependencies:
  espressif/esp-box-3: "1.1.3"
  espressif/button: "3.5.0"
  lvgl/lvgl: "8.4.0"
  espressif/esp_codec_dev: "1.1.0"   # 1.5.x pulls new i2c_master -> conflict abort
  idf:
    version: ">=5.1"
```

`sdkconfig.defaults` — USB console + **octal** PSRAM (Gotcha #4):
```ini
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_ESP32S3_DATA_CACHE_64KB=y
```

`main/lcd_test_main.c` — bring up display via BSP, draw an LVGL label + spinner
(no touch init). See the working copy in `~/esp/lcd_test/main/`.

### 5b. BSP patch — make touch non-fatal (Gotcha #3)

After `idf.py set-target esp32s3` fetches the BSP into `managed_components/`, edit
`managed_components/espressif__esp-box-3/esp-box-3.c`:

In `bsp_display_indev_init()` — replace the aborting `BSP_ERROR_CHECK_RETURN_NULL(
bsp_touch_new(...))` so missing touch returns NULL instead of aborting:
```c
esp_err_t tret = bsp_touch_new(NULL, &tp);
if (tret != ESP_OK || tp == NULL) {
    ESP_LOGW(TAG, "bsp_touch_new failed (0x%x); skipping touch input", tret);
    return NULL;
}
```
In `bsp_display_start_with_config()` — let display continue if indev is NULL:
```c
disp_indev = bsp_display_indev_init(disp);
if (disp_indev == NULL) {
    ESP_LOGW(TAG, "Touch input unavailable; continuing display-only");
}
```
> ⚠️ Re-fetching/re-resolving dependencies **wipes this patch** — re-apply it after
> any `rm -rf managed_components` / dep change.

### 5c. Build, flash, observe

```bash
cd ~/esp/esp-idf && . ./export.sh
cd ~/esp/lcd_test
idf.py set-target esp32s3      # fetches BSP -> then apply patch 5b
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash monitor
```
Success = LCD shows the UI (blue screen, "LCD OK!" text, spinner) and the log loops
`I (...) lcd_test: alive N`.

---

## Gotchas reference (all hit during this setup)

| # | Symptom | Cause | Fix |
|---|---------|-------|-----|
| 1 | Board never enumerates (`/dev/cu.usbmodem*` missing) | Cabled through dock hub, or charge-only cable | Plug **directly into device** with a **data** cable |
| 2 | `idf.py` → "Python requirements not satisfied" | install.sh broke setuptools/ruamel in py3.9 env | `pip install "setuptools<81" "ruamel.yaml<0.18" "ruamel.yaml.clib==0.2.8"` |
| 3 | factory_demo boot-loops; black screen | `bsp_touch_new` aborts (touch not found on 3B) and kills display | Patch BSP touch init non-fatal (5b) |
| 4 | `PSRAM ID read error ... wrong line mode` abort | PSRAM left in quad mode | `CONFIG_SPIRAM_MODE_OCT=y` + `SPEED_80M` |
| 5 | `check_i2c_driver_conflict` abort at boot | `esp_codec_dev` 1.5.x pulls new i2c_master vs BSP's legacy i2c | Pin `esp_codec_dev: "1.1.0"` (+ button 3.5.0, lvgl 8.4.0) |
| 6 | esptool "No serial data received"; flash fails | USB-Serial-JTAG wedged after a crash-loop | Physically **unplug ~5s and replug** |

### USB-JTAG / serial notes (macOS, headless)
- `idf.py monitor` needs a real TTY; it refuses to run when scripted.
- macOS `cu` asserts DTR on open → straps GPIO0 → a crashing app drops to download
  mode (`boot:0x22 DOWNLOAD`). A clean board on a button-RESET runs the app fine.
- To boot the flashed app without re-strapping: `python -m esptool --chip esp32s3
  -p $PORT --after hard_reset run`.

## Onboard peripherals — empirically confirmed (I²C scan)

Verified on-device 2026-06-29 by scanning the BSP I²C bus (port 1, SDA=GPIO8,
SCL=GPIO18). Four devices ACK:

| I²C addr | Device | Status |
|---------|--------|--------|
| **0x68** | **IMU — ICM-4267x family (6-axis accel+gyro)** | ✅ confirmed; reads live accel/gyro. At rest ≈ accel (−0.14, 0.00, −2.01)g |
| **0x5D** | **GT911 capacitive touch** | ✅ present on bus (this is the controller factory_demo failed to find) |
| 0x40 | ES7210 mic ADC (4-ch) | ACK |
| 0x18 | ES8311 speaker DAC | ACK |

**IMU note:** WHO_AM_I reads **0x60**, not the `0x67` the generic `icm42670`
driver expects — it's a compatible variant (BSP calls it ICM-42607-P). The driver
configures and reads fine regardless. BSP advertises `BSP_CAPS_IMU 1`; usage:
`icm42670_handle_t imu = icm42670_create(BSP_I2C_NUM, ICM42670_I2C_ADDRESS /*0x68*/);`
(needs `REQUIRES icm42670 driver` in the component's CMakeLists). A working
I²C-scan + IMU probe app is in `~/esp/lcd_test/main/` git history.

### Touch — ✅ WORKING (resolved)
GT911 touch is fully functional on the BOX-3B. Two issues had to be fixed:

**1. The real blocker — `scl_speed_hz` in legacy I²C LCD driver.** BSP 1.1.3's
`bsp_touch_new()` probes 0x5D fine, but `esp_lcd_new_panel_io_i2c()` then fails:
`scl_speed_hz is not need to set in legacy i2c_lcd driver` → returns NOT_FOUND →
abort. The GT911/TT21100 config macros set `scl_speed_hz` (a NEW i2c_master
field) but the BSP uses the LEGACY i2c driver. **Fix** (in `esp-box-3.c`, right
before the `esp_lcd_new_panel_io_i2c` call in `bsp_touch_new`):
```c
tp_io_config.scl_speed_hz = 0;   // legacy i2c_lcd driver rejects this field
```

**2. Shared reset on GPIO48.** Touch RST is shared with the LCD (GPIO48,
active-high) and is only pulsed during *LCD* init (`esp_lcd_panel_reset`). A
touch-ONLY app (no display) leaves GPIO48 undriven, so the GT911 reads OK over
I²C but reports `point_num=0` forever. **Use the full path:** call
`bsp_display_start()` (LCD + touch together) — it resets the GT911 correctly.
Verified: an LVGL button visibly reacts to presses. INT=GPIO3 (strapping),
addr 0x5D. Pinout ref:
`mcpmarket-me/skills/esphome-box3-builder/references/box3-pinout.md`.

> Both patches live in the BSP under `managed_components/` and are wiped by any
> dependency re-resolve — re-apply after `rm -rf managed_components`.
