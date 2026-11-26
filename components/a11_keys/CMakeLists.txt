idf_component_register(SRCS "a11_keys.c" INCLUDE_DIRS "include" REQUIRES nrf_swd freertos PRIV_REQUIRES esp_timer esp_driver_gpio esp_driver_i2c esp_hw_support i2c_bus)
