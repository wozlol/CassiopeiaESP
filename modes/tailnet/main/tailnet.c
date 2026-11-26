// Feasibility test only: does the MicroLink Tailscale client build with our ESP-IDF, and what does it cost in flash and RAM.
#include "esp_log.h"
#include "microlink.h"

void app_main(void) {
    microlink_config_t cfg = {.auth_key = "REPLACE_WITH_A_KEY", .device_name = "a11-test", .enable_derp = true, .enable_disco = true, .enable_stun = true, .max_peers = 8};
    microlink_t *ml = microlink_init(&cfg);
    ESP_LOGI("tailnet", "microlink_init returned %p", (void *)ml);
}
