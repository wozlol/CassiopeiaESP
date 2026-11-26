// Mode 1 shell. A terminal that shows the tmux sessions of the paired computer. Without a session it echoes locally.
#include <stdio.h>
#include <string.h>
#include "a11_keys.h"
#include "touch.h"
#include "launcher_guard.h"
#include "status_led.h"
#include "display.h"
#include "sessions.h"
#include "ui.h"
#include "vterm.h"
#include "vtkeys.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"

static void session_send(const char *data, size_t len) {
    if (sess_send(data, len)) return;
    // No session, echo locally. A carriage return becomes a new line and prompt.
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\r') vterm_write("\r\n$ ", 4);
        else if (data[i] == 0x7f || data[i] == '\b') vterm_write("\b \b", 3);
        else vterm_write(&data[i], 1);
    }
}

static void terminal_start(int cols, int rows) {
    vterm_init(cols, rows);
    sess_state_t st = sess_state();
    if (st == SESS_CONNECTING || st == SESS_ACTIVE) return;   // the computer paints this screen
    char b[220];
    snprintf(b, sizeof b, "\x1b[2J\x1b[H\x1b[1mCassiopeiaESP\x1b[0m mode1  %dx%d\r\nGUI menu, then Wi-Fi, Computer and Sessions.\r\nCtrl+GUI clipboard, Ctrl - and Ctrl = font, Alt+H history.\r\n$ ", cols, rows);
    vterm_write(b, strlen(b));
}

void app_main(void) {
    launcher_guard_start();
    display_init();
    ui_callbacks_t cbs = {.send = session_send, .terminal_reset = terminal_start};
    int cols, rows;
    ui_init(&cbs, &cols, &rows);
    terminal_start(cols, rows);
    QueueHandle_t q = xQueueCreate(32, sizeof(a11_event_t));
    a11_keys_start(q);   // before ui_ready, it reads which keys woke the device from Off
    ui_ready();
    status_led_start();
    QueueHandle_t tq = xQueueCreate(32, sizeof(touch_event_t));
    touch_start(tq);
    int64_t last_tick = 0;
    esp_task_wdt_add(NULL);   // if this loop ever stops for 6 seconds the device prints where it was and restarts
    for (;;) {
        esp_task_wdt_reset();
        touch_event_t te;
        while (xQueueReceive(tq, &te, 0) == pdTRUE) ui_touch(te);
        a11_event_t ev;
        bool got = xQueueReceive(q, &ev, pdMS_TO_TICKS(20)) == pdTRUE;
        if (!got || esp_timer_get_time() - last_tick > 100000) { ui_tick(); last_tick = esp_timer_get_time(); }
        if (!got) continue;
        if (ui_handle(&ev) || !ev.down) continue;
        vtkey_t vk = vtkeys_from_hid(ev.code);
        if (vk != VTKEY_NONE) {
            uint8_t buf[VTKEYS_MAX_LEN];
            size_t n = vtkeys_encode(vk, (ui_shift() ? VTMOD_SHIFT : 0) | (ui_ctrl() ? VTMOD_CTRL : 0), vterm_app_cursor_keys(), buf, sizeof buf);
            if (n) { status_led_user_key(false); session_send((const char *)buf, n); }
            continue;
        }
        char c = a11_hid_to_char(ev.code, ui_shift(), ui_caps());
        if (ui_ctrl() && c >= 'a' && c <= 'z') c -= 96;
        if (c == '\n') c = '\r';
        if (c == '\b') c = 0x7f;
        if (c) { status_led_user_key(c == '\r'); session_send(&c, 1); }
    }
}
