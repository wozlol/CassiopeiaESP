// The picture viewer. JPEG goes through the hardware friendly decoder at the size the view needs. PNG goes through a small streaming decoder of
// our own that shrinks while it decodes, so a big PNG never needs its full size in memory. Either way the picture on screen is made from a
// decoded copy that is only as big as the current zoom needs, and a limit from the free memory sets how far a very large picture can zoom.
#include <math.h>
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
#include "miniz.h"
#include "stb_image.h"

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
#define MAX_FILE (6 * 1024 * 1024)
#define MAX_STEPS 14
#define ZOOM_TOP 4.0f   // screen pixels per picture pixel at the last step

static char err_text[96] = "";
const char *imgview_error(void) { return err_text; }
static bool fail(const char *t) { snprintf(err_text, sizeof err_text, "%s", t); ESP_LOGW(TAG, "%s", t); return false; }

// ---------- the picture and what is decoded from it ----------
typedef enum { K_JPEG, K_PNG, K_RAW } kind_t;   // K_RAW: already decoded in full by the general decoder, kept as the buffer
static uint8_t *src; static uint32_t src_len; static kind_t kind;
static int fw, fh;                          // the whole picture
static uint16_t *buf; static int bw, bh, bden;   // the decoded copy, its size, and the shrink it was decoded at
static float zs[MAX_STEPS]; static int nz, zi;   // the zoom steps and the one showing
static float cx = 0.5f, cy = 0.5f;          // the middle of the view, as a share of the whole picture
static int den_min = 1;                     // the smallest shrink that fits in memory
static bool is_open;

static void free_buf(void) { free(buf); buf = NULL; bw = bh = bden = 0; }
static void free_all(void) { free_buf(); free(src); src = NULL; src_len = 0; }

