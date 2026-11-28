#include <string.h>
#include "app_switch.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "app_status.h"
#include "status_led.h"
#include <stdio.h>

bool app_switch(const char *project_name) {
    for (int i = 0; i < 4; i++) {
        const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_0 + i), NULL);
        esp_app_desc_t d;
        if (!p || esp_ota_get_partition_description(p, &d) != ESP_OK) continue;
        if (strcmp(d.project_name, project_name) != 0) continue;
        if (esp_ota_set_boot_partition(p) != ESP_OK) return false;
        esp_restart();
    }
    return false;
}

void app_battery_text(char *out, size_t cap) {
    int mv, pct;
    if (status_led_battery(&mv, &pct)) snprintf(out, cap, "Batt %d%%%s", pct, status_led_charging() ? "+" : "");
    else if (cap) out[0] = 0;
}
