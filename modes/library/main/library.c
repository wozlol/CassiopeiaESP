// The Library program. No Wi-Fi, so all of the fast RAM is for reading, pictures and sound.
// It opens straight into the reader. The second row of the library list is Files, the whole card with pictures and sounds.
// The reader menu has the way back to the terminal (Mode 1), which restarts the handheld into it.
#include <stdio.h>
#include <string.h>
#include "a11_keys.h"
#include "app_files.h"
#include "app_media.h"
#include "app_reader.h"
#include "app_switch.h"
#include "board.h"
#include "clipboard.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "launcher_guard.h"
#include "sdcard.h"
#include "settings.h"
#include "status_led.h"
#include "themes.h"
#include "touch.h"

static const char *TAG = "library";
static const int BRIGHT[] = {10, 25, 40, 60, 80, 100};
static QueueHandle_t g_q, g_tq;
static int bright_idx;

// Files plays sounds with the same player the Library has always had.
static void play_sound(const char *path) { media_app_file(g_q, g_tq, path); }
// The Files row of the reader list. A text file chosen there comes back as a path for the reader to show.
static const char *run_files(void) {
    app_exit_t r = files_app(g_q, g_tq);
    if (r == APP_SLEEP) {   // Off was pressed inside Files, the sleep happens in the main loop after the reader notices
        a11_event_t ev = {.code = A11_KEY_OFF, .down = true};
        xQueueSendToFront(g_q, &ev, 0);
    }
    return files_take_doc();
}

// Off. The back light goes dark first, it keeps its last color through sleep otherwise. If deep sleep cannot be arranged, light sleep until On.
static void sleep_now(void) {
    status_led_off(true);
    display_sleep(true);
    if (a11_keys_prepare_sleep()) { touch_sleep(); display_power_off(); a11_keys_deep_sleep(); }
}
static void wait_on(QueueHandle_t q) {
    a11_event_t ev;
    while (xQueueReceive(q, &ev, portMAX_DELAY) == pdTRUE) if (ev.down && ev.code == A11_KEY_ON) break;
    display_sleep(false);
    status_led_off(false);
}

void app_main(void) {
    launcher_guard_start();
    display_init();
    settings_init();
    int font_idx = settings_get("font", 0), theme_idx = settings_get("theme", 0);
    bright_idx = settings_get("bl", 5);
    if (bright_idx < 0 || bright_idx > 5) bright_idx = 5;
    int cols, rows;
    display_set_theme(themes_get(theme_idx % themes_count()));
    display_set_backlight(BRIGHT[bright_idx]);
    display_set_font(font_idx, &cols, &rows);
    esp_vfs_littlefs_conf_t lfs = {.base_path = "/store", .partition_label = "storage", .format_if_mount_failed = true, .dont_mount = false};
    if (esp_vfs_littlefs_register(&lfs) == ESP_OK) clipboard_init();
    sdcard_mount();
    QueueHandle_t q = xQueueCreate(32, sizeof(a11_event_t));
    a11_keys_start(q);
    QueueHandle_t tq = xQueueCreate(32, sizeof(touch_event_t));
    touch_start(tq);
    g_q = q; g_tq = tq;
    files_set_sound_handler(play_sound);
    reader_set_files_handler(run_files);
    status_led_start();   // the same battery glow as the other programs, and it goes dark with Off
    status_led_set_brightness(BRIGHT[bright_idx]);
    ESP_LOGI(TAG, "library ready");
    for (;;) {   // the reader is the home of the Library. The only ways out are Off and the menu item that goes back to the terminal.
        app_exit_t r = reader_app(q, tq);
        bright_idx = settings_get("bl", bright_idx);
        if (bright_idx < 0 || bright_idx > 5) bright_idx = 5;
        status_led_set_brightness(BRIGHT[bright_idx]);
        if (r == APP_SLEEP) { sleep_now(); wait_on(q); continue; }
        if (!app_switch("mode1")) { ESP_LOGW(TAG, "the terminal program is not installed"); vTaskDelay(pdMS_TO_TICKS(1500)); }
    }
}
