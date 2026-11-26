idf_component_register(SRCS "ssh_client.c" "ssh_txbuf.c" INCLUDE_DIRS "include" REQUIRES libssh2_esp lwip mbedtls freertos esp_wifi esp_timer)
