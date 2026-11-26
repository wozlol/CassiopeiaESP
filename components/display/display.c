// Text display for the A-11 on the ST7796 SPI panel. Each text row is composed into a small DMA buffer from the
// cell buffer and sent to the panel, only rows that changed are sent. No full frame buffer is needed.
#include "display.h"
#include <stdlib.h>
#include <string.h>
#include "a11_font.h"
#include "board.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define SENTINEL_FG DISPLAY_DEFAULT_FG   // must equal COLOR_DEFAULT_FG in components/tsm/src/color.h
#define SENTINEL_BG DISPLAY_DEFAULT_BG
#define W BOARD_LCD_WIDTH
#define H BOARD_LCD_HEIGHT
// One second period, on for 750 ms and off for 250 ms so the cursor is always easy to find. The cycle restarts on every move.
#define CURSOR_PERIOD_US 1000000
#define CURSOR_ON_US 750000

static const char *TAG = "display";
static const display_theme_t THEME_NEON = {"Neon", RGB565(0x39, 0xFF, 0x14), COLOR_BLACK, RGB565(0x39, 0xFF, 0x14)};

static display_theme_t theme;
static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t tx_tokens;   // two tokens, one per line buffer
static SemaphoreHandle_t lock;        // held while a pass draws, and by anything that changes geometry
static uint16_t *linebuf[2];
static int cur_buf;
static int font_idx, cw, ch, cols, rows;
static const terminal_cell_t *cells;
static terminal_cell_t *shadow;
static int cells_cols, cells_rows;
static volatile int cur_x, cur_y, cur_mode;
static int drawn_x = -1, drawn_y = -1, drawn_mode, drawn_mode_req;
static volatile bool all_dirty;
static int64_t blink_t0;
static bool sel_on, flat_bg, panel_asleep;
static int cursor_override = -1, last_backlight = 100;
static bool cursor_held;
static volatile uint32_t frame_count;
uint32_t display_frames(void) { return frame_count; }
static int sel_lo, sel_hi;   // reading order positions, row * cols + col

static inline uint16_t sw(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }

static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx) {
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(tx_tokens, &hp);
    return hp == pdTRUE;
}

esp_err_t display_set_backlight(uint8_t percent) {
    static bool ready = false;
    if (!ready) {
        ledc_timer_config_t t = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT, .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK};
        ledc_timer_config(&t);
        ledc_channel_config_t c = {.gpio_num = BOARD_LCD_BL, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0};
        ledc_channel_config(&c);
        ready = true;
    }
    if (percent > 100) percent = 100;
    if (percent) last_backlight = percent;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, percent * 255 / 100);
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

#define LEFT_PAD 1
// Scroll bar strip on the right edge, drawn over the last text column after the text. The app reserves that column.
#define SB_W 6
static volatile int sb_pos, sb_vis, sb_total;
static bool sb_drawn, strip_need;
static uint16_t *sbuf;
static void send(int x, int y, int w, int h, uint16_t *buf) {
    xSemaphoreTake(tx_tokens, portMAX_DELAY);
    esp_lcd_panel_draw_bitmap(panel, x, y, x + w, y + h, buf);
}

static uint16_t resolve_fg(const terminal_cell_t *c) { return c->fg_color == SENTINEL_FG ? theme.fg : c->fg_color; }
static uint16_t resolve_bg(const terminal_cell_t *c) { return (c->bg_color == SENTINEL_BG || flat_bg) ? theme.bg : c->bg_color; }

