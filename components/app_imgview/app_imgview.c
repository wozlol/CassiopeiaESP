// The picture viewer, shared by the reader, the media viewer and the file manager. It starts fitted to the screen. Zoom steps go to bigger,
// and the arrows or a finger drag move around it. Large pictures, like pages of a manual, are decoded at the size the view needs, so the
// fitted view is quick and a zoomed view is sharp.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "app_imgview.h"
#include "board.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"

static const char *TAG = "imgview";
#define SW BOARD_LCD_WIDTH
#define SH BOARD_LCD_HEIGHT
#define HID_ENTER 40
#define HID_SPACE 44
#define HID_MINUS 45
#define HID_EQUALS 46
#define HID_RIGHT 79
#define HID_LEFT 80
#define HID_DOWN 81
#define HID_UP 82
#define MAX_PIC (6 * 1024 * 1024)

static uint8_t *iv_jpg; static uint32_t iv_len;
static uint16_t *iv_buf; static int iv_w, iv_h, iv_den;   // the decoded copy, its size, and the jpeg scale it was decoded at (1, 2, 4 or 8)
static int iv_fw, iv_fh;                                   // size of the whole picture
static int iv_level;                                       // 0 fitted, 1 and 2 zoomed in
static float iv_cx = 0.5f, iv_cy = 0.5f;                   // the middle of the view, as a share of the whole picture
static void iv_free(void) { free(iv_jpg); free(iv_buf); iv_jpg = NULL; iv_buf = NULL; iv_w = iv_h = iv_den = 0; }
static float iv_zoom(int level) {
    float z0 = (float)SW / iv_fw < (float)SH / iv_fh ? (float)SW / iv_fw : (float)SH / iv_fh;
    if (z0 > 1.0f) z0 = 1.0f;
    float z1 = z0 < 1.0f ? 1.0f : 2.0f;
    return level == 0 ? z0 : level == 1 ? z1 : z1 * 2.0f;
}
static bool iv_decode(int den) {
    if (iv_buf && iv_den == den) return true;
    free(iv_buf); iv_buf = NULL;
    esp_jpeg_image_cfg_t cfg = {.indata = iv_jpg, .indata_size = iv_len, .out_format = JPEG_IMAGE_FORMAT_RGB565,
                                .out_scale = den == 1 ? JPEG_IMAGE_SCALE_0 : den == 2 ? JPEG_IMAGE_SCALE_1_2 : den == 4 ? JPEG_IMAGE_SCALE_1_4 : JPEG_IMAGE_SCALE_1_8,
                                .flags = {.swap_color_bytes = 1}};
    esp_jpeg_image_output_t info;
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK) return false;
    cfg.outbuf_size = (uint32_t)info.width * info.height * 2 + 64;
    iv_buf = heap_caps_malloc(cfg.outbuf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!iv_buf) return false;
    cfg.outbuf = (uint8_t *)iv_buf;
    if (esp_jpeg_decode(&cfg, &info) != ESP_OK) { free(iv_buf); iv_buf = NULL; return false; }
    iv_w = info.width; iv_h = info.height; iv_den = den;
    return true;
}
static bool iv_render(void) {
    float z = iv_zoom(iv_level);
    int den = 1;
    if (z < 1.0f) { while (den < 8 && 1.0f / (den * 2) >= z) den *= 2; }   // the biggest shrink that still has enough pixels
    if (!iv_decode(den)) return false;
    float viewW = SW / z, viewH = SH / z;                                  // how much of the picture the screen shows, in picture pixels
    float cxF = iv_cx * iv_fw, cyF = iv_cy * iv_fh;
    if (viewW >= iv_fw) cxF = iv_fw / 2.0f; else { if (cxF < viewW / 2) cxF = viewW / 2; if (cxF > iv_fw - viewW / 2) cxF = iv_fw - viewW / 2; }
    if (viewH >= iv_fh) cyF = iv_fh / 2.0f; else { if (cyF < viewH / 2) cyF = viewH / 2; if (cyF > iv_fh - viewH / 2) cyF = iv_fh - viewH / 2; }
    iv_cx = cxF / iv_fw; iv_cy = cyF / iv_fh;
    uint16_t *screen = heap_caps_calloc((size_t)SW * SH, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!screen) return false;
    int32_t step = (int32_t)(65536.0f / z / den);                           // decoded pixels per screen pixel, 16.16
    int32_t fx0 = (int32_t)(((cxF - viewW / 2) / den) * 65536.0f);
    int32_t fy0 = (int32_t)(((cyF - viewH / 2) / den) * 65536.0f);
    for (int y = 0; y < SH; y++) {
        int32_t sy = (fy0 + y * step) >> 16;
        if (sy < 0 || sy >= iv_h) continue;
        const uint16_t *row = iv_buf + (size_t)sy * iv_w;
        uint16_t *out = screen + (size_t)y * SW;
        int32_t fx = fx0;
        for (int x = 0; x < SW; x++, fx += step) { int32_t sx = fx >> 16; if (sx >= 0 && sx < iv_w) out[x] = row[sx]; }
    }
    display_raw(true);
    display_draw_pixels(0, 0, SW, SH, screen);
    free(screen);
    return true;
}
static void iv_zoom_to(int level, float cx, float cy) {
    if (level < 0) level = 0;
    if (level > 2) level = 2;
    iv_level = level;
    if (cx >= 0) { iv_cx = cx; iv_cy = cy; }
    iv_render();
}
static void iv_pan(int dxs, int dys) {   // in screen pixels
    float z = iv_zoom(iv_level);
    iv_cx += dxs / z / iv_fw; iv_cy += dys / z / iv_fh;
    iv_render();
}

