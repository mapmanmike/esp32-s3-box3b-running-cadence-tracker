idf_component_register(SRCS "cadence_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp-box-3 icm42670 driver esp_timer nvs_flash)
