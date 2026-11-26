// Stage 5 test image. Prints which slot it runs from, then boots back to the loader after 8 seconds.
#include <stdio.h>
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void) {
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    const esp_partition_t *p = esp_ota_get_running_partition();
    for (int i = 0; i < 4; i++) {
        printf("HELLO %s slot=%s flash=%luMB psram=%uKB\n", HELLO_NAME, p->label, (unsigned long)(flash >> 20),
               (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) >> 10));
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    const esp_partition_t *f = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    printf("HELLO %s returning to the loader\n", HELLO_NAME);
    esp_ota_set_boot_partition(f);
    esp_restart();
}