// Draws cells c0 up to c1 (exclusive) of text row r from row_cells, with the cursor in column cursor_col (or -1).
static void draw_region(int r, int c0, int c1, const terminal_cell_t *row_cells, int cursor_col, int cursor_mode) {
    bool sel_row = sel_on && r * cols + cols > sel_lo && r * cols <= sel_hi;
    uint16_t *buf = linebuf[cur_buf];
    xSemaphoreTake(tx_tokens, portMAX_DELAY);   // wait until this buffer is free, send() takes a token again
    xSemaphoreGive(tx_tokens);
    int stride = (c1 - c0) * cw;
    // Everything is drawn LEFT_PAD pixels in from the edge so letters never touch it. The first cell of a row also paints the pad pixel.
    int pad0 = c0 == 0 ? LEFT_PAD : 0;
    int x0 = c0 == 0 ? 0 : c0 * cw + LEFT_PAD;
    int tw = pad0 + stride;
    if (x0 + tw > W) tw = W - x0;   // the very last pixel column of the last cell falls off the panel
    for (int c = c0; c < c1; c++) {
        const terminal_cell_t *cell = &row_cells[c];
        uint16_t fg = resolve_fg(cell), bg = resolve_bg(cell);
        if (cell->attrs & ATTR_DIM) fg = (fg >> 1) & 0x7BEF;
        if (cell->attrs & ATTR_REVERSE) { uint16_t t = fg; fg = bg; bg = t; }
        if (sel_row && r * cols + c >= sel_lo && r * cols + c <= sel_hi) { uint16_t t = fg; fg = bg; bg = t; }
        bool block = cursor_mode == CURSOR_BLOCK && c == cursor_col;
        bool bar = cursor_mode == CURSOR_UNDERSCORE && c == cursor_col;
        if (block) { uint16_t t = fg; fg = bg; bg = t; if (cell->fg_color == SENTINEL_FG && !(cell->attrs & ATTR_REVERSE)) bg = theme.cursor; }
        if (cell->attrs & ATTR_INVISIBLE) fg = bg;
        const a11_font_t *f = a11_font_get(font_idx, cell->attrs & ATTR_BOLD);
        const uint8_t *g = a11_font_glyph(f, cell->cp ? cell->cp : ' ');
        uint16_t fgs = sw(fg), bgs = sw(bg);
        int rb = f->row_bytes;
        for (int y = 0; y < ch; y++) {
            unsigned bits = 0;
            if (g) bits = rb == 1 ? g[y] : (unsigned)(g[y * 2] << 8 | g[y * 2 + 1]);
            bool line = ((cell->attrs & ATTR_UNDERLINE) && y == ch - 1) || ((cell->attrs & ATTR_STRIKE) && y == ch / 2) ||
                        (bar && y >= ch - (ch / 4 > 2 ? ch / 4 : 2));
            uint16_t *px = buf + y * tw;
            if (pad0 && c == c0) px[0] = bgs;
            int base = pad0 + (c - c0) * cw;
            for (int x = 0; x < cw; x++) if (base + x < tw) px[base + x] = (line || ((bits >> (8 * rb - 1 - x)) & 1)) ? fgs : bgs;
        }
    }
    if (c1 == cols) strip_need = true;
    send(x0, r * ch, tw, ch, buf);
    cur_buf ^= 1;
    frame_count++;
}

static volatile bool raw_mode;
static uint16_t *rawbuf[2];
static int raw_i;
#define RAW_ROWS 16

static void wait_idle(void);
static void draw_strip(void) {
    if (!sbuf) sbuf = heap_caps_malloc((size_t)SB_W * H * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!sbuf) return;
    wait_idle();
    int total = sb_total, vis = sb_vis, pos = sb_pos;
    bool on = total > vis && vis > 0;
    uint16_t bg = sw(theme.bg), thumb = sw(theme.fg), track = sw((theme.fg >> 2) & 0x39E7);
    int th = on ? (H * vis) / total : 0;
    if (th < 14) th = 14;
    int ty = on && total > vis ? (int)((long)(H - th) * pos / (total - vis)) : 0;
    for (int y = 0; y < H; y++) {
        uint16_t *px = sbuf + y * SB_W;
        for (int x = 0; x < SB_W; x++) {
            uint16_t c = bg;
            if (on && x >= 1 && x <= 4) c = (y >= ty && y < ty + th) ? thumb : (x >= 2 && x <= 3 ? track : bg);
            px[x] = c;
        }
    }
    send(W - SB_W, 0, SB_W, H, sbuf);
    sb_drawn = true;
    strip_need = false;
}

void display_set_scrollbar(int pos, int visible, int total) {
    bool was_on = sb_total > sb_vis && sb_vis > 0;
    bool on = total > visible && visible > 0;
    int ty_old = sb_pos, v_old = sb_vis, t_old = sb_total;
    sb_pos = pos; sb_vis = visible; sb_total = total;
    if (on != was_on) { if (!on) all_dirty = true; else strip_need = true; }
    else if (on && (ty_old != pos || v_old != visible || t_old != total)) strip_need = true;
}
int display_scrollbar_width(void) { return SB_W; }

// Pictures and video: while raw mode is on the text grid is not drawn, the caller paints the whole screen with display_draw_pixels.
void display_raw(bool on) {
    xSemaphoreTake(lock, portMAX_DELAY);
    raw_mode = on;
    if (!on) all_dirty = true;   // the text comes back in full
    xSemaphoreGive(lock);
}

// Pixels are already in the panel's byte order, so a decoder can write them straight in. Copied in strips through two DMA buffers.
void display_draw_pixels(int x, int y, int w, int h, const uint16_t *px) {
    if (w <= 0 || h <= 0 || w > W) return;
    if (!rawbuf[0]) for (int i = 0; i < 2; i++) rawbuf[i] = heap_caps_malloc((size_t)W * RAW_ROWS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!rawbuf[0] || !rawbuf[1]) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int row = 0; row < h; row += RAW_ROWS) {
        int n = h - row < RAW_ROWS ? h - row : RAW_ROWS;
        xSemaphoreTake(tx_tokens, portMAX_DELAY);
        memcpy(rawbuf[raw_i], px + (size_t)row * w, (size_t)n * w * 2);
        esp_lcd_panel_draw_bitmap(panel, x, y + row, x + w, y + row + n, rawbuf[raw_i]);
        raw_i ^= 1;
    }
    xSemaphoreGive(lock);
}

