idf_component_register(SRCS "ui.c" "wrap.c"
                       INCLUDE_DIRS "include"
                       REQUIRES clipboard webadmin app_common tunnel sdcard prefs keystore display a11_keys vterm sessions net a11_id touch status_led
                       PRIV_REQUIRES esp_driver_usb_serial_jtag board nvs_flash esp_timer app_update esp_system esp_psram a11_font joltwallet__littlefs monocypher esp_hw_support net)
