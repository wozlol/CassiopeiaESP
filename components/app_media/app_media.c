// Media viewer. Browses /sdcard/media, shows JPEG pictures full screen and plays WAV and MP3 through the board's speaker (ES8311).
// Same text sizes, themes and brightness key as Mode 1. Video was dropped. Convert files on the computer with tools/a11convert.py.
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "a11_keys.h"
#include "board.h"
#include "display.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "es8311_codec.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "app_imgview.h"
#include "app_status.h"
#include "app_media.h"
#include "freertos/queue.h"
#include "sdcard.h"
#include "settings.h"
#include "themes.h"
#include "status_led.h"
#include "touch.h"
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

static const char *TAG = "media";
#define ROOT SDCARD_MOUNT "/media"
#define MAX_ENT 400
#define MAX_PIC (6 * 1024 * 1024)
#define VOL_CAP 75
#define HID_ENTER 40
#define HID_ESC 41
#define HID_BACKSPACE 42
#define HID_SPACE 44
#define HID_MINUS 45
#define HID_EQUALS 46
#define HID_PGUP 75
#define HID_PGDN 78
#define HID_RIGHT 79
#define HID_LEFT 80
#define HID_DOWN 81
#define HID_UP 82
static const int BRIGHT[] = {10, 25, 40, 60, 80, 100};

typedef enum { ST_BROWSE, ST_PICTURE, ST_PLAY, ST_MSG, ST_MENU } state_t;
static state_t state = ST_BROWSE, msg_back = ST_BROWSE;
static char msg_text[160];

// ---------- screen ----------
static terminal_cell_t *scr;
static int cols, rows, font_idx, theme_idx, bright_idx;
#define TC (cols - 1)   // text columns, the last column belongs to the scroll bar
static bool shift, ctrl, alt, gui;

