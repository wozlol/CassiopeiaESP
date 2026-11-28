// The Library program. No Wi-Fi, so all of the fast RAM is for reading, pictures and sound.
// A short list: Reader, Media, Files, and a way back to the terminal (Mode 1), which restarts the handheld into it.
#include <stdio.h>
#include <string.h>
#include "a11_keys.h"
#include "app_files.h"
#include "app_media.h"
#include "app_reader.h"
#include "app_status.h"
#include "app_switch.h"
#include "board.h"
#include "clipboard.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "launcher_guard.h"
#include "sdcard.h"
#include "status_led.h"
#include "settings.h"
#include "themes.h"
#include "touch.h"

static const char *TAG = "library";
static const int BRIGHT[] = {10, 25, 40, 60, 80, 100};
static const char *const ITEMS[] = {"Reader", "Media", "Files", "Back to the terminal"};
#define N_ITEMS 4
static terminal_cell_t *scr;
static int cols, rows, sel, bright_idx;
static char note[120];
static bool asleep;

static void draw(void) {
    for (int i = 0; i < cols * rows; i++) scr[i] = (terminal_cell_t){' ', DISPLAY_DEFAULT_FG, DISPLAY_DEFAULT_BG, 0};
    char title[48];
    snprintf(title, sizeof title, "Library  v%s", esp_app_get_description()->version);
    for (int c = 0; c < cols; c++) scr[c].attrs = ATTR_REVERSE;
    for (int c = 0; title[c]; c++) { scr[c + 1].cp = (unsigned char)title[c]; scr[c + 1].attrs = ATTR_REVERSE | ATTR_BOLD; }
    char bt[24];
    app_battery_text(bt, sizeof bt);
    for (int c = 0; bt[c]; c++) { int x = cols - (int)strlen(bt) - 2 + c; if (x > 9) { scr[x].cp = (unsigned char)bt[c]; scr[x].attrs = ATTR_REVERSE; } }
    for (int i = 0; i < N_ITEMS; i++) {
        int r = 2 + i;
        char b[60];
        snprintf(b, sizeof b, " %s", ITEMS[i]);
        for (int c = 0; b[c] && c < cols - 1; c++) { scr[r * cols + c].cp = (unsigned char)b[c]; scr[r * cols + c].attrs = i == sel ? ATTR_REVERSE : 0; }
        if (i == sel) for (int c = 0; c < cols; c++) scr[r * cols + c].attrs |= ATTR_REVERSE;
    }
    if (note[0]) for (int c = 0; note[c] && c < cols - 2; c++) scr[(2 + N_ITEMS + 1) * cols + 1 + c].cp = (unsigned char)note[c];
    const char *hint = " Up Down move   Enter open   Esc back to the terminal";
    int r = rows - 1;
    for (int c = 0; c < cols; c++) scr[r * cols + c].attrs = ATTR_REVERSE;
    for (int c = 0; hint[c] && c < cols - 1; c++) { scr[r * cols + c].cp = (unsigned char)hint[c]; scr[r * cols + c].attrs = ATTR_REVERSE; }
    display_set_cursor(0, 0, CURSOR_NONE);
}

// Off. The back light goes dark first, it keeps its last color through sleep otherwise. If deep sleep cannot be arranged, light sleep until On.
static void sleep_now(void) {
    status_led_off(true);
    display_sleep(true);
    if (a11_keys_prepare_sleep()) { touch_sleep(); display_power_off(); a11_keys_deep_sleep(); }
    asleep = true;
}

static void to_terminal(void) {
    if (!app_switch("mode1")) snprintf(note, sizeof note, "The terminal program is not installed. Install mode1.bin from the loader.");
}

void app_main(void) {
    launcher_guard_start();
    display_init();
    settings_init();
    int font_idx = settings_get("font", 0), theme_idx = settings_get("theme", 0);
    bright_idx = settings_get("bl", 5);
    if (bright_idx < 0 || bright_idx > 5) bright_idx = 5;
    display_set_theme(themes_get(theme_idx % themes_count()));
    display_set_backlight(BRIGHT[bright_idx]);
    display_set_font(font_idx, &cols, &rows);
    scr = heap_caps_calloc((size_t)cols * rows, sizeof *scr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    display_set_text_buffer(scr, cols, rows);
    esp_vfs_littlefs_conf_t lfs = {.base_path = "/store", .partition_label = "storage", .format_if_mount_failed = true, .dont_mount = false};
    if (esp_vfs_littlefs_register(&lfs) == ESP_OK) clipboard_init();
    sdcard_mount();
    QueueHandle_t q = xQueueCreate(32, sizeof(a11_event_t));
    a11_keys_start(q);
    QueueHandle_t tq = xQueueCreate(32, sizeof(touch_event_t));
    touch_start(tq);
    status_led_start();   // the same battery glow as the other programs, and it goes dark with Off
    status_led_set_brightness(BRIGHT[bright_idx]);
    ESP_LOGI(TAG, "library ready");
    draw();
    for (;;) {
        a11_event_t ev;
        touch_event_t te;
        bool changed = false;
        while (xQueueReceive(tq, &te, 0) == pdTRUE) {
            if (te.type == TOUCH_UP) { int cw, ch; display_cell_size(&cw, &ch); int r = te.y / ch - 2; if (r >= 0 && r < N_ITEMS) { sel = r; ev.code = 40; ev.down = true; xQueueSendToFront(q, &ev, 0); } }
        }
        if (xQueueReceive(q, &ev, pdMS_TO_TICKS(100)) != pdTRUE || !ev.down) continue;
        note[0] = 0;
        if (ev.code == A11_KEY_OFF) { sleep_now(); continue; }
        if (asleep) { if (ev.code == A11_KEY_ON) { asleep = false; display_sleep(false); status_led_off(false); changed = true; } else continue; }
        if (ev.code == A11_KEY_BACKLIGHT) { bright_idx = bright_idx == 0 ? 5 : bright_idx - 1; display_set_backlight(BRIGHT[bright_idx]); status_led_set_brightness(BRIGHT[bright_idx]); settings_set("bl", bright_idx); changed = true; }
        else if (ev.code == 82 && sel > 0) { sel--; changed = true; }
        else if (ev.code == 81 && sel < N_ITEMS - 1) { sel++; changed = true; }
        else if (ev.code == 41) to_terminal();
        else if (ev.code == 40) {
            if (sel == N_ITEMS - 1) to_terminal();
            else {
                app_exit_t r = sel == 0 ? reader_app(q, tq) : sel == 1 ? media_app(q, tq) : files_app(q, tq);
                // the list is back, with whatever text size the app left. The new buffer is put in use before the old one is freed.
                display_set_font(settings_get("font", font_idx), &cols, &rows);
                terminal_cell_t *fresh = heap_caps_calloc((size_t)cols * rows, sizeof *fresh, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                display_set_text_buffer(fresh, cols, rows);
                free(scr);
                scr = fresh;
                bright_idx = settings_get("bl", bright_idx);
                if (bright_idx < 0 || bright_idx > 5) bright_idx = 5;
                status_led_set_brightness(BRIGHT[bright_idx]);
                if (r == APP_SLEEP) { sleep_now(); continue; }
            }
            changed = true;
        }
        draw();
    }
}
