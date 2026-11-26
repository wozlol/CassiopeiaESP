idf_component_register(SRCS "sessions.c" INCLUDE_DIRS "include" REQUIRES ssh a11_id vterm display PRIV_REQUIRES syncd net esp_timer status_led)