// Every screen is drawn into a hidden copy and then switched in with one copy, so the display never catches it half drawn.
// That half drawn state was the flashing. The cursor is also kept aside and applied once at the end.
static terminal_cell_t *back_buf;
static int pc_x, pc_y, pc_m;
static void pend_cursor(int x, int y, int m) { pc_x = x; pc_y = y; pc_m = m; }
#define display_set_cursor(x, y, m) pend_cursor((x), (y), (m))
// The selected row of a list scrolls sideways when it does not fit: a pause, then a slow crawl to the end, a pause, and again.
static bool marquee_on;
static int mq_id;
static int64_t mq_t0;
static int marquee_off(int id, int len, int width) {
    int over = len - width;
    if (over <= 0) return 0;
    int64_t now = esp_timer_get_time();
    if (id != mq_id) { mq_id = id; mq_t0 = now; }
    marquee_on = true;
    const int pause = 1200, step = 220;
    int64_t t = (now - mq_t0) / 1000 % (pause + (int64_t)over * step + pause);
    if (t < pause) return 0;
    int off = (int)((t - pause) / step);
    return off > over ? over : off;
}
static int list_gen;   // changes whenever a new list is shown, so the sideways scroll starts over
static void scr_alloc(void) {
    free(scr);
    free(back_buf);
    scr = heap_caps_calloc((size_t)cols * rows, sizeof *scr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    back_buf = heap_caps_calloc((size_t)cols * rows, sizeof *scr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    display_set_text_buffer(scr, cols, rows);
}
static void clear(void) {
    for (int i = 0; i < cols * rows; i++) scr[i] = (terminal_cell_t){' ', DISPLAY_DEFAULT_FG, DISPLAY_DEFAULT_BG, 0};
    display_set_cursor(0, 0, CURSOR_NONE);
}
static void put(int r, int c, const char *s, uint8_t attrs) {
    if (r < 0 || r >= rows) return;
    for (; *s && c < TC; s++, c++) { scr[r * cols + c].cp = (unsigned char)*s; scr[r * cols + c].attrs = attrs; }
}
static void rowattr(int r, uint8_t attrs) { for (int c = 0; c < cols; c++) scr[r * cols + c].attrs |= attrs; }
static int btop;   // first visible row of the list
static void fix_top(int sel, int *top, int vis, int n) {
    if (*top > n - vis) *top = n - vis;
    if (*top < 0) *top = 0;
    if (sel < *top) *top = sel;
    if (sel >= *top + vis) *top = sel - vis + 1;
}
static void header(const char *t) { put(0, 1, t, ATTR_BOLD); rowattr(0, ATTR_REVERSE); }

// Text that is longer than the screen wraps at spaces instead of running off the edge.
static int wrap_len(const char *p, int width) {   // how many characters fit on this row, breaking at a space when it must
    int n = (int)strlen(p);
    if (n <= width) return n;
    int k = width;
    while (k > 0 && p[k] != ' ') k--;
    return k > 0 ? k : width;
}
static int wrap_rows(const char *s, int width) {
    if (width < 8) width = 8;
    int rows_n = 0;
    for (const char *p = s; *p; ) {
        int n = wrap_len(p, width);
        rows_n++;
        p += n;
        while (*p == ' ') p++;
    }
    return rows_n ? rows_n : 1;
}
// Puts a paragraph at row r and column c, wrapped to the screen. Returns the next free row.
static int put_flow(int r, int c, const char *s, uint8_t attrs) {
    int width = TC - c - 1;
    if (width < 8) width = 8;
    for (const char *p = s; *p && r < rows; ) {
        int n = wrap_len(p, width);
        char line[160];
        snprintf(line, sizeof line, "%.*s", n < 150 ? n : 150, p);
        put(r++, c, line, attrs);
        p += n;
        while (*p == ' ') p++;
    }
    return r;
}
static int last_foot_n = 1;   // rows the bottom hint takes, up to 3
static int foot_n_for(const char *t) { int n = wrap_rows(t, TC - 2); return n > 3 ? 3 : n; }
static void footer(const char *t) {
    int n = foot_n_for(t);
    last_foot_n = n;
    int r0 = rows - n;
    for (int r = r0; r < rows; r++) rowattr(r, ATTR_REVERSE);
    int width = TC - 2, r = r0;
    for (const char *p = t; *p && r < rows; ) {
        int k = wrap_len(p, width);
        char line[160];
        snprintf(line, sizeof line, "%.*s", k < 150 ? k : 150, p);
        put(r++, 1, line, ATTR_REVERSE);
        p += k;
        while (*p == ' ') p++;
    }
}

static void apply_font(int idx) {
    if (idx < 0 || idx >= display_font_count()) return;
    font_idx = idx;
    display_set_font(idx, &cols, &rows);
    scr_alloc();
    settings_set("font", idx);
}
static void apply_theme(int idx) {
    theme_idx = (idx + themes_count()) % themes_count();
    display_set_theme(themes_get(theme_idx));
    settings_set("theme", theme_idx);
}
static void apply_brightness(int idx) {
    bright_idx = idx < 0 ? 5 : idx > 5 ? 0 : idx;
    display_set_backlight(BRIGHT[bright_idx]);
    status_led_set_brightness(BRIGHT[bright_idx]);   // the back light dims with the screen
    settings_set("bl", bright_idx);
}
static void msg_show(state_t back, const char *text) {
    snprintf(msg_text, sizeof msg_text, "%.150s", text);
    msg_back = back;
    state = ST_MSG;
}


// ---------- GUI menu ----------
static int menu_sel;
static state_t menu_back = ST_BROWSE;
static bool gui_tap;
static void to_loader(void);
static app_exit_t quit_app;   // set when the person leaves, the loop in media_app then returns
static void draw_menu(void) {
    display_set_scrollbar(0, 0, 0);
    clear();
    header("Menu");
    { char bt[24]; app_battery_text(bt, sizeof bt); if (bt[0]) put(0, TC - (int)strlen(bt) - 1, bt, ATTR_REVERSE); }
    int cw, ch;
    display_cell_size(&cw, &ch);
    char b[100];
    put(2, 1, "Back to the list", 0);
    snprintf(b, sizeof b, "Theme        < %s >", themes_get(theme_idx)->name); put(3, 1, b, 0);
    snprintf(b, sizeof b, "Text size    < %dx%d, %dx%d cells >", cw, ch, cols, rows); put(4, 1, b, 0);
    snprintf(b, sizeof b, "Brightness   < %d%% >", BRIGHT[bright_idx]); put(5, 1, b, 0);
    put(6, 1, "Close", 0);
    rowattr(2 + menu_sel, ATTR_REVERSE);
    footer(" Up Down move   Left Right change   Enter select   GUI closes");
}

// ---------- browser ----------
typedef struct { char name[64]; bool dir; int kind; long size; } ent_t;   // kind 1 picture, 2 sound
static ent_t *ents;
static int nents, bsel;
static char cwd[256] = ROOT;

static int kind_of(const char *name) {
    const char *d = strrchr(name, '.');
    if (!d) return 0;
    if (!strcasecmp(d, ".jpg") || !strcasecmp(d, ".jpeg")) return 1;
    if (!strcasecmp(d, ".wav") || !strcasecmp(d, ".mp3")) return 2;
    return 0;
}
static int ent_cmp(const void *a, const void *b) {
    const ent_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}
static void browse(const char *dir) {
    list_gen++;
    if (!ents) ents = heap_caps_calloc(MAX_ENT, sizeof(ent_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    snprintf(cwd, sizeof cwd, "%.255s", dir);
    nents = 0; bsel = 0;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && nents < MAX_ENT) {
        if (e->d_name[0] == '.') continue;
        char p[400];
        snprintf(p, sizeof p, "%.255s/%.100s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        ent_t *x = &ents[nents];
        snprintf(x->name, sizeof x->name, "%.63s", e->d_name);
        x->dir = S_ISDIR(st.st_mode);
        x->size = (long)st.st_size;
        x->kind = x->dir ? 0 : kind_of(x->name);
        if (!x->dir && !x->kind) continue;
        nents++;
    }
    closedir(d);
    qsort(ents, nents, sizeof(ent_t), ent_cmp);
}
static void go_up(void) {
    if (!strcmp(cwd, ROOT)) { quit_app = APP_BACK; return; }   // Esc at the top of the media folder leaves the viewer
    char *s = strrchr(cwd, '/');
    if (s && s > cwd) { *s = 0; browse(cwd); }
}
static void to_loader(void) {
    esp_ota_set_boot_partition(esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL));
    esp_restart();
}

// ---------- pictures ----------
#define SW BOARD_LCD_WIDTH
#define SH BOARD_LCD_HEIGHT
static int pic_idx;   // index into ents

static bool show_picture(int idx) {
    char path[400];
    snprintf(path, sizeof path, "%.255s/%.100s", cwd, ents[idx].name);
    return imgview_file(path);   // the shared viewer, with zoom
}
static int next_picture(int from, int dir) {
    for (int i = from + dir; i >= 0 && i < nents; i += dir) if (ents[i].kind == 1) return i;
    return from;
}

// ---------- sound ----------
static esp_codec_dev_handle_t codec;
static bool audio_ready;
static volatile int play_cmd;       // 0 none, 1 stop, 2 pause toggle
static volatile bool playing, paused, track_done;
static volatile int vol = 55;
static volatile uint32_t played_ms, total_ms;
static int play_idx;
static TaskHandle_t play_task;

static bool audio_init(void) {
    if (audio_ready) return true;
    i2s_chan_handle_t tx = NULL;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.auto_clear = true;
    if (i2s_new_channel(&cc, &tx, NULL) != ESP_OK) return false;
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = 4, .bclk = 5, .ws = 7, .dout = 8, .din = I2S_GPIO_UNUSED, .invert_flags = {0}},
    };
    sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    if (i2s_channel_init_std_mode(tx, &sc) != ESP_OK) return false;
    i2s_channel_enable(tx);
    audio_codec_i2s_cfg_t ic = {.port = I2S_NUM_0, .tx_handle = tx, .rx_handle = NULL};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&ic);
    i2c_master_bus_handle_t bus = i2c_bus_get();
    audio_codec_i2c_cfg_t i2c = {.port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus};
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    es8311_codec_cfg_t ec = {.ctrl_if = ctrl_if, .gpio_if = gpio_if, .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC, .pa_pin = 1, .use_mclk = true, .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3}};
    const audio_codec_if_t *codec_if = es8311_codec_new(&ec);
    if (!data_if || !ctrl_if || !gpio_if || !codec_if) return false;
    esp_codec_dev_cfg_t dc = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec_if, .data_if = data_if};
    codec = esp_codec_dev_new(&dc);
    audio_ready = codec != NULL;
    return audio_ready;
}

