// Stage 6 typing test. Prints every key event and the text typed so far. GUI plus Escape returns to the loader.
#include <stdio.h>
#include <string.h>
#include "a11_keys.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

void app_main(void) {
    QueueHandle_t q = xQueueCreate(32, sizeof(a11_event_t));
    a11_keys_start(q);
    bool shift = false, caps = false, gui = false;
    char text[120] = "";
    printf("KEYTEST ready, type on the A-11. GUI plus Escape returns to the loader.\n");
    for (;;) {
        a11_event_t ev;
        if (xQueueReceive(q, &ev, portMAX_DELAY) != pdTRUE) continue;
        printf("KEY %s %s (0x%03X)\n", ev.down ? "down" : "up  ", a11_key_name(ev.code), ev.code);
        if (ev.code == A11_HID_LEFT_SHIFT) shift = ev.down;
        if (ev.code == A11_HID_LEFT_GUI) gui = ev.down;
        if (!ev.down) continue;
        if (ev.code == A11_HID_CAPS_LOCK) { caps = !caps; printf("CAPS %s\n", caps ? "on" : "off"); }
        if (gui && ev.code == A11_HID_ESCAPE) {
            printf("returning to the loader\n");
            esp_ota_set_boot_partition(esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL));
            esp_restart();
        }
        char c = a11_hid_to_char(ev.code, shift, caps);
        size_t len = strlen(text);
        if (c == '\n') { printf("LINE %s\n", text); text[0] = 0; }
        else if (c == '\b') { if (len) text[len - 1] = 0; printf("TEXT %s\n", text); }
        else if (c && c != '\x1b' && len < sizeof text - 1) { text[len] = c; text[len + 1] = 0; printf("TEXT %s\n", text); }
    }
}