static void render_task(void *arg) {
    esp_task_wdt_add(NULL);
    for (;;) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(16));
        xSemaphoreTake(lock, portMAX_DELAY);
        if (!panel_asleep && !raw_mode && cells && shadow && cells_cols == cols && cells_rows == rows) {
            int64_t now = esp_timer_get_time();
            int mode = cur_mode;
            if (cur_x != drawn_x || cur_y != drawn_y || mode != drawn_mode_req) blink_t0 = now;
            bool visible = ((now - blink_t0) % CURSOR_PERIOD_US) < CURSOR_ON_US;
            int eff = visible ? ((mode != CURSOR_NONE && cursor_override >= 0) ? cursor_override : mode) : CURSOR_NONE;
            int cx = cur_x, cy = cur_y;
            bool cursor_changed = (cx != drawn_x || cy != drawn_y || eff != drawn_mode);
            for (int r = 0; r < rows; r++) {
                terminal_cell_t *s = &shadow[r * cols];
                const terminal_cell_t *live = &cells[r * cols];
                int lo = cols, hi = -1;
                if (all_dirty) { lo = 0; hi = cols - 1; }
                else {
                    for (int c = 0; c < cols; c++)
                        if (memcmp(&s[c], &live[c], sizeof(terminal_cell_t)) != 0) { if (c < lo) lo = c; hi = c; }
                    if (cursor_changed) {
                        if (r == drawn_y && drawn_x >= 0 && drawn_x < cols) { if (drawn_x < lo) lo = drawn_x; if (drawn_x > hi) hi = drawn_x; }
                        if (r == cy && cx >= 0 && cx < cols) { if (cx < lo) lo = cx; if (cx > hi) hi = cx; }
                    }
                }
                if (hi < 0) continue;
                memcpy(&s[lo], &live[lo], (hi - lo + 1) * sizeof(terminal_cell_t));
                draw_region(r, lo, hi + 1, s, r == cy ? cx : -1, eff);
            }
            all_dirty = false;
            drawn_x = cx; drawn_y = cy; drawn_mode = eff; drawn_mode_req = mode;
            if (strip_need && sb_total > sb_vis && sb_vis > 0) draw_strip();
            strip_need = false;
        }
        xSemaphoreGive(lock);
    }
}

