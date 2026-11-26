// Stage 7 first shell. A local terminal: the A-11 keyboard types into the vterm and the text shows on the screen.
// Backlight key steps brightness, Ctrl plus minus and equals change the font size, GUI plus Escape returns to the loader.
#include <stdio.h>
#include <string.h>
#include "a11_keys.h"
#include "display.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "vterm.h"
#include "vtkeys.h"

static void say(const char *s) { vterm_write(s, strlen(s)); }

static void start_terminal(void) {
    int cols, rows;
    display_get_grid(&cols, &rows);
    vterm_init(cols, rows);
    char b[160];
    snprintf(b, sizeof b, "\x1b[2J\x1b[H\x1b[1mCassiopeiaESP\x1b[0m shell0  %dx%d\r\nType here. Ctrl - and Ctrl = change the font.\r\n\x1b[31mred \x1b[32mgreen \x1b[34mblue \x1b[0m\x1b[4munderline\x1b[0m\r\n$ ", cols, rows);
    say(b);
}

void app_main(void) {
    display_init();
    start_terminal();
    QueueHandle_t q = xQueueCreate(32, sizeof(a11_event_t));
    a11_keys_start(q);
    bool shift = false, ctrl = false, gui = false, caps = false;
    int bl = 100;
    for (;;) {
        a11_event_t ev;
        if (xQueueReceive(q, &ev, portMAX_DELAY) != pdTRUE) continue;
        if (ev.code == A11_HID_LEFT_SHIFT) shift = ev.down;
        if (ev.code == A11_HID_LEFT_CTRL) ctrl = ev.down;
        if (ev.code == A11_HID_LEFT_GUI) gui = ev.down;
        if (!ev.down) continue;
        if (ev.code == A11_HID_CAPS_LOCK) { caps = !caps; continue; }
        if (gui && ev.code == A11_HID_ESCAPE) {
            esp_ota_set_boot_partition(esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL));
            esp_restart();
        }
        if (ev.code == A11_KEY_BACKLIGHT) { bl = bl <= 20 ? 100 : bl - 20; display_set_backlight(bl); continue; }
        if (ctrl && (ev.code == 45 || ev.code == 46)) {   // minus, equals
            int idx = display_font_index() + (ev.code == 46 ? 1 : -1);
            if (idx < 0 || idx >= display_font_count()) continue;
            int cols, rows;
            display_set_font(idx, &cols, &rows);
            start_terminal();
            continue;
        }
        vtkey_t vk = vtkeys_from_hid(ev.code);
        uint8_t buf[VTKEYS_MAX_LEN];
        if (vk != VTKEY_NONE) {
            size_t n = vtkeys_encode(vk, (shift ? VTMOD_SHIFT : 0) | (ctrl ? VTMOD_CTRL : 0), vterm_app_cursor_keys(), buf, sizeof buf);
            say("");   // keys with escape sequences go to the remote, there is none yet so show them
            (void)n;
            continue;
        }
        char c = a11_hid_to_char(ev.code, shift, caps);
        if (ctrl && c >= 'a' && c <= 'z') c -= 96;
        if (!c) continue;
        if (c == '\n') { vterm_write("\r\n$ ", 4); continue; }
        if (c == '\b') { vterm_write("\b \b", 3); continue; }
        vterm_write(&c, 1);
    }
}
