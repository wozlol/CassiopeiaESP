idf_component_register(SRCS "display.c" INCLUDE_DIRS "include"
                       REQUIRES board a11_font
                       PRIV_REQUIRES esp_lcd esp_driver_spi esp_driver_ledc esp_driver_gpio freertos esp_timer esp_system)
