# Cadence Meter — firmware

ESP-IDF v5.3.x app for the **ESP32-S3-BOX-3B**. Reads the onboard IMU
(ICM-4267x @ I²C 0x68) to detect treadmill foot-strike impacts, computes a live
rolling cadence (steps/min), and shows START → live number → STOP → session
summary (avg cadence + line graph) on the LCD via LVGL. Peloton-styled theme.

## Prerequisites
- ESP-IDF **v5.3.x** installed (`. ~/esp/esp-idf/export.sh` in your shell).
- ESP32-S3-BOX-3B connected by a **data** USB-C cable, **directly to the device**
  (not through the dock/base hub). It enumerates as `/dev/cu.usbmodem*` (macOS).

See [`../docs/SETUP_GUIDE.md`](../docs/SETUP_GUIDE.md) for full toolchain setup and
the BOX-3B gotchas this project works around.

## Build, patch, flash

```bash
. ~/esp/esp-idf/export.sh
cd firmware

# 1) Fetch components (pulls the esp-box-3 BSP into managed_components/)
idf.py set-target esp32s3

# 2) Apply the two BOX-3B BSP fixes (idempotent; re-run after any dep re-resolve)
python3 patches/apply_bsp_patches.py

# 3) Build + flash + monitor
idf.py build
idf.py -p /dev/cu.usbmodem101 flash monitor   # exit monitor: Ctrl+]
```

> **Why the patch step?** The pinned `espressif/esp-box-3` 1.1.3 BSP needs two
> fixes for the 3B: (a) zero `scl_speed_hz` so the **legacy** I²C LCD driver
> accepts the touch panel-IO config, and (b) make touch init non-fatal. Both are
> applied to `managed_components/` by `apply_bsp_patches.py`. The component
> manager re-downloads the BSP on a clean checkout / dep change, which wipes the
> edits — so always run the patch script after `set-target`/`reconfigure`.

## Configuration
Key dependency pins (`main/idf_component.yml`) — chosen to match the known-good
factory_demo set and avoid driver conflicts:
- `espressif/esp-box-3: 1.1.3`, `lvgl/lvgl: 8.4.0`, `espressif/button: 3.5.0`
- `espressif/esp_codec_dev: 1.1.0` (newer 1.5.x pulls the new i2c_master driver
  and aborts with `check_i2c_driver_conflict`).

Key sdkconfig (`sdkconfig.defaults`): octal PSRAM, USB-Serial-JTAG console,
Montserrat 20/28/48 fonts, and **`CONFIG_LV_COLOR_16_SWAP=y`** (the SPI panel
expects byte-swapped RGB565 — without it, colors render wrong/purple).

## Tuning step detection
Constants at the top of `main/cadence_main.c` (watch the `STEP # d=..g` serial log
to tune against your treadmill):

| Define | Default | Meaning |
|---|---|---|
| `THRESHOLD_G` | 0.35 | impact magnitude (g, above baseline) to count a step |
| `RELEASE_G` | 0.20 | hysteresis: must fall below before next count |
| `MIN_STEP_MS` | 200 | refractory period (max ~300 spm); debounces one bang |
| `WINDOW_MS` | 10000 | rolling window for the live cadence average |
| `SESSION_SAMPLE_MS` | 30000 | how often cadence is logged to the summary graph |

The graph buffer (`MAX_PTS=120`) auto-downsamples for sessions longer than its
span, so a ~1hr+ run stays bounded in memory and still graphs end-to-end.