static void alloc_linebufs(void) {
    for (int i = 0; i < 2; i++) {
        free(linebuf[i]);
        linebuf[i] = heap_caps_malloc((size_t)W * ch * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }
}

static void wait_idle(void) {
    xSemaphoreTake(tx_tokens, portMAX_DELAY);
    xSemaphoreTake(tx_tokens, portMAX_DELAY);
    xSemaphoreGive(tx_tokens);
    xSemaphoreGive(tx_tokens);
}

void display_clear(void) {
    xSemaphoreTake(lock, portMAX_DELAY);   // the render task also uses these buffers, a theme change while it draws used to crash
    wait_idle();
    uint16_t *buf = linebuf[0];
    uint16_t bgs = sw(theme.bg);
    for (int i = 0; i < W * ch; i++) buf[i] = bgs;
    for (int y = 0; y < H; y += ch) {
        int h = (H - y) < ch ? (H - y) : ch;
        send(0, y, W, h, buf);
        wait_idle();
    }
    all_dirty = true;
    xSemaphoreGive(lock);
}

void display_set_font(int index, int *out_cols, int *out_rows) {
    if (index < 0 || index >= A11_FONT_SIZES) index = 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    cells = NULL;
    wait_idle();
    font_idx = index;
    cw = A11_FONTS[index * 2].w;
    ch = A11_FONTS[index * 2].h;
    cols = W / cw;
    rows = H / ch;
    alloc_linebufs();
    drawn_x = drawn_y = -1;
    xSemaphoreGive(lock);
    display_clear();
    if (out_cols) *out_cols = cols;
    if (out_rows) *out_rows = rows;
}

int display_font_count(void) { return A11_FONT_SIZES; }
int display_font_index(void) { return font_idx; }
void display_get_grid(int *c, int *r) { if (c) *c = cols; if (r) *r = rows; }
void display_cell_size(int *w, int *h) { if (w) *w = cw; if (h) *h = ch; }
const display_theme_t *display_theme(void) { return &theme; }

void display_set_theme(const display_theme_t *t) {
    xSemaphoreTake(lock, portMAX_DELAY);
    theme = *t;
    xSemaphoreGive(lock);
    display_clear();
}

void display_set_text_buffer(const terminal_cell_t *buf, int c, int r) {
    xSemaphoreTake(lock, portMAX_DELAY);
    cells = NULL;
    free(shadow);
    shadow = NULL;
    if (!buf || c <= 0 || r <= 0) { xSemaphoreGive(lock); return; }
    shadow = heap_caps_calloc((size_t)c * r, sizeof(terminal_cell_t), MALLOC_CAP_8BIT);
    cells_cols = c;
    cells_rows = r;
    all_dirty = true;
    cells = shadow ? buf : NULL;
    xSemaphoreGive(lock);
}

bool display_get_text_buffer(const terminal_cell_t **buf, int *c, int *r) {
    if (buf) *buf = cells;
    if (c) *c = cells_cols;
    if (r) *r = cells_rows;
    return cells != NULL;
}
bool display_is_raw(void) { return raw_mode; }

void display_set_cursor(int x, int y, cursor_mode_t mode) { if (cursor_held) return; cur_x = x; cur_y = y; cur_mode = mode; }
void display_hold_cursor(bool hold, int x, int y, cursor_mode_t mode) {
    cursor_held = hold;
    if (hold) { cur_x = x; cur_y = y; cur_mode = mode; }
}
void display_get_cursor(int *x, int *y) { if (x) *x = cur_x; if (y) *y = cur_y; }

void display_set_selection(bool active, int r0, int c0, int r1, int c1) {
    int a = r0 * cols + c0, b = r1 * cols + c1;
    xSemaphoreTake(lock, portMAX_DELAY);
    sel_on = active;
    sel_lo = a < b ? a : b;
    sel_hi = a < b ? b : a;
    all_dirty = true;
    xSemaphoreGive(lock);
}

void display_set_flat_backgrounds(bool flat) {
    xSemaphoreTake(lock, portMAX_DELAY);
    flat_bg = flat;
    all_dirty = true;
    xSemaphoreGive(lock);
}
void display_bell(void) {}

esp_err_t display_init(void) {
    gpio_deep_sleep_hold_dis();   // pins were held through deep sleep
    gpio_hold_dis(BOARD_LCD_BL);
    gpio_hold_dis(BOARD_LCD_CS);
    theme = THEME_NEON;
    lock = xSemaphoreCreateMutex();
    tx_tokens = xSemaphoreCreateCounting(2, 2);
    spi_bus_config_t bus = {.mosi_io_num = BOARD_LCD_MOSI, .miso_io_num = -1, .sclk_io_num = BOARD_LCD_SCLK, .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = W * 32 * 2};
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_LCD_HOST, &bus, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t ioc = {.dc_gpio_num = BOARD_LCD_DC, .cs_gpio_num = BOARD_LCD_CS, .pclk_hz = BOARD_LCD_PCLK_HZ, .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 4, .on_color_trans_done = on_color_done};
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_HOST, &ioc, &io));
    esp_lcd_panel_dev_config_t pc = {.reset_gpio_num = BOARD_LCD_RST, .rgb_ele_order = BOARD_LCD_BGR ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16};
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7796(io, &pc, &panel));
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, BOARD_LCD_INVERT);
    esp_lcd_panel_swap_xy(panel, BOARD_LCD_SWAP_XY);
    esp_lcd_panel_mirror(panel, BOARD_LCD_MIRROR_X, BOARD_LCD_MIRROR_Y);
    esp_lcd_panel_disp_on_off(panel, true);
    int c, r;
    display_set_font(0, &c, &r);
    blink_t0 = esp_timer_get_time();
    xTaskCreate(render_task, "display", 4096, NULL, 4, NULL);
    display_set_backlight(100);
    ESP_LOGI(TAG, "display up, %dx%d cells with a %dx%d font", c, r, cw, ch);
    return ESP_OK;
}

void display_set_cursor_override(int mode) { cursor_override = mode; }

// Last step before deep sleep, keeps the backlight off and the panel deselected while the ESP is off.
void display_power_off(void) {
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    gpio_hold_en(BOARD_LCD_BL);
    gpio_hold_en(BOARD_LCD_CS);
}

void display_sleep(bool sleep) {
    xSemaphoreTake(lock, portMAX_DELAY);
    panel_asleep = sleep;
    if (sleep) {
        display_set_backlight(0);
        esp_lcd_panel_disp_sleep(panel, true);
    } else {
        esp_lcd_panel_disp_sleep(panel, false);
        vTaskDelay(pdMS_TO_TICKS(130));   // the panel needs a moment after sleep out
        all_dirty = true;
    }
    xSemaphoreGive(lock);
    if (!sleep) display_set_backlight(last_backlight);
}
