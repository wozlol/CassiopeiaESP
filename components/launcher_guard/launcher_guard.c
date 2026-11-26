#include "launcher_guard.h"
#include <stdlib.h>
#include <string.h>
#include "a11_keys.h"
#include "display.h"
#include "status_led.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "guard";
#define MAGIC 0x47554152UL
static RTC_NOINIT_ATTR struct { uint32_t magic, tries; } g;
static bool cold_start, deep_wake;   // how this run began
#define MOD_SHIFT 1
#define MOD_CTRL 2
#define MOD_ALT 4
#define MOD_GUI 8

void launcher_guard_go_loader(void) {
    const esp_partition_t *f = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (f) esp_ota_set_boot_partition(f);
    esp_restart();
}

// ---- The trip back to the loader. The moment GUI and Esc are both down the screen fills with stars in the theme colours: mostly dots, some
// bigger stars and the odd planet. They all drift one way, picked at random each time, and fade a little at every step. It is a few quick
// frames, then the loader starts. Letting go before the end puts everything back.
#define STEPS 8
#define STEP_US 100000LL
#define HOLD_US (STEPS * STEP_US)
static uint16_t hue565(int h, int v) {   // h 0 to 359, v 0 to 255, full colour, used for the back light
    h %= 360;
    int region = h / 60, f = (h % 60) * 255 / 60;
    int p = 0, q = v * (255 - f) / 255, t = v * f / 255, r, g, b;
    switch (region) { case 0: r = v; g = t; b = p; break; case 1: r = q; g = v; b = p; break; case 2: r = p; g = v; b = t; break;
                      case 3: r = p; g = q; b = v; break; case 4: r = t; g = p; b = v; break; default: r = v; g = p; b = q; break; }
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static uint16_t mix565(uint16_t bg, uint16_t fg, int k) {   // k 0 is the background colour, 255 is the text colour
    int r = (((bg >> 11) & 31) * (255 - k) + ((fg >> 11) & 31) * k) / 255;
    int g = (((bg >> 5) & 63) * (255 - k) + ((fg >> 5) & 63) * k) / 255;
    int b = ((bg & 31) * (255 - k) + (fg & 31) * k) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}
typedef struct { int x, y, speed; char ch; } star_t;
static char star_glyph(int *speed) {
    static const char dots[] = ".......,'`", mid[] = ":;-+", big[] = "*+ox", planet[] = "O@0";
    uint32_t r = esp_random() % 100;
    if (r < 66) { *speed = 1; return dots[esp_random() % (sizeof dots - 1)]; }
    if (r < 86) { *speed = 1; return mid[esp_random() % (sizeof mid - 1)]; }
    if (r < 96) { *speed = 2; return big[esp_random() % (sizeof big - 1)]; }
    *speed = 3; return planet[esp_random() % (sizeof planet - 1)];   // the closer ones move faster
}
// Runs while GUI and Esc are down. Returns when they are let go. After the last step it does not return, it restarts into the loader.
static void fx_run(void) {
    int cols = 0, rows = 0;
    display_get_grid(&cols, &rows);
    if (cols < 10 || rows < 6) { launcher_guard_go_loader(); return; }
    const terminal_cell_t *prev = NULL; int pc = 0, pr = 0;
    display_get_text_buffer(&prev, &pc, &pr);
    bool was_raw = display_is_raw();
    terminal_cell_t *fx = heap_caps_calloc((size_t)cols * rows, sizeof *fx, MALLOC_CAP_8BIT);
    int nstars = cols * rows / 12;
    star_t *st = heap_caps_calloc((size_t)nstars, sizeof *st, MALLOC_CAP_8BIT);
    if (!fx || !st) { free(fx); free(st); launcher_guard_go_loader(); return; }
    const display_theme_t *th = display_theme();
    static const int DX[8] = {1, -1, 0, 0, 1, 1, -1, -1}, DY[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    int d = (int)(esp_random() % 8), dx = DX[d], dy = DY[d];   // one direction for every star, picked now
    for (int i = 0; i < nstars; i++) {
        st[i].x = (int)(esp_random() % (uint32_t)cols);
        st[i].y = (int)(esp_random() % (uint32_t)rows);
        st[i].ch = star_glyph(&st[i].speed);
    }
    if (was_raw) display_raw(false);
    display_set_text_buffer(fx, cols, rows);
    display_set_cursor(0, 0, CURSOR_NONE);
    int64_t t0 = esp_timer_get_time();
    int drawn = -1;
    for (;;) {
        int64_t el = esp_timer_get_time() - t0;
        if (!(a11_keys_down(A11_HID_LEFT_GUI) && a11_keys_down(A11_HID_ESCAPE))) break;
        int step = (int)(el / STEP_US);
        if (step >= STEPS) { ESP_LOGW(TAG, "GUI and Esc held, back to the loader"); launcher_guard_go_loader(); return; }
        if (step != drawn) {   // the first frame goes up at once, then one every step
            for (int i = 0; i < cols * rows; i++) fx[i] = (terminal_cell_t){' ', DISPLAY_DEFAULT_FG, DISPLAY_DEFAULT_BG, 0};
            int k = 255 * (STEPS - step) / STEPS;   // each step is a little dimmer
            uint16_t col = mix565(th->bg, th->fg, k);
            for (int i = 0; i < nstars; i++) {
                int x = ((st[i].x + dx * st[i].speed * step) % cols + cols) % cols;
                int y = ((st[i].y + dy * st[i].speed * step) % rows + rows) % rows;
                fx[y * cols + x] = (terminal_cell_t){(uint16_t)(unsigned char)st[i].ch, col, DISPLAY_DEFAULT_BG, 0};
            }
            drawn = step;
            uint16_t lc = hue565(step * 45, 255);
            status_led_override(true, (uint8_t)(((lc >> 11) & 31) << 3), (uint8_t)(((lc >> 5) & 63) << 2), (uint8_t)((lc & 31) << 3));
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    status_led_override(false, 0, 0, 0);   // let go in time, everything goes back
    if (prev) display_set_text_buffer(prev, pc, pr);
    free(fx); free(st);
}

static void guard_task(void *arg) {
    bool healthy = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30));
        int64_t now = esp_timer_get_time();
        if (!healthy && now > 20000000LL) { g.tries = 0; healthy = true; }   // it has been running for a while, not crashing
        // GUI held while the power comes on or the board is reset: straight to the loader
        static int held_polls;
        if (cold_start && now > 400000 && now < 4000000) {
            if (a11_keys_down(A11_HID_LEFT_GUI)) { if (++held_polls >= 14) { ESP_LOGW(TAG, "GUI held at start, back to the loader"); launcher_guard_go_loader(); } }
            else held_polls = 0;
        }
        // GUI held with On to wake from Off: the loader too, unless the secret unlock press is GUI alone
        static bool wake_checked;
        if (deep_wake && !wake_checked && now > 600000) {
            wake_checked = true;
            uint16_t codes[16];
            int n = a11_keys_wake_held(codes, 16), m = 0;
            for (int i = 0; i < n; i++) m |= codes[i] == A11_HID_LEFT_SHIFT ? MOD_SHIFT : codes[i] == A11_HID_LEFT_CTRL ? MOD_CTRL : codes[i] == 226 ? MOD_ALT : codes[i] == A11_HID_LEFT_GUI ? MOD_GUI : 0;
            int secret = settings_get("wakemask", MOD_CTRL | MOD_GUI);
            if (n >= 0 && m == MOD_GUI && secret != MOD_GUI) { ESP_LOGW(TAG, "GUI held with On, back to the loader"); launcher_guard_go_loader(); }
        }
        if (a11_keys_down(A11_HID_LEFT_GUI) && a11_keys_down(A11_HID_ESCAPE)) fx_run();
    }
}

void launcher_guard_start(void) {
    esp_reset_reason_t r = esp_reset_reason();
    bool crash = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
    if (g.magic != MAGIC) { g.magic = MAGIC; g.tries = 0; }
    else if (crash) g.tries++;
    else g.tries = 0;
    deep_wake = r == ESP_RST_DEEPSLEEP;
    cold_start = !crash && !deep_wake;
    ESP_LOGI(TAG, "reset reason %d, crashes in a row %u", (int)r, (unsigned)g.tries);
    if (g.tries >= 3) {
        ESP_LOGE(TAG, "this mode crashed three times in a row, starting the loader");
        g.tries = 0;
        launcher_guard_go_loader();
    }
    xTaskCreate(guard_task, "guard", 4096, NULL, 6, NULL);
}
