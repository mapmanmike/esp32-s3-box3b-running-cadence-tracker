#!/usr/bin/env python3
"""
Apply the two ESP32-S3-BOX-3B fixes to the fetched espressif/esp-box-3 BSP.

The BSP is downloaded by the IDF component manager into
  managed_components/espressif__esp-box-3/esp-box-3.c
on the first `idf.py set-target` / `reconfigure`. Re-resolving dependencies
(e.g. deleting managed_components) wipes these edits, so re-run this script
afterwards. It is idempotent — safe to run repeatedly.

Fixes (both validated on real BOX-3B hardware):

1. Touch panel-IO creation fails on the legacy I2C LCD driver because the
   GT911/TT21100 config macros set `scl_speed_hz` (a NEW i2c_master field) that
   the legacy `esp_lcd_new_panel_io_i2c` rejects -> bsp_touch_new returns
   NOT_FOUND -> abort. We zero scl_speed_hz before the call.

2. Make touch init non-fatal so the LCD still comes up if touch ever fails to
   initialise (the BSP otherwise asserts/aborts and takes the display with it).

Usage:
  python3 patches/apply_bsp_patches.py [path-to-project-root]
Defaults to the current directory.
"""
import sys
import pathlib

root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
bsp = root / "managed_components" / "espressif__esp-box-3" / "esp-box-3.c"

if not bsp.exists():
    sys.exit(f"BSP not found at {bsp}\n"
             "Run `idf.py set-target esp32s3` (or reconfigure) first so the "
             "component manager fetches espressif/esp-box-3, then re-run this.")

src = bsp.read_text()
changed = False

# ---- Patch 1: zero scl_speed_hz before legacy panel-IO creation ----
PATCH1_ANCHOR = (
    "    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c("
    "(esp_lcd_i2c_bus_handle_t)BSP_I2C_NUM, &tp_io_config, &tp_io_handle), TAG, \"\");"
)
PATCH1_REPLACEMENT = (
    "    /* BOX-3B patch: legacy i2c_lcd driver rejects scl_speed_hz set by the\n"
    "     * GT911/TT21100 config macros; zero it so panel-IO creation succeeds. */\n"
    "    tp_io_config.scl_speed_hz = 0;\n"
    + PATCH1_ANCHOR
)
if "tp_io_config.scl_speed_hz = 0;" in src:
    print("Patch 1 (scl_speed_hz): already applied")
elif PATCH1_ANCHOR in src:
    src = src.replace(PATCH1_ANCHOR, PATCH1_REPLACEMENT, 1)
    changed = True
    print("Patch 1 (scl_speed_hz): applied")
else:
    print("Patch 1 (scl_speed_hz): WARNING anchor not found — BSP version differs?")

# ---- Patch 2: make touch init non-fatal in bsp_display_indev_init ----
PATCH2_ANCHOR = (
    "    BSP_ERROR_CHECK_RETURN_NULL(bsp_touch_new(NULL, &tp));\n"
    "    assert(tp);"
)
PATCH2_REPLACEMENT = (
    "    /* BOX-3B patch: do not abort if touch init fails; return NULL so the\n"
    "     * display still comes up (LCD-only fallback). */\n"
    "    esp_err_t tret = bsp_touch_new(NULL, &tp);\n"
    "    if (tret != ESP_OK || tp == NULL) {\n"
    "        ESP_LOGW(TAG, \"bsp_touch_new failed (0x%x); skipping touch input\", tret);\n"
    "        return NULL;\n"
    "    }"
)
if "skipping touch input" in src:
    print("Patch 2 (non-fatal touch): already applied")
elif PATCH2_ANCHOR in src:
    src = src.replace(PATCH2_ANCHOR, PATCH2_REPLACEMENT, 1)
    changed = True
    print("Patch 2 (non-fatal touch): applied")
else:
    print("Patch 2 (non-fatal touch): WARNING anchor not found — BSP version differs?")

if changed:
    bsp.write_text(src)
    print(f"Wrote {bsp}")
else:
    print("No changes needed.")