static bool is_open;

void imgview_close(void) { iv_free(); display_raw(false); is_open = false; }
bool imgview_active(void) { return is_open; }

bool imgview_open(uint8_t *jpg, uint32_t len) {
    iv_free();
    iv_jpg = jpg; iv_len = len;
    esp_jpeg_image_cfg_t cfg = {.indata = jpg, .indata_size = len, .out_format = JPEG_IMAGE_FORMAT_RGB565, .out_scale = JPEG_IMAGE_SCALE_0, .flags = {.swap_color_bytes = 1}};
    esp_jpeg_image_output_t info;
    esp_err_t e = esp_jpeg_get_image_info(&cfg, &info);
    if (e != ESP_OK || info.width < 1 || info.height < 1) { ESP_LOGW(TAG, "not a readable JPEG (%s)", esp_err_to_name(e)); iv_free(); return false; }
    iv_fw = info.width; iv_fh = info.height;
    iv_level = 0; iv_cx = iv_cy = 0.5f;
    if (!iv_render()) { ESP_LOGW(TAG, "could not draw the picture, %dx%d", (int)info.width, (int)info.height); imgview_close(); return false; }
    is_open = true;
    return true;
}

bool imgview_file(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > MAX_PIC) { ESP_LOGW(TAG, "%s: not found or over 6 MB", path); return false; }
    uint8_t *jpg = heap_caps_malloc((size_t)st.st_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpg) { ESP_LOGW(TAG, "%s: no memory for the file", path); return false; }
    FILE *f = fopen(path, "rb");
    bool ok = f && fread(jpg, 1, (size_t)st.st_size, f) == (size_t)st.st_size;
    if (f) fclose(f);
    if (!ok) { ESP_LOGW(TAG, "%s: could not read it", path); free(jpg); return false; }
    return imgview_open(jpg, (uint32_t)st.st_size);
}

iv_action_t imgview_key(uint16_t code) {
    if (code == HID_EQUALS) iv_zoom_to(iv_level + 1, -1, -1);                 // plus: zoom in
    else if (code == HID_MINUS) iv_zoom_to(iv_level - 1, -1, -1);             // minus: zoom out
    else if (code == HID_ENTER) iv_zoom_to(iv_level ? 0 : 1, -1, -1);         // Enter: zoomed in or fitted
    else if (iv_level > 0 && (code == HID_RIGHT || code == HID_LEFT || code == HID_UP || code == HID_DOWN))   // zoomed in: the arrows move the view
        iv_pan(code == HID_RIGHT ? SW / 3 : code == HID_LEFT ? -SW / 3 : 0, code == HID_DOWN ? SH / 3 : code == HID_UP ? -SH / 3 : 0);
    else if (code == HID_RIGHT || code == HID_DOWN || code == HID_SPACE) return IV_NEXT;   // fitted: the next picture
    else if (code == HID_LEFT || code == HID_UP) return IV_PREV;
    else return IV_CLOSE;
    return IV_NONE;
}

iv_action_t imgview_touch(touch_event_t te) {
    static int ix, iy, lx, ly;
    static int64_t it0;
    static bool moved;
    int64_t tn = esp_timer_get_time();
    if (te.type == TOUCH_DOWN) { ix = lx = te.x; iy = ly = te.y; it0 = tn; moved = false; }
    else if (te.type == TOUCH_MOVE) {
        if (abs(te.x - ix) > 14 || abs(te.y - iy) > 14) moved = true;
        if (moved && iv_level > 0) iv_pan(lx - te.x, ly - te.y);
        lx = te.x; ly = te.y;
    } else if (te.type == TOUCH_UP && !moved) {
        if (tn - it0 > 700000) return IV_CLOSE;   // holding a finger down closes the picture
        if (iv_level == 0) {   // a tap zooms in on the spot touched
            float z = iv_zoom(0);
            float cxF = iv_cx * iv_fw, cyF = iv_cy * iv_fh;
            float px = cxF + (ix - SW / 2) / z, py = cyF + (iy - SH / 2) / z;
            iv_zoom_to(1, px / iv_fw, py / iv_fh);
        } else iv_zoom_to(0, -1, -1);
    }
    return IV_NONE;
}