// ---------- PNG ----------
typedef struct {
    uint32_t w, h;
    uint8_t depth, ctype, interlace, ch;
    uint8_t pal[256][3], palpha[256];
    int npal;
    bool has_trns; uint16_t trns_g, trns_rgb[3];
} png_t;
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static bool png_header(const uint8_t *d, size_t n, png_t *p) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (n < 33 || memcmp(d, sig, 8) || memcmp(d + 12, "IHDR", 4)) return false;
    memset(p, 0, sizeof *p);
    p->w = be32(d + 16); p->h = be32(d + 20); p->depth = d[24]; p->ctype = d[25]; p->interlace = d[28];
    p->ch = p->ctype == 0 ? 1 : p->ctype == 2 ? 3 : p->ctype == 3 ? 1 : p->ctype == 4 ? 2 : 4;
    for (size_t o = 33; o + 12 <= n;) {   // palette and transparency chunks come before the data
        uint32_t cl = be32(d + o);
        const uint8_t *ty = d + o + 4, *cd = d + o + 8;
        if (o + 12 + cl > n) break;
        if (!memcmp(ty, "PLTE", 4)) { p->npal = (int)(cl / 3) > 256 ? 256 : (int)(cl / 3); for (int i = 0; i < p->npal; i++) memcpy(p->pal[i], cd + 3 * i, 3); }
        else if (!memcmp(ty, "tRNS", 4)) {
            p->has_trns = true;
            if (p->ctype == 3) for (uint32_t i = 0; i < cl && i < 256; i++) p->palpha[i] = cd[i];
            else if (p->ctype == 0 && cl >= 2) p->trns_g = (uint16_t)((cd[0] << 8) | cd[1]);
            else if (p->ctype == 2 && cl >= 6) for (int i = 0; i < 3; i++) p->trns_rgb[i] = (uint16_t)((cd[2 * i] << 8) | cd[2 * i + 1]);
        } else if (!memcmp(ty, "IDAT", 4)) break;
        o += 12 + cl;
    }
    if (p->ctype == 3) for (int i = 0; i < 256; i++) if (!p->has_trns) p->palpha[i] = 255;
    return p->w > 0 && p->h > 0 && p->w <= 30000 && p->h <= 30000;
}
static inline uint8_t paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}
// Decodes row by row and shrinks by an integer on the way. Returns a new buffer of ceil(w/den) by ceil(h/den), panel ordered RGB565.
static uint16_t *png_decode(const uint8_t *d, size_t n, const png_t *p, int den, int *ow, int *oh) {
    int w = (int)p->w, h = (int)p->h;
    int ocw = (w + den - 1) / den, och = (h + den - 1) / den;
    size_t stride = ((size_t)w * p->ch * p->depth + 7) / 8;
    int bpp = (p->ch * p->depth + 7) / 8;
    uint16_t *out = heap_caps_malloc((size_t)ocw * och * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *line = heap_caps_calloc(1, stride + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *prev = heap_caps_calloc(1, stride + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *dict = heap_caps_malloc(TINFL_LZ_DICT_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t *sum = heap_caps_calloc((size_t)ocw * 3, 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    tinfl_decompressor *inf = heap_caps_malloc(sizeof *inf, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool ok = out && line && prev && dict && sum && inf;
    if (!ok) { fail("Not enough memory for that picture."); goto done; }
    tinfl_init(inf);
    size_t lpos = 0, dict_ofs = 0;
    int y = 0;
    // the next piece of compressed data: the chunks named IDAT, one after the other
    size_t chunk_o = 33, in_left = 0; const uint8_t *in_p = NULL;
    bool done_inflate = false;
    while (y < h && !done_inflate) {
        if (in_left == 0) {   // find the next IDAT chunk
            in_p = NULL;
            while (chunk_o + 12 <= n) {
                uint32_t cl = be32(d + chunk_o);
                if (chunk_o + 12 + cl > n) break;
                bool idat = !memcmp(d + chunk_o + 4, "IDAT", 4);
                size_t at = chunk_o + 8;
                chunk_o += 12 + cl;
                if (idat && cl) { in_p = d + at; in_left = cl; break; }
            }
        }
        bool more = in_p != NULL;
        size_t in_n = in_left, out_n = TINFL_LZ_DICT_SIZE - dict_ofs;
        int flags = (more ? TINFL_FLAG_HAS_MORE_INPUT : 0) | TINFL_FLAG_PARSE_ZLIB_HEADER;   // PNG data is zlib wrapped
        if (!in_p) in_n = 0;
        tinfl_status st = tinfl_decompress(inf, in_p, &in_n, dict, dict + dict_ofs, &out_n, flags);
        if (in_p) { in_p += in_n; in_left -= in_n; }
        if (st < TINFL_STATUS_DONE) { ok = false; fail("That PNG is damaged."); goto done; }
        // the new bytes, fed to the row builder
        const uint8_t *s = dict + dict_ofs;
        size_t left = out_n;
        while (left && y < h) {
            size_t take = stride + 1 - lpos;
            if (take > left) take = left;
            memcpy(line + lpos, s, take);
            lpos += take; s += take; left -= take;
            if (lpos < stride + 1) break;
            lpos = 0;
            // undo the row filter
            uint8_t ft = line[0];
            uint8_t *cur = line + 1, *up = prev + 1;
            for (size_t i = 0; i < stride; i++) {
                int a = i >= (size_t)bpp ? cur[i - bpp] : 0, b = up[i], c = i >= (size_t)bpp ? up[i - bpp] : 0;
                int v = cur[i];
                switch (ft) { case 1: v += a; break; case 2: v += b; break; case 3: v += (a + b) / 2; break; case 4: v += paeth(a, b, c); break; default: break; }
                cur[i] = (uint8_t)v;
            }
            // to colors, added into the block of the shrunk row
            uint32_t *sp = sum + 0;
            for (int x = 0; x < w; x++) {
                int r, g, bl, al = 255;
                if (p->depth == 8 || p->depth == 16) {
                    int st2 = p->depth == 8 ? 1 : 2;
                    const uint8_t *px = cur + (size_t)x * p->ch * st2;
                    switch (p->ctype) {
                        case 0: r = g = bl = px[0]; if (p->has_trns && (uint16_t)(st2 == 1 ? px[0] : (px[0] << 8) | px[1]) == p->trns_g) al = 0; break;
                        case 2: r = px[0]; g = px[st2]; bl = px[2 * st2];
                            if (p->has_trns && (st2 == 1 ? (px[0] == p->trns_rgb[0] && px[1] == p->trns_rgb[1] && px[2] == p->trns_rgb[2]) : (((px[0] << 8) | px[1]) == p->trns_rgb[0] && ((px[2] << 8) | px[3]) == p->trns_rgb[1] && ((px[4] << 8) | px[5]) == p->trns_rgb[2]))) al = 0;
                            break;
                        case 3: { int ix = px[0]; r = p->pal[ix][0]; g = p->pal[ix][1]; bl = p->pal[ix][2]; al = p->palpha[ix]; break; }
                        case 4: r = g = bl = px[0]; al = px[st2]; break;
                        default: r = px[0]; g = px[st2]; bl = px[2 * st2]; al = px[3 * st2]; break;
                    }
                } else {   // 1, 2 and 4 bit gray or palette
                    int perbyte = 8 / p->depth, idx = x / perbyte, sh = 8 - p->depth * (x % perbyte + 1);
                    int v = (cur[idx] >> sh) & ((1 << p->depth) - 1);
                    if (p->ctype == 3) { r = p->pal[v][0]; g = p->pal[v][1]; bl = p->pal[v][2]; al = p->palpha[v]; }
                    else { r = g = bl = v * 255 / ((1 << p->depth) - 1); if (p->has_trns && v == p->trns_g) al = 0; }
                }
                if (al < 255) { r = (r * al + 128 * (255 - al)) / 255; g = (g * al + 128 * (255 - al)) / 255; bl = (bl * al + 128 * (255 - al)) / 255; }   // see through parts show as mid gray
                uint32_t *q = sp + (size_t)(x / den) * 3;
                q[0] += (uint32_t)r; q[1] += (uint32_t)g; q[2] += (uint32_t)bl;
            }
            if ((y % den) == den - 1 || y == h - 1) {
                int rows = (y % den) + 1;
                uint16_t *orow = out + (size_t)(y / den) * ocw;
                for (int ox = 0; ox < ocw; ox++) {
                    int cols = w - ox * den < den ? w - ox * den : den;
                    uint32_t cnt = (uint32_t)(rows * cols);
                    uint32_t *q = sum + (size_t)ox * 3;
                    uint32_t R = q[0] / cnt, G = q[1] / cnt, B = q[2] / cnt;
                    uint16_t v = (uint16_t)(((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (B >> 3));
                    orow[ox] = (uint16_t)((v >> 8) | (v << 8));
                    q[0] = q[1] = q[2] = 0;
                }
            }
            memcpy(prev, line, stride + 1);
            y++;
        }
        dict_ofs = (dict_ofs + out_n) & (TINFL_LZ_DICT_SIZE - 1);
        if (st == TINFL_STATUS_DONE) done_inflate = true;
        if (!more && st == TINFL_STATUS_NEEDS_MORE_INPUT) break;
    }
    if (y < h) { ok = false; fail("That PNG ends too early."); }
done:
    free(line); free(prev); free(dict); free(sum); free(inf);
    if (!ok) { free(out); return NULL; }
    *ow = ocw; *oh = och;
    return out;
}

// ---------- decoding to the size the view needs ----------
static const int DENS[] = {1, 2, 4, 8, 16, 32};
static bool decode(int den) {
    if (kind == K_RAW) return buf != NULL;   // the one full size copy is all there is
    if (buf && bden == den) return true;
    free_buf();
    if (kind == K_JPEG) {
        esp_jpeg_image_cfg_t cfg = {.indata = src, .indata_size = src_len, .out_format = JPEG_IMAGE_FORMAT_RGB565,
                                    .out_scale = den == 1 ? JPEG_IMAGE_SCALE_0 : den == 2 ? JPEG_IMAGE_SCALE_1_2 : den == 4 ? JPEG_IMAGE_SCALE_1_4 : JPEG_IMAGE_SCALE_1_8,
                                    .flags = {.swap_color_bytes = 1}};
        esp_jpeg_image_output_t info;
        if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK) return fail("That JPEG could not be read.");
        cfg.outbuf_size = (uint32_t)info.width * info.height * 2 + 64;
        buf = heap_caps_malloc(cfg.outbuf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) return fail("Not enough memory for that picture.");
        cfg.outbuf = (uint8_t *)buf;
        if (esp_jpeg_decode(&cfg, &info) != ESP_OK) { free_buf(); return fail("That JPEG could not be decoded."); }
        bw = info.width; bh = info.height; bden = den;
        return true;
    }
    png_t p;
    if (!png_header(src, src_len, &p)) return fail("That PNG could not be read.");
    int ow = 0, oh = 0;
    buf = png_decode(src, src_len, &p, den, &ow, &oh);
    if (!buf) return false;
    bw = ow; bh = oh; bden = den;
    return true;
}

// ---------- the general decoder, for what the others cannot read ----------
// The picture is decoded whole, so it has to fit in memory with room to spare. A progressive JPEG needs about 8 bytes per pixel while it decodes.
static bool open_general(size_t budget, bool progressive) {
    int w = 0, h = 0, c = 0;
    if (!stbi_info_from_memory(src, (int)src_len, &w, &h, &c)) return fail("That picture type is not supported.");
    size_t per = progressive ? 8 : 4;
    if ((size_t)w * h * per > budget) {
        char t[96];
        snprintf(t, sizeof t, "Too big. %s pictures up to about %.1f megapixels fit here.", progressive ? "Progressive JPEG" : "These", (double)budget / per / 1e6);
        return fail(t);
    }
    uint8_t *rgb = stbi_load_from_memory(src, (int)src_len, &w, &h, &c, 3);
    if (!rgb) return fail("That picture could not be decoded.");
    uint16_t *o = (uint16_t *)rgb;   // converted in place, each output pixel is behind the three bytes it came from
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint16_t v = (uint16_t)(((rgb[3 * i] & 0xF8) << 8) | ((rgb[3 * i + 1] & 0xFC) << 3) | (rgb[3 * i + 2] >> 3));
        o[i] = (uint16_t)((v >> 8) | (v << 8));
    }
    uint16_t *small = heap_caps_realloc(rgb, (size_t)w * h * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    free(src); src = NULL; src_len = 0;   // the compressed file is not needed any more
    buf = small ? small : o;
    bw = fw = w; bh = fh = h; bden = 1;
    kind = K_RAW;
    return true;
}

// ---------- zoom steps ----------
static void build_steps(size_t budget) {
    // the smallest shrink whose decoded copy fits in memory
    den_min = 32;
    for (int i = 0; i < 6; i++) {
        size_t bytes = (size_t)((fw + DENS[i] - 1) / DENS[i]) * (size_t)((fh + DENS[i] - 1) / DENS[i]) * 2;
        if (bytes <= budget) { den_min = DENS[i]; break; }
    }
    float zfit = fminf((float)SW / fw, (float)SH / fh);
    float zmax = ZOOM_TOP / den_min;                  // a very large picture cannot show every one of its pixels, this is how far it can go
    nz = 0;
    float s0 = zfit < 1.0f ? zfit : 1.0f;             // the whole picture, small pictures are not blown up yet
    zs[nz++] = s0;
    float z = s0;
    if (zfit > s0 * 1.05f) { z = zfit; zs[nz++] = z; }   // first zoom: full width or height
    while (nz < MAX_STEPS - 1 && z * 1.05f < zmax) {
        z = z * 2.0f;
        if (z > zmax) z = zmax;
        zs[nz++] = z;
    }
    zi = 0;
}
static int den_for(float z) {
    if (kind == K_RAW) return 1;
    int d = den_min;
    for (int i = 0; i < 6; i++) if (DENS[i] >= den_min && z * DENS[i] <= 1.0f) d = DENS[i];   // the biggest shrink that still has a pixel per screen pixel
    return d;
}

static bool render(void) {
    float z = zs[zi];
    if (!decode(den_for(z))) return false;
    float viewW = SW / z, viewH = SH / z;   // how much of the picture the screen shows, in picture pixels
    float cxF = cx * fw, cyF = cy * fh;
    if (viewW >= fw) cxF = fw / 2.0f; else { if (cxF < viewW / 2) cxF = viewW / 2; if (cxF > fw - viewW / 2) cxF = fw - viewW / 2; }
    if (viewH >= fh) cyF = fh / 2.0f; else { if (cyF < viewH / 2) cyF = viewH / 2; if (cyF > fh - viewH / 2) cyF = fh - viewH / 2; }
    cx = cxF / fw; cy = cyF / fh;
    uint16_t *screen = heap_caps_calloc((size_t)SW * SH, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!screen) return fail("Not enough memory for that picture.");
    int32_t step = (int32_t)(65536.0f / z / bden);                       // decoded pixels per screen pixel, 16.16
    int32_t fx0 = (int32_t)(((cxF - viewW / 2) / bden) * 65536.0f);
    int32_t fy0 = (int32_t)(((cyF - viewH / 2) / bden) * 65536.0f);
    for (int y = 0; y < SH; y++) {
        int32_t sy = (fy0 + y * step) >> 16;
        if (sy < 0 || sy >= bh) continue;
        const uint16_t *row = buf + (size_t)sy * bw;
        uint16_t *o = screen + (size_t)y * SW;
        int32_t fx = fx0;
        for (int x = 0; x < SW; x++, fx += step) { int32_t sx = fx >> 16; if (sx >= 0 && sx < bw) o[x] = row[sx]; }
    }
    display_raw(true);
    display_draw_pixels(0, 0, SW, SH, screen);
    free(screen);
    return true;
}
static void zoom_to(int idx, float nx, float ny) {
    if (idx < 0) idx = 0;
    if (idx >= nz) idx = nz - 1;
    zi = idx;
    if (nx >= 0) { cx = nx; cy = ny; }
    render();
}
static void pan(int dxs, int dys) {   // in screen pixels
    float z = zs[zi];
    cx += dxs / z / fw; cy += dys / z / fh;
    render();
}

// ---------- the public side ----------
void imgview_close(void) { free_all(); display_raw(false); is_open = false; }
bool imgview_active(void) { return is_open; }

bool imgview_open(uint8_t *data, uint32_t len) {
    free_all();
    err_text[0] = 0;
    src = data; src_len = len;
    bool progressive = false;
    size_t freeb0 = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t budget0 = freeb0 > 1500000 ? freeb0 - 1500000 : 0;
    if (budget0 > 6 * 1024 * 1024) budget0 = 6 * 1024 * 1024;
    if (len > 8 && ((data[0] == 'B' && data[1] == 'M') || !memcmp(data, "GIF8", 4))) {   // BMP and GIF
        if (!open_general(budget0, false)) { free_all(); return false; }
        goto shown;
    }
    if (len > 8 && data[0] == 0xFF && data[1] == 0xD8) {
        kind = K_JPEG;
        for (uint32_t i = 2; i + 9 < len;) {   // the size, and whether it is progressive
            if (data[i] != 0xFF) { i++; continue; }
            uint8_t m = data[i + 1];
            if (m == 0xFF) { i++; continue; }
            if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { i += 2; continue; }
            uint32_t sl = ((uint32_t)data[i + 2] << 8) | data[i + 3];
            if (m == 0xC2 || m == 0xC6 || m == 0xCA) { progressive = true; }
            if (m == 0xC0 || m == 0xC1) { fh = (data[i + 5] << 8) | data[i + 6]; fw = (data[i + 7] << 8) | data[i + 8]; break; }
            i += 2 + sl;
        }
        if (progressive) {
            if (!open_general(budget0, true)) { free_all(); return false; }
            goto shown;
        }
        if (fw < 1 || fh < 1) { free_all(); fail("That JPEG could not be read."); return false; }
    } else {
        png_t p;
        if (!png_header(data, len, &p)) { free_all(); fail("That is not a JPEG or PNG picture."); return false; }
        if (p.interlace) { free_all(); fail("Interlaced PNG is not supported yet."); return false; }
        if (p.depth != 1 && p.depth != 2 && p.depth != 4 && p.depth != 8 && p.depth != 16) { free_all(); fail("That PNG type is not supported."); return false; }
        kind = K_PNG; fw = (int)p.w; fh = (int)p.h;
    }
    size_t freeb = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t budget = freeb > 1500000 ? freeb - 1500000 : 0;   // room is kept for the screen copy and the decoder
    if (budget > 6 * 1024 * 1024) budget = 6 * 1024 * 1024;
    build_steps(budget);
    cx = cy = 0.5f;
    if (!render()) {
        if (kind == K_JPEG) {   // the quick decoder could not do this one, the general one is tried
            free_buf();
            if (open_general(budget, false)) goto shown;
        }
        free_all(); display_raw(false); return false;
    }
    is_open = true;
    return true;
shown:
    build_steps(budget0);
    cx = cy = 0.5f;
    if (!render()) { free_all(); display_raw(false); return false; }
    is_open = true;
    return true;
}

bool imgview_file(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) { fail("That file could not be found."); return false; }
    if (st.st_size > MAX_FILE) { fail("That picture file is over 6 MB."); return false; }
    uint8_t *d = heap_caps_malloc((size_t)st.st_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!d) { fail("Not enough memory for that picture."); return false; }
    FILE *f = fopen(path, "rb");
    bool ok = f && fread(d, 1, (size_t)st.st_size, f) == (size_t)st.st_size;
    if (f) fclose(f);
    if (!ok) { free(d); fail("That file could not be read."); return false; }
    return imgview_open(d, (uint32_t)st.st_size);
}

iv_action_t imgview_key(uint16_t code) {
    if (code == HID_ENTER) zoom_to((zi + 1) % nz, -1, -1);                         // each Enter zooms in one step, after the last it shows the whole picture again
    else if (code == HID_EQUALS) zoom_to(zi + 1, -1, -1);                          // plus: one step in
    else if (code == HID_MINUS) zoom_to(zi - 1, -1, -1);                           // minus: one step out
    else if (zi > 0 && (code == HID_RIGHT || code == HID_LEFT || code == HID_UP || code == HID_DOWN))   // zoomed in: the arrows move the view
        pan(code == HID_RIGHT ? SW / 3 : code == HID_LEFT ? -SW / 3 : 0, code == HID_DOWN ? SH / 3 : code == HID_UP ? -SH / 3 : 0);
    else if (code == HID_RIGHT || code == HID_DOWN || code == HID_SPACE) return IV_NEXT;   // the whole picture is showing: the next picture
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
        if (moved && zi > 0) pan(lx - te.x, ly - te.y);
        lx = te.x; ly = te.y;
    } else if (te.type == TOUCH_UP && !moved) {
        if (tn - it0 > 700000) return IV_CLOSE;   // holding a finger down closes the picture
        if (zi + 1 < nz) {   // a tap zooms in one step on the spot touched
            float z = zs[zi];
            float px = cx * fw + (ix - SW / 2) / z, py = cy * fh + (iy - SH / 2) / z;
            zoom_to(zi + 1, px / fw, py / fh);
        } else zoom_to(0, 0.5f, 0.5f);   // after the last step it starts over with the whole picture
    }
    return IV_NONE;
}