static void pcm_out(const int16_t *pcm, int frames, int ch) {
    static int16_t mono[1152];
    int off = 0;
    while (off < frames) {
        int n = frames - off > 1152 ? 1152 : frames - off;
        if (ch == 2) for (int i = 0; i < n; i++) mono[i] = (int16_t)(((int)pcm[(off + i) * 2] + pcm[(off + i) * 2 + 1]) / 2);
        else memcpy(mono, pcm + off, (size_t)n * 2);
        esp_codec_dev_write(codec, mono, n * 2);
        off += n;
    }
}
static bool wait_if_paused(void) {   // true when the track should stop
    for (;;) {
        int c = play_cmd;
        if (c == 1) return true;
        if (c == 2) { play_cmd = 0; paused = !paused; }
        if (!paused) return false;
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}
static void play_wav(FILE *f) {
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return;
    int ch = 0, rate = 0, bits = 0;
    uint32_t data_len = 0;
    for (;;) {
        uint8_t ck[8];
        if (fread(ck, 1, 8, f) != 8) return;
        uint32_t len = ck[4] | ck[5] << 8 | ck[6] << 16 | (uint32_t)ck[7] << 24;
        if (!memcmp(ck, "fmt ", 4)) {
            uint8_t fm[16];
            if (len < 16 || fread(fm, 1, 16, f) != 16) return;
            ch = fm[2] | fm[3] << 8; rate = fm[4] | fm[5] << 8 | fm[6] << 16 | fm[7] << 24; bits = fm[14] | fm[15] << 8;
            fseek(f, (long)(len - 16 + (len & 1)), SEEK_CUR);
        } else if (!memcmp(ck, "data", 4)) { data_len = len; break; }
        else fseek(f, (long)(len + (len & 1)), SEEK_CUR);
    }
    if (bits != 16 || (ch != 1 && ch != 2) || rate < 8000 || rate > 48000) return;
    total_ms = (uint32_t)((uint64_t)data_len / (uint32_t)(ch * 2) * 1000 / (uint32_t)rate);
    esp_codec_dev_sample_info_t fs = {.bits_per_sample = 16, .channel = 1, .sample_rate = (uint32_t)rate};
    if (esp_codec_dev_open(codec, &fs) != ESP_CODEC_DEV_OK) return;
    esp_codec_dev_set_out_vol(codec, vol);
    static int16_t buf[2048];
    uint64_t frames_done = 0;
    while (!feof(f)) {
        if (wait_if_paused()) break;
        esp_codec_dev_set_out_vol(codec, vol);
        size_t n = fread(buf, 2 * (size_t)ch, 1024, f);
        if (!n) break;
        pcm_out(buf, (int)n, ch);
        frames_done += n;
        played_ms = (uint32_t)(frames_done * 1000 / (uint32_t)rate);
    }
    esp_codec_dev_close(codec);
}
static void play_mp3(FILE *f) {
    mp3dec_t *dec = heap_caps_malloc(sizeof *dec, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *in = heap_caps_malloc(16384, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *pcm = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!dec || !in || !pcm) { free(dec); free(in); free(pcm); return; }
    mp3dec_init(dec);
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    int have = 0, rate = 0;
    bool opened = false, eof = false;
    uint64_t frames_done = 0;
    long bytes_read = 0;
    int bitrate_kbps = 0;
    for (;;) {
        if (wait_if_paused()) break;
        if (have < 8192 && !eof) { size_t n = fread(in + have, 1, 16384 - (size_t)have, f); if (!n) eof = true; have += (int)n; bytes_read += (long)n; }
        if (have == 0) break;
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(dec, in, have, pcm, &info);
        if (info.frame_bytes == 0) { if (eof) break; continue; }
        have -= info.frame_bytes;
        memmove(in, in + info.frame_bytes, (size_t)have);
        if (samples > 0) {
            if (!opened) {
                rate = info.hz; bitrate_kbps = info.bitrate_kbps;
                if (bitrate_kbps > 0) total_ms = (uint32_t)((int64_t)fsize * 8 / bitrate_kbps);
                esp_codec_dev_sample_info_t fs = {.bits_per_sample = 16, .channel = 1, .sample_rate = (uint32_t)rate};
                if (esp_codec_dev_open(codec, &fs) != ESP_CODEC_DEV_OK) break;
                opened = true;
            }
            esp_codec_dev_set_out_vol(codec, vol);
            pcm_out(pcm, samples, info.channels);
            frames_done += (uint64_t)samples;
            played_ms = (uint32_t)(frames_done * 1000 / (uint32_t)rate);
        }
    }
    if (opened) esp_codec_dev_close(codec);
    free(dec); free(in); free(pcm);
}
static void play_task_fn(void *arg) {
    char path[400];
    snprintf(path, sizeof path, "%.255s/%.100s", cwd, ents[play_idx].name);
    FILE *f = fopen(path, "rb");
    played_ms = 0; total_ms = 0;
    if (f) {
        const char *ext = strrchr(path, '.');
        if (ext && !strcasecmp(ext, ".mp3")) play_mp3(f); else play_wav(f);
        fclose(f);
    }
    bool stopped = play_cmd == 1;
    play_cmd = 0; playing = false; paused = false;
    track_done = !stopped;
    play_task = NULL;
    vTaskDelete(NULL);
}
static void play_stop(void) {
    if (!play_task) return;
    play_cmd = 1;
    for (int i = 0; i < 100 && play_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
    track_done = false;
}
static bool play_start(int idx) {
    play_stop();
    if (!audio_init()) return false;
    play_idx = idx; play_cmd = 0; playing = true; paused = false; track_done = false;
    return xTaskCreate(play_task_fn, "play", 12288, NULL, 5, &play_task) == pdPASS;
}
static int next_track(int from, int dir) {
    for (int i = from + dir; i >= 0 && i < nents; i += dir) if (ents[i].kind == 2) return i;
    return from;
}

// ---------- drawing ----------
static void draw_browse(void) {
    clear();
    char b[300];
    snprintf(b, sizeof b, "Media  %.200s", cwd + strlen(SDCARD_MOUNT));
    header(b);
    const char *hint = "Enter open   Esc up   GUI menu   t theme";
    last_foot_n = foot_n_for(hint);
    int visible = rows - 1 - last_foot_n;
    fix_top(bsel, &btop, visible, nents);
    int top = btop;
    display_set_scrollbar(top, visible, nents);
    if (!sdcard_ok()) put_flow(2, 1, "No SD card, or it is not FAT32.", 0);
    else if (nents == 0) { int r = put_flow(2, 1, "Nothing here yet.", ATTR_BOLD); put_flow(r + 1, 1, "Put JPEG pictures, WAV and MP3 files in the media folder on the SD card. tools/a11convert.py makes them from other files. GUI opens the menu.", 0); }
    for (int i = 0; i < visible && top + i < nents; i++) {
        ent_t *x = &ents[top + i];
        if (x->dir) snprintf(b, sizeof b, " [dir]    %.200s", x->name);
        else snprintf(b, sizeof b, " %-8s %.200s  (%ld KB)", x->kind == 1 ? "[pic]" : "[sound]", x->name, (x->size + 1023) / 1024);
        int off = top + i == bsel ? marquee_off(list_gen * 4096 + bsel, (int)strlen(b), TC) : 0;
        put(1 + i, 0, b + off, 0);
        if (top + i == bsel) rowattr(1 + i, ATTR_REVERSE);
    }
    footer(hint);
}
static void fmt_time(char *out, size_t cap, uint32_t ms) { snprintf(out, cap, "%u:%02u", (unsigned)(ms / 60000), (unsigned)(ms / 1000 % 60)); }
static void draw_play(void) {
    clear();
    display_set_scrollbar(0, 0, 0);
    header("Now playing");
    char b[200], t1[16], t2[16];
    put(2, 2, ents[play_idx].name, ATTR_BOLD);
    fmt_time(t1, sizeof t1, played_ms);
    fmt_time(t2, sizeof t2, total_ms);
    snprintf(b, sizeof b, "%s  %s / %s", paused ? "Paused" : playing ? "Playing" : "Stopped", t1, total_ms ? t2 : "?");
    put(4, 2, b, 0);
    int w = TC - 6 > 50 ? 50 : TC - 6;
    char bar[64];
    int fill = total_ms ? (int)((uint64_t)played_ms * (uint64_t)w / total_ms) : 0;
    if (fill > w) fill = w;
    bar[0] = '[';
    for (int i = 0; i < w; i++) bar[1 + i] = i < fill ? '#' : '-';
    bar[1 + w] = ']'; bar[2 + w] = 0;
    put(6, 2, bar, 0);
    snprintf(b, sizeof b, "Volume %d%%", vol);
    put(8, 2, b, 0);
    footer("Space pause   Left Right track   Up Down volume   Esc stop");
}
static void draw_msg(void) {
    clear();
    display_set_scrollbar(0, 0, 0);
    header("Media");
    put_flow(2, 1, msg_text, 0);
    footer("Any key continues");
}
static void draw_screen(void) {
    switch (state) {
        case ST_BROWSE: draw_browse(); break;
        case ST_PLAY: draw_play(); break;
        case ST_MSG: draw_msg(); break;
        case ST_PICTURE: break;
        case ST_MENU: draw_menu(); break;
    }
}
static void draw(void) {
    if (!back_buf || state == ST_PICTURE) return;
    terminal_cell_t *live = scr;
    scr = back_buf;
    marquee_on = false;
    pend_cursor(0, 0, CURSOR_NONE);
    draw_screen();
    scr = live;
    memcpy(live, back_buf, (size_t)cols * rows * sizeof *scr);
    (display_set_cursor)(pc_x, pc_y, pc_m);
}

// ---------- keys ----------
static void leave_picture(void) { imgview_close(); state = ST_BROWSE; }
static void open_entry(int i) {
    if (i < 0 || i >= nents) return;
    ent_t *x = &ents[i];
    if (x->dir) { char p[400]; snprintf(p, sizeof p, "%.255s/%.100s", cwd, x->name); browse(p); }
    else if (x->kind == 1) {
        pic_idx = i;
        state = ST_PICTURE;
        if (!show_picture(i)) { leave_picture(); msg_show(ST_BROWSE, "That picture could not be shown. Use baseline JPEG up to 6 MB."); }
    } else if (x->kind == 2) {
        if (play_start(i)) state = ST_PLAY; else msg_show(ST_BROWSE, "Sound did not start, check the speaker setup in the log.");
    }
}
static void menu_toggle(void) {
    if (state == ST_MENU) { state = menu_back; return; }
    if (state == ST_PICTURE) { imgview_close(); menu_back = ST_BROWSE; }
    else menu_back = state == ST_MSG ? ST_BROWSE : state;
    menu_sel = 0;
    state = ST_MENU;
}
static void key_menu(uint16_t code) {
    if (code == HID_UP) menu_sel = menu_sel > 0 ? menu_sel - 1 : 4;       // wraps round
    else if (code == HID_DOWN) menu_sel = menu_sel < 4 ? menu_sel + 1 : 0;
    else if (code == HID_LEFT || code == HID_RIGHT) {
        int d = code == HID_RIGHT ? 1 : -1;
        if (menu_sel == 1) apply_theme(theme_idx + d);
        else if (menu_sel == 2) { apply_font(font_idx + d); browse(cwd); }
        else if (menu_sel == 3) apply_brightness(bright_idx + d);
    }
    else if (code == HID_ENTER) { if (menu_sel == 0) { quit_app = APP_BACK; } else if (menu_sel == 4) state = menu_back; }
    else if (code == HID_ESC) state = menu_back;
}
static bool power_wait;   // never set now, Mode 1 does the sleeping so the PIN lock stays in one place
static void power_off(void) { play_stop(); quit_app = APP_SLEEP; }
static void handle_key(a11_event_t ev) {
    uint16_t code = ev.code;
    if (code == A11_HID_CAPS_LOCK) code = A11_HID_LEFT_CTRL;
    if (code == A11_HID_LEFT_SHIFT) { shift = ev.down; if (ev.down) gui_tap = false; return; }
    if (code == A11_HID_LEFT_CTRL) { ctrl = ev.down; if (ev.down) gui_tap = false; return; }
    if (code == A11_HID_LEFT_ALT) { alt = ev.down; if (ev.down) gui_tap = false; return; }
    if (code == A11_HID_LEFT_GUI) {
        gui = ev.down;
        if (ev.down) gui_tap = true;
        else if (gui_tap) { gui_tap = false; if (!power_wait) menu_toggle(); }
        return;
    }
    if (!ev.down) return;
    gui_tap = false;
    if (power_wait) { if (code == A11_KEY_ON) { power_wait = false; display_sleep(false); } return; }
    if (code == A11_KEY_OFF) { power_off(); return; }
    if (code == A11_KEY_BACKLIGHT) { apply_brightness(bright_idx == 0 ? 5 : bright_idx - 1); return; }
    if (gui && code == HID_ESC) { play_stop(); to_loader(); }
    if (ctrl && (code == HID_MINUS || code == HID_EQUALS) && state != ST_PICTURE) { apply_font(font_idx + (code == HID_EQUALS ? 1 : -1)); if (state == ST_BROWSE) browse(cwd); return; }
    char c = a11_hid_to_char(code, false, false);
    switch (state) {
        case ST_MENU: key_menu(code); break;
        case ST_BROWSE:
            if (alt && code == HID_UP) code = HID_PGUP;   // Alt+Up and Alt+Down are Page Up and Page Down
            else if (alt && code == HID_DOWN) code = HID_PGDN;
            if (code == HID_UP && bsel > 0) bsel--;
            else if (code == HID_DOWN && bsel + 1 < nents) bsel++;
            else if (code == HID_PGUP) bsel = bsel > rows - 2 - last_foot_n ? bsel - (rows - 2 - last_foot_n) : 0;
            else if (code == HID_PGDN) bsel = bsel + rows - 2 - last_foot_n < nents ? bsel + rows - 2 - last_foot_n : (nents ? nents - 1 : 0);
            else if (code == HID_ENTER) open_entry(bsel);
            else if (code == HID_ESC || code == HID_BACKSPACE) go_up();
            else if (c == 't') apply_theme(theme_idx + 1);
            break;
        case ST_PICTURE: {
            iv_action_t act = imgview_key(code);
            if (act == IV_NEXT || act == IV_PREV) { int n = next_picture(pic_idx, act == IV_PREV ? -1 : 1); if (n != pic_idx) { pic_idx = n; if (!show_picture(n)) { leave_picture(); msg_show(ST_BROWSE, "That picture could not be shown. Use JPEG up to 6 MB."); } } }
            else if (act == IV_CLOSE) leave_picture();
            break;
        }
        case ST_PLAY:
            if (code == HID_ESC || code == HID_BACKSPACE) { play_stop(); state = ST_BROWSE; }
            else if (code == HID_SPACE) play_cmd = 2;
            else if (code == HID_UP) vol = vol + 5 > VOL_CAP ? VOL_CAP : vol + 5;
            else if (code == HID_DOWN) vol = vol < 5 ? 0 : vol - 5;
            else if (code == HID_RIGHT) { int n = next_track(play_idx, 1); if (n != play_idx) play_start(n); }
            else if (code == HID_LEFT) { int n = next_track(play_idx, -1); if (n != play_idx) play_start(n); }
            break;
        case ST_MSG: state = msg_back; break;
    }
}
static void handle_touch(touch_event_t te) {
    static int start_y;
    if (state == ST_PICTURE) {   // a tap zooms in or back out, a drag moves around when zoomed in, holding a finger down closes the picture
        if (imgview_touch(te) == IV_CLOSE) leave_picture();
        return;
    }
    int cw, ch;
    display_cell_size(&cw, &ch);
    // Dragging the scroll bar on the right edge jumps to the matching place in the list.
    static bool sbdrag;
    int vis = rows - 1 - last_foot_n;
    if (te.type == TOUCH_DOWN) sbdrag = state == ST_BROWSE && nents > vis && te.x >= BOARD_LCD_WIDTH - 3 * display_scrollbar_width();
    if (sbdrag) {
        if (te.type == TOUCH_UP) { sbdrag = false; return; }
        int y = te.y < 0 ? 0 : te.y > BOARD_LCD_HEIGHT - 1 ? BOARD_LCD_HEIGHT - 1 : te.y;
        btop = (int)((long)(nents - vis) * y / (BOARD_LCD_HEIGHT - 1));
        if (bsel < btop) bsel = btop;
        if (bsel >= btop + vis) bsel = btop + vis - 1;
        return;
    }
    if (te.type == TOUCH_DOWN) { start_y = te.y; return; }
    if (te.type == TOUCH_UP && abs(te.y - start_y) < 8) {
        if (state == ST_BROWSE) {
            int top = btop;
            int r = te.y / ch - 1;
            if (r >= 0 && top + r < nents) { bsel = top + r; open_entry(bsel); }
        } else if (state == ST_PICTURE) { /* handled above */ }
        else if (state == ST_PLAY) play_cmd = 2;
    }
}

app_exit_t media_app(QueueHandle_t q, QueueHandle_t tq) {
    quit_app = APP_NONE;
    shift = ctrl = alt = gui = false;
    gui_tap = false;
    font_idx = settings_get("font", 0);
    theme_idx = settings_get("theme", 0);
    bright_idx = settings_get("bl", 5);
    if (bright_idx < 0 || bright_idx > 5) bright_idx = 5;
    display_set_theme(themes_get(theme_idx % themes_count()));
    display_set_backlight(BRIGHT[bright_idx]);
    display_set_font(font_idx, &cols, &rows);
    scr_alloc();
    sdcard_mount();
    mkdir(ROOT, 0777);
    state = ST_BROWSE;
    browse(cwd[0] ? cwd : ROOT);
    ESP_LOGI(TAG, "media ready, %d entries", nents);
    int64_t last_draw = 0;
    while (!quit_app) {
        a11_event_t ev;
        touch_event_t te;
        bool changed = false;
        while (xQueueReceive(tq, &te, 0) == pdTRUE) { handle_touch(te); changed = true; }
        if (xQueueReceive(q, &ev, pdMS_TO_TICKS(40)) == pdTRUE) { handle_key(ev); changed = true; }
        if (quit_app) break;
        if (track_done && state == ST_PLAY) {   // next track of the folder, or stop at the end
            track_done = false;
            int n = next_track(play_idx, 1);
            if (n != play_idx && play_start(n)) changed = true; else { state = ST_BROWSE; changed = true; }
        }
        int64_t now = esp_timer_get_time();
        if ((changed || (state == ST_PLAY && now - last_draw > 500000) || (marquee_on && now - last_draw > 200000)) && state != ST_PICTURE) { draw(); last_draw = now; }
    }
    play_stop();   // the music stops when you leave
    if (state == ST_PICTURE) leave_picture();
    free(scr); free(back_buf);
    scr = back_buf = NULL;
    return quit_app;
}
