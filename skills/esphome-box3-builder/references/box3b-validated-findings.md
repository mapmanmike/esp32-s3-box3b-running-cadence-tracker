# BOX-3B — hardware-validated findings

Empirical results from bringing up an ESP32-S3-**BOX-3B** on real hardware (via
native ESP-IDF; the peripheral facts apply regardless of framework). Use these to
sanity-check addresses, pins, and known failure modes when authoring ESPHome
configs for the 3B variant.

## Confirmed peripheral map (live I²C scan)
Bus: SDA=GPIO8, SCL=GPIO18.

| I²C addr | Device | Notes |
|---------|--------|-------|
| 0x5D | **GT911 touch** | present & working; INT=GPIO3 (strapping), RST shared w/ LCD on GPIO48 |
| 0x68 | **ICM-4267x IMU** | 6-axis; live accel/gyro. WHO_AM_I reads **0x60** on the 3B (compatible variant, not the generic 0x67) |
| 0x40 | ES7210 mic ADC | 4-channel |
| 0x18 | ES8311 speaker DAC | needs MCLK (GPIO2) |

Matches `box3-pinout.md`; the IMU is confirmed I²C @ 0x68 (some refs list it as
SPI — on this unit it is on the shared I²C bus per the esp-box-3 BSP).

## Failure modes worth knowing (seen on the 3B)
- **USB doesn't enumerate** → cabled through the dock/base hub, or a charge-only
  cable. Plug a data cable **directly into the device**. Espressif VID is 0x303A.
- **Octal PSRAM** → the 3B has 16MB **octal** PSRAM; quad-mode config fails to
  init. In ESPHome ensure `psram:` is configured for octal/80MHz.
- **Display reset shared with touch on GPIO48** (active in BSP terms) → touch
  won't scan unless that reset is driven; keep display + touch reset consistent
  (`reset_pin: GPIO48, inverted: true` in ESPHome, per box3-pinout.md).
- **Color byte order** → the SPI panel wants byte-swapped RGB565; wrong setting
  shows a near-black background as purple/grainy.
- **USB-Serial-JTAG wedges after a crash-loop** → unplug ~5s and replug to flash.

## Native ESP-IDF path
A separate `esp-idf-box3-builder` skill (and a working cadence-meter firmware)
captures the full native ESP-IDF workflow + BSP patches for these same issues, if
you need lower-level control than ESPHome provides.
