idf_component_register(SRCS "syncd.c" INCLUDE_DIRS "include" REQUIRES ssh clipboard sdcard PRIV_REQUIRES app_update esp_partition esp_timer freertos)
