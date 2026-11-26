idf_component_register(SRCS "touch.c" INCLUDE_DIRS "include" REQUIRES board freertos i2c_bus PRIV_REQUIRES esp_driver_i2c esp_driver_gpio esp_timer)
