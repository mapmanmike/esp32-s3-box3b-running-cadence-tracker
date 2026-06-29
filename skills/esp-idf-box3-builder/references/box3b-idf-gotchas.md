# ESP32-S3-BOX-3B + ESP-IDF v5.3.1 — gotchas & fixes

Every issue below was hit and resolved bringing up a native ESP-IDF app
(LCD + touch + IMU via the `espressif/esp-box-3` BSP + LVGL 8.4) on the BOX-3B
from a clean Apple Silicon Mac. Quick table, then details.

| # | Symptom | Cause | Fix |
|---|---------|-------|-----|
| 1 | Board never enumerates (`/dev/cu.usbmodem*` absent) | Cabled through the dock/base USB hub, or charge-only cable | Plug a **data** cable **directly into the device** |
| 2 | `idf.py` → "Python requirements not satisfied" | `install.sh` upgrades setuptools≥81 (drops pkg_resources) + ruamel dist-info py3.9 can't resolve | `pip install "setuptools<81" "ruamel.yaml<0.18" "ruamel.yaml.clib==0.2.8"` in the IDF venv |
| 3 | factory_demo boot-loops / touch "not found" → abort | GT911 config macro sets `scl_speed_hz`, rejected by the legacy i2c_lcd driver | set `tp_io_config.scl_speed_hz = 0;` before `esp_lcd_new_panel_io_i2c` (Patch 1) |
| 4 | `PSRAM ID read error ... wrong line mode` abort | PSRAM left in quad mode | `CONFIG_SPIRAM_MODE_OCT=y` + `CONFIG_SPIRAM_SPEED_80M=y` |
| 5 | `check_i2c_driver_conflict` abort at boot | `esp_codec_dev` 1.5.x pulls the new i2c_master driver vs the BSP's legacy i2c | pin `espressif/esp_codec_dev: "1.1.0"` |
| 6 | Touch reads OK over I²C but `point_num=0` forever | GT911 RST shared with LCD on GPIO48; only pulsed during LCD init | use full `bsp_display_start()` (display+touch), not a touch-only path |
| 7 | Colors wrong — near-black bg shows as **purple**, grainy | SPI panel expects byte-swapped RGB565 | `CONFIG_LV_COLOR_16_SWAP=y` |
| 8 | esptool "No serial data received"; flash fails | USB-Serial-JTAG wedged after an app crash-loop | physically **unplug ~5s and replug** |

## Details

### 1. Connection — device, not dock
The BOX-3 base/dock contains a USB hub; the ESP32-S3 native USB does not reliably
enumerate through it (you'll see a Realtek hub chain and **no** Espressif VID
`0x303A`). Plug a known-data USB-C cable straight into the device. Verify:
`ioreg -p IOUSB -l -w0 | grep -i "JTAG\|303a"` → "USB JTAG_serial debug unit".

### 2. Broken IDF Python env (py3.9)
`install.sh` exits 0 but `idf.py` then errors "requirements not satisfied". Two
root causes in `~/.espressif/python_env/idf5.3_py3.9_env`: setuptools ≥81 removed
`pkg_resources`, and ruamel.yaml 0.19 / clib 0.2.15 ship dist-info names py3.9's
`importlib.metadata` can't resolve. Fix:
```bash
PYENV=~/.espressif/python_env/idf5.3_py3.9_env/bin/python
$PYENV -m pip install "setuptools<81" "ruamel.yaml<0.18" "ruamel.yaml.clib==0.2.8"
```

### 3 & 6. Touch (GT911) — the two real blockers
The GT911 **is present at I²C 0x5D** (confirmed by live bus scan). Two issues, not
addressing:
- **Patch 1 (`scl_speed_hz`)** — in `esp-box-3.c` `bsp_touch_new`, the GT911/
  TT21100 config macros set `scl_speed_hz` (a *new* i2c_master field). The BSP
  uses the *legacy* i2c_lcd driver, so `esp_lcd_new_panel_io_i2c` errors
  "scl_speed_hz is not need to set in legacy i2c_lcd driver" → NOT_FOUND → abort.
  Zero it right before the call:
  ```c
  tp_io_config.scl_speed_hz = 0;
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(..., &tp_io_config, &tp_io_handle), TAG, "");
  ```
- **Shared reset on GPIO48** — touch RST is shared with the LCD and only pulsed
  during LCD init. Bring up touch via the full `bsp_display_start()` path; a
  touch-only app leaves GPIO48 undriven and the GT911 reads OK but reports 0
  points.

Also make touch non-fatal so the LCD survives a touch failure (the BSP error
macros are `ESP_ERROR_CHECK`/`assert`):
```c
// in bsp_display_indev_init(), replacing BSP_ERROR_CHECK_RETURN_NULL(bsp_touch_new(...)):
esp_err_t tret = bsp_touch_new(NULL, &tp);
if (tret != ESP_OK || tp == NULL) {
    ESP_LOGW(TAG, "bsp_touch_new failed (0x%x); skipping touch input", tret);
    return NULL;
}
```
> Re-fetching dependencies wipes `managed_components/` — re-apply both patches
> after any `set-target`/`reconfigure`. The cadence firmware ships
> `patches/apply_bsp_patches.py` to do this idempotently.

### 4. Octal PSRAM
BOX-3/3B has 16MB **octal** PSRAM. Default quad config aborts with
`PSRAM ID read error ... wrong PSRAM line mode`. Set
`CONFIG_SPIRAM_MODE_OCT=y`, `CONFIG_SPIRAM_SPEED_80M=y`.

### 5. I²C driver conflict
ESP-IDF forbids mixing the legacy (`driver/i2c.h`) and new
(`driver/i2c_master.h`) I²C drivers — it aborts at startup
(`check_i2c_driver_conflict`). The esp-box-3 BSP uses legacy i2c; `esp_codec_dev`
≥1.5 pulls the new one. Pin `espressif/esp_codec_dev: "1.1.0"` (and keep
button 3.5.0 / lvgl 8.4.0 to match BSP 1.1.3).

### 7. Color byte-swap
The SPI LCD expects byte-swapped RGB565. Without `CONFIG_LV_COLOR_16_SWAP=y`,
`0x101012` (near-black) renders as purple and gradients look grainy. The
known-good factory_demo sets this; match it.

### 8. USB-Serial-JTAG wedge + headless serial notes
A crash-looping app wedges the native USB-Serial-JTAG: esptool reports
"No serial data received" and flashing fails even though `/dev/cu.usbmodem*`
still exists. Only a physical unplug/replug clears it.

Headless/scripted serial caveats (macOS):
- `idf.py monitor` refuses to run without an interactive TTY.
- macOS `cu` asserts DTR on open → straps GPIO0 → a crashing app reboots into
  download mode. To read the app's log without re-strapping, `os.open` the port
  with termios `CLOCAL` set and do **not** toggle DTR/RTS.
- To boot the flashed app cleanly: `esptool --chip esp32s3 -p PORT --after hard_reset run`.

## Confirmed peripheral map (live I²C scan, BSP I²C bus, SDA=8 SCL=18)
| I²C addr | Device |
|---------|--------|
| 0x18 | ES8311 speaker DAC |
| 0x40 | ES7210 mic ADC (4-ch) |
| 0x5D | GT911 capacitive touch |
| 0x68 | ICM-4267x IMU (6-axis; WHO_AM_I reads 0x60 on the 3B) |
