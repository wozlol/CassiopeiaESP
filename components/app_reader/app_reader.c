// Reference reader, now a screen inside Mode 1. Call reader_app() and it returns when the person leaves it.
// Reference reader. Browses /sdcard/library, shows text, Markdown and HTML files, and reference packs made by tools/a11pack.py
// (Wikipedia and other Kiwix files). Same text sizes, themes, scroll keys and brightness key as Mode 1.
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "a11_keys.h"
#include "board.h"
#include "clipboard.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "miniz.h"
#include "sdcard.h"
#include "settings.h"
#include "themes.h"
#include "status_led.h"
#include "touch.h"
#include "app_imgview.h"
#include "app_status.h"
#include "app_reader.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "reader";
#define LIB SDCARD_MOUNT "/library"
#define MAX_ENT 300
#define MAX_FILE (1024 * 1024)
#define MAX_LINKS 600
#define LINKSTR_CAP 24576
#define HID_ENTER 40
#define HID_ESC 41
#define HID_BACKSPACE 42
#define HID_TAB 43
#define HID_SPACE 44
#define HID_MINUS 45
#define HID_EQUALS 46
#define HID_HOME 74
#define HID_PGUP 75
#define HID_END 77
#define HID_PGDN 78
#define HID_RIGHT 79
#define HID_LEFT 80
#define HID_DOWN 81
#define HID_UP 82
static const int BRIGHT[] = {10, 25, 40, 60, 80, 100};

typedef enum { ST_BROWSE, ST_TEXT, ST_FIND, ST_SEARCH, ST_MSG, ST_IMAGE, ST_MENU } state_t;
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
static int btop, rtop;   // first visible row of the library list and of the search results
static int tsel, ttop;   // selected and first visible title of the browsable title list inside a pack
static bool engaged;     // Up or Down was pressed in the title list, so Enter opens the title instead of searching
static int s_first, s_rvis;   // first row and row count of the list on the search screen, for touch
// Keeps the selected row on screen without moving the list more than it has to.
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
static int foot_n_for(const char *t) {
    if (state == ST_FIND) return 1;   // the find bar sits on the bottom row
    if (font_idx >= 2) return 0;      // the bottom line only shows at the two smallest text sizes
    int n = wrap_rows(t, TC - 2);
    return n > 3 ? 3 : n;
}
static void footer(const char *t) {
    int n = foot_n_for(t);
    last_foot_n = n;
    if (n == 0) return;
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


// ---------- GUI menu ----------
static int menu_sel;
static state_t menu_back = ST_BROWSE;
static bool gui_tap;
static void to_loader(void);
static app_exit_t quit_app;   // set when the person leaves, the loop in reader_app then returns
static void draw_menu(void) {
    display_set_scrollbar(0, 0, 0);
    clear();
    header("Menu");
    { char bt[24]; app_battery_text(bt, sizeof bt); if (bt[0]) put(0, TC - (int)strlen(bt) - 1, bt, ATTR_REVERSE); }
    int cw, ch;
    display_cell_size(&cw, &ch);
    char b[100];
    put(2, 1, "Back to the terminal (restarts)", 0);
    put(3, 1, "Open the loader (restarts)", 0);
    snprintf(b, sizeof b, "Theme        < %s >", themes_get(theme_idx)->name); put(4, 1, b, 0);
    snprintf(b, sizeof b, "Text size    < %dx%d, %dx%d cells >", cw, ch, cols, rows); put(5, 1, b, 0);
    snprintf(b, sizeof b, "Brightness   < %d%% >", BRIGHT[bright_idx]); put(6, 1, b, 0);
    put(7, 1, "Close", 0);
    rowattr(2 + menu_sel, ATTR_REVERSE);
    footer(" Up Down move   Left Right change   Enter select   GUI closes");
}
static void draw(void);

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
static void msg_show(state_t back, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void msg_show(state_t back, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg_text, sizeof msg_text, fmt, ap);
    va_end(ap);
    msg_back = back;
    state = ST_MSG;
}

// ---------- text document ----------
typedef struct { int32_t s, e, tgt; } link_t;
static uint16_t *dcp;
static uint8_t *dat;
static int32_t dn, dcap;
static int32_t *drows;
static int dnrows, dtop;
static link_t links[MAX_LINKS];
static int nlinks, sellink = -1;
static char linkstr[LINKSTR_CAP];
static int linkstr_n;
static char doc_title[80];
static bool doc_article;
static char find_buf[40], last_find[40];
static int find_anchor;                            // where the screen was when the find box opened
static int fcur;                                   // cursor inside the find text
static bool match_at(int32_t i, int n);
static int32_t sel_a = -1, sel_b = -1;   // characters of the article selected by a tap, copied the moment they are selected
static int32_t find_at = -1;                       // first character of the match you are on, -1 when none
static int find_total, find_idx;                   // matches in the article, and which one you are on
static bool hub_mode;

static void doc_free(void) {
    free(dcp); free(dat); free(drows);
    dcp = NULL; dat = NULL; drows = NULL;
    dn = dcap = 0; dnrows = dtop = 0; nlinks = 0; linkstr_n = 0; sellink = -1; find_at = -1; find_total = find_idx = 0;
}
static bool doc_alloc(size_t n) {
    doc_free();
    dcap = (int32_t)n + 16;
    dcp = heap_caps_malloc((size_t)dcap * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    dat = heap_caps_malloc((size_t)dcap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dcp || !dat) { doc_free(); return false; }
    return true;
}
static void doc_push(uint32_t cp, uint8_t at) {
    if (dn >= dcap) return;
    if (cp == 0xA0) cp = ' ';
    else if (cp == 0x2018 || cp == 0x2019) cp = '\'';
    else if (cp == 0x201C || cp == 0x201D) cp = '"';
    else if (cp == 0x2013 || cp == 0x2014 || cp == 0x2212) cp = '-';
    else if (cp == 0x2022 || cp == 0x25CF) cp = '*';
    else if (cp == 0x2026) cp = '.';
    else if (cp > 0x2FFF) cp = '?';
    dcp[dn] = (uint16_t)cp; dat[dn] = at; dn++;
}
static uint32_t u8next(const unsigned char **pp, const unsigned char *end) {
    const unsigned char *p = *pp;
    uint32_t c = *p++;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (c >= 0xF0) c &= 0x07; else if (c >= 0xE0) c &= 0x0F; else if (c >= 0xC0) c &= 0x1F; else if (c >= 0x80) c = '?';
    while (extra-- > 0 && p < end && (*p & 0xC0) == 0x80) c = (c << 6) | (*p++ & 0x3F);
    *pp = p;
    return c;
}

// Turns the pack and Markdown flavoured text into the document: headings bold, list items, code dim, [[key|text]] links underlined.
static void markup_to_doc(const char *s, size_t n, bool md, bool linkable) {
    const unsigned char *p = (const unsigned char *)s, *end = p + n;
    while (p < end) {
        const unsigned char *le = p;
        while (le < end && *le != '\n') le++;
        uint8_t base = 0;
        const unsigned char *q = p;
        if (md) {
            if (le - q >= 3 && q[0] == '`' && q[1] == '`' && q[2] == '`') { p = le + 1; continue; }
            if (*q == '#') { while (q < le && *q == '#') q++; if (q < le && *q == ' ') q++; base = ATTR_BOLD; }
            else if (le - q >= 2 && (q[0] == '-' || q[0] == '*') && q[1] == ' ') { doc_push('-', 0); doc_push(' ', 0); q += 2; }
            else if (le - q >= 4 && q[0] == ' ' && q[1] == ' ' && q[2] == ' ' && q[3] == ' ') base = ATTR_DIM;
        }
        while (q < le) {
            if (linkable && q + 1 < le && q[0] == '[' && q[1] == '[') {
                const unsigned char *e = q + 2;
                while (e + 1 < le && !(e[0] == ']' && e[1] == ']')) e++;
                if (e + 1 < le) {
                    const unsigned char *bar = q + 2;
                    while (bar < e && *bar != '|') bar++;
                    if (nlinks < MAX_LINKS && linkstr_n + (int)(bar - (q + 2)) + 1 < LINKSTR_CAP) {
                        int ks = linkstr_n;
                        memcpy(linkstr + linkstr_n, q + 2, bar - (q + 2));
                        linkstr_n += (int)(bar - (q + 2));
                        linkstr[linkstr_n++] = 0;
                        links[nlinks].s = dn;
                        const unsigned char *t = bar < e ? bar + 1 : q + 2;
                        while (t < e) doc_push(u8next(&t, e), base | ATTR_UNDERLINE);
                        links[nlinks].e = dn;
                        links[nlinks].tgt = ks;
                        nlinks++;
                    }
                    q = e + 2;
                    continue;
                }
            }
            if (*q == '\t') { for (int i = 0; i < 4; i++) doc_push(' ', base); q++; continue; }
            if (*q == '\r') { q++; continue; }
            doc_push(u8next(&q, le), base);
        }
        doc_push('\n', 0);
        p = le + 1;
    }
}

// Rows of the wrapped document. Pass with out NULL only counts.
static int wrap_pass(int32_t *out) {
    int nr = 0;
    int32_t i = 0;
    while (i < dn) {
        int32_t le = i;
        while (le < dn && dcp[le] != '\n') le++;
        if (le == i) { if (out) out[nr] = i; nr++; }
        else {
            int32_t p = i;
            while (p < le) {
                int32_t end = p + TC;
                if (end >= le) end = le;
                else {
                    int32_t qx = end;
                    while (qx > p && dcp[qx] != ' ') qx--;
                    if (qx > p) end = qx + 1;
                }
                if (out) out[nr] = p;
                nr++;
                p = end;
            }
        }
        i = le + 1;
    }
    return nr;
}
static int row_of_char(int32_t pos) {
    int lo = 0, hi = dnrows - 1;
    while (lo < hi) { int mid = (lo + hi + 1) / 2; if (drows[mid] <= pos) lo = mid; else hi = mid - 1; }
    return lo;
}
static void doc_wrap(void) {
    int32_t keep = dnrows && dtop < dnrows ? drows[dtop] : 0;
    free(drows);
    drows = NULL;
    int nr = wrap_pass(NULL);
    drows = heap_caps_malloc(sizeof(int32_t) * (nr + 2), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!drows) { dnrows = 0; return; }
    dnrows = wrap_pass(drows);
    drows[dnrows] = dn;
    dtop = dnrows ? row_of_char(keep) : 0;
}
static int content_rows(void) { return rows - 1 - last_foot_n; }
static void doc_scroll(int d) {
    int maxtop = dnrows > content_rows() ? dnrows - content_rows() : 0;
    dtop += d;
    if (dtop < 0) dtop = 0;
    if (dtop > maxtop) dtop = maxtop;
}

// ---------- HTML to text ----------
static size_t html_to_md(const char *in, size_t n, char *out, size_t cap) {
    size_t o = 0;
    bool space = false;
    #define OUT(ch) do { if (o + 4 < cap) out[o++] = (ch); } while (0)
    #define NEWLINE(k) do { int have = 0; for (size_t z = o; z > 0 && out[z - 1] == '\n'; z--) have++; for (int z = have; z < (k); z++) OUT('\n'); space = false; } while (0)
    for (size_t i = 0; i < n;) {
        char c = in[i];
        if (c == '<') {
            if (i + 3 < n && !strncmp(in + i, "<!--", 4)) { const char *e = strstr(in + i + 4, "-->"); i = e ? (size_t)(e - in) + 3 : n; continue; }
            size_t j = i + 1;
            bool closing = j < n && in[j] == '/';
            if (closing) j++;
            char name[12] = {0};
            int k = 0;
            while (j < n && isalnum((unsigned char)in[j]) && k < 10) name[k++] = (char)tolower((unsigned char)in[j++]);
            while (j < n && in[j] != '>') j++;
            i = j < n ? j + 1 : n;
            if (!closing && (!strcmp(name, "script") || !strcmp(name, "style"))) {
                char endtag[16];
                snprintf(endtag, sizeof endtag, "</%s", name);
                size_t p = i;
                while (p < n && strncasecmp(in + p, endtag, strlen(endtag)) != 0) p++;
                while (p < n && in[p] != '>') p++;
                i = p < n ? p + 1 : n;
            } else if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
                NEWLINE(2);
                if (!closing) { OUT('#'); OUT(' '); } else NEWLINE(2);
            } else if (!strcmp(name, "li")) { NEWLINE(1); if (!closing) { OUT('-'); OUT(' '); } }
            else if (!strcmp(name, "p") || !strcmp(name, "blockquote") || !strcmp(name, "pre")) NEWLINE(2);
            else if (!strcmp(name, "br") || !strcmp(name, "div") || !strcmp(name, "tr") || !strcmp(name, "ul") || !strcmp(name, "ol") || !strcmp(name, "table") || !strcmp(name, "section")) NEWLINE(1);
            continue;
        }
        if (c == '&') {
            const char *e = memchr(in + i, ';', n - i < 10 ? n - i : 10);
            if (e) {
                char ent[12] = {0};
                memcpy(ent, in + i + 1, (size_t)(e - (in + i + 1)) < 10 ? (size_t)(e - (in + i + 1)) : 10);
                uint32_t cp = 0;
                if (!strcmp(ent, "amp")) cp = '&'; else if (!strcmp(ent, "lt")) cp = '<'; else if (!strcmp(ent, "gt")) cp = '>';
                else if (!strcmp(ent, "quot")) cp = '"'; else if (!strcmp(ent, "apos")) cp = '\''; else if (!strcmp(ent, "nbsp")) cp = ' ';
                else if (ent[0] == '#') cp = (uint32_t)(ent[1] == 'x' || ent[1] == 'X' ? strtol(ent + 2, NULL, 16) : strtol(ent + 1, NULL, 10));
                if (cp) {
                    if (cp < 0x80) OUT((char)cp);
                    else if (cp < 0x800) { OUT((char)(0xC0 | cp >> 6)); OUT((char)(0x80 | (cp & 0x3F))); }
                    else { OUT((char)(0xE0 | cp >> 12)); OUT((char)(0x80 | ((cp >> 6) & 0x3F))); OUT((char)(0x80 | (cp & 0x3F))); }
                    i = (size_t)(e - in) + 1;
                    space = false;
                    continue;
                }
            }
        }
        if (isspace((unsigned char)c)) { if (!space && o && out[o - 1] != '\n') OUT(' '); space = true; i++; continue; }
        OUT(c);
        space = false;
        i++;
    }
    out[o] = 0;
    return o;
    #undef OUT
    #undef NEWLINE
}

static const char *ext_of(const char *name) { const char *d = strrchr(name, '.'); return d ? d + 1 : ""; }

static bool doc_open_file(const char *path, const char *title) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    size_t n = (size_t)st.st_size > MAX_FILE ? MAX_FILE : (size_t)st.st_size;
    char *buf = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return false;
    FILE *f = fopen(path, "rb");
    if (!f) { free(buf); return false; }
    n = fread(buf, 1, n, f);
    fclose(f);
    buf[n] = 0;
    const char *ext = ext_of(path);
    bool html = !strcasecmp(ext, "html") || !strcasecmp(ext, "htm");
    bool md = html || !strcasecmp(ext, "md") || !strcasecmp(ext, "markdown");
    if (html) {
        char *o = heap_caps_malloc(n + 16, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!o) { free(buf); return false; }
        n = html_to_md(buf, n, o, n + 16);
        free(buf);
        buf = o;
    }
    bool ok = doc_alloc(n);
    if (ok) {
        markup_to_doc(buf, n, md, false);
        snprintf(doc_title, sizeof doc_title, "%.78s", title);
        doc_article = false;
        doc_wrap();
    }
    free(buf);
    return ok;
}

// ---------- reference packs ----------
// A pack is a folder made by tools/a11pack.py. Records point at articles by file number (top 16 bits of the offset) and offset.
typedef struct __attribute__((packed)) { uint64_t art_off; uint32_t key_off, clen, rlen; } rec_t;
#define MAX_PACKS 64
typedef struct { char dir[200]; char title[48]; uint32_t n, docs, words; int weight, images; } packinfo_t;
static packinfo_t packs[MAX_PACKS];
static int npacks;
static FILE *f_idx, *f_str, *f_doc, *f_dat;
static int cur_pack = -1, dat_no = -1;

static void pack_close(void) {
    if (f_idx) fclose(f_idx);
    if (f_str) fclose(f_str);
    if (f_doc) fclose(f_doc);
    if (f_dat) fclose(f_dat);
    f_idx = f_str = f_doc = f_dat = NULL;
    cur_pack = -1; dat_no = -1;
}
static FILE *open_in(int pi, const char *name) {
    char p[300];
    snprintf(p, sizeof p, "%.200s/%s", packs[pi].dir, name);
    return fopen(p, "rb");
}
static bool pack_use(int pi) {
    if (pi == cur_pack && f_idx) return true;
    pack_close();
    f_idx = open_in(pi, "titles.idx");
    f_str = open_in(pi, "titles.str");
    f_doc = open_in(pi, "docs.idx");
    if (!f_idx || !f_str) { pack_close(); return false; }
    cur_pack = pi;
    return true;
}
static bool info_read(const char *dir, packinfo_t *pk) {
    char p[300];
    snprintf(p, sizeof p, "%.200s/pack.txt", dir);
    FILE *f = fopen(p, "r");
    if (!f) return false;
    memset(pk, 0, sizeof *pk);
    pk->weight = 100;
    snprintf(pk->dir, sizeof pk->dir, "%.199s", dir);
    char line[160];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "title=", 6)) snprintf(pk->title, sizeof pk->title, "%.47s", line + 6);
        else if (!strncmp(line, "count=", 6)) pk->n = (uint32_t)strtoul(line + 6, NULL, 10);
        else if (!strncmp(line, "docs=", 5)) pk->docs = (uint32_t)strtoul(line + 5, NULL, 10);
        else if (!strncmp(line, "words=", 6)) pk->words = (uint32_t)strtoul(line + 6, NULL, 10);
        else if (!strncmp(line, "weight=", 7)) pk->weight = atoi(line + 7);
        else if (!strncmp(line, "images=", 7)) pk->images = atoi(line + 7);
    }
    fclose(f);
    return pk->n > 0;
}
static void scan_packs(void) {
    npacks = 0;
    DIR *d = opendir(LIB);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && npacks < MAX_PACKS) {
        if (e->d_name[0] == '.') continue;
        char p[300];
        snprintf(p, sizeof p, LIB "/%.100s", e->d_name);
        if (info_read(p, &packs[npacks])) npacks++;
    }
    closedir(d);
}
static bool rec_at(FILE *f, uint32_t i, rec_t *r) {
    return f && fseek(f, (long)i * (long)sizeof(rec_t), SEEK_SET) == 0 && fread(r, sizeof *r, 1, f) == 1;
}
// key and title strings of a record
static void str_get(uint32_t off, char *key, size_t kcap, char *title, size_t tcap) {
    char buf[320] = {0};
    fseek(f_str, (long)off, SEEK_SET);
    size_t n = fread(buf, 1, sizeof buf - 1, f_str);
    buf[n] = 0;
    size_t kl = strnlen(buf, n);
    if (key) snprintf(key, kcap, "%.*s", (int)(kcap - 1), buf);
    if (title) snprintf(title, tcap, "%.*s", (int)(tcap - 1), kl + 1 < n ? buf + kl + 1 : "");
}
// first title record whose key is not smaller than q
static uint32_t lower_bound(const char *q) {
    uint32_t lo = 0, hi = packs[cur_pack].n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        rec_t r; char key[128];
        if (!rec_at(f_idx, mid, &r)) break;
        str_get(r.key_off, key, sizeof key, NULL, 0);
        if (strcmp(key, q) < 0) lo = mid + 1; else hi = mid;
    }
    return lo;
}

typedef struct { uint8_t pack; bool doc; uint32_t idx; int score; char title[76]; } res_t;
#define MAX_RES 20
static res_t res[MAX_RES];
static int nres, rsel;
static char query[60];
static bool search_all, text_mode, results_fresh, searching;
static int qcur;   // cursor position inside the search text
static char last_query[60];   // the last words searched, Up in an empty search box brings it back
static int search_pack;
typedef struct { uint8_t pack; bool doc; uint32_t idx; int top; } hist_t;
static hist_t back[16];
static int nback;
static hist_t cur;

// ----- full text search -----
static const char *STOPW = " the and for with that this from are was were been have has had not but you your can may will all any one two use used using such also into than then there their they them its his her she him who whom what when where which while about over under more most other some each both between per via etc would could should these those only very just like how why out off our own same too ";
static void stem_word(char *w) {
    size_t n = strlen(w);
    if (n > 4 && !strcmp(w + n - 3, "ies")) { w[n - 3] = 'y'; w[n - 2] = 0; }
    else if (n > 4 && (!strcmp(w + n - 4, "ches") || !strcmp(w + n - 4, "shes") || !strcmp(w + n - 4, "sses") || !strcmp(w + n - 3, "xes") || !strcmp(w + n - 3, "zes"))) w[n - 2] = 0;
    else if (n > 3 && w[n - 1] == 's' && strcmp(w + n - 2, "ss") && strcmp(w + n - 2, "us") && strcmp(w + n - 2, "is")) w[n - 1] = 0;
}
typedef struct { char w[28]; int wt; } qterm_t;   // wt in tenths, 10 is a word you typed, 6 a synonym
#define MAX_TERMS 16
static int add_term(qterm_t *t, int n, const char *raw, int len, int wt) {
    char w[28];
    if (len < 2 || len > 24 || n >= MAX_TERMS) return n;
    for (int i = 0; i < len; i++) w[i] = (char)tolower((unsigned char)raw[i]);
    w[len] = 0;
    char padded[32];
    snprintf(padded, sizeof padded, " %s ", w);
    if (strstr(STOPW, padded)) return n;
    stem_word(w);
    for (int i = 0; i < n; i++) if (!strcmp(t[i].w, w)) { if (wt > t[i].wt) t[i].wt = wt; return n; }
    snprintf(t[n].w, sizeof t[n].w, "%s", w);
    t[n].wt = wt;
    return n + 1;
}
// Everyday words and brand names to the words the packs use. File: /library/synonyms.txt, one line like: stomach ache = abdominal pain, belly
static char *syn_text;
static void syn_load(void) {
    if (syn_text) return;
    FILE *f = fopen(LIB "/synonyms.txt", "rb");
    syn_text = calloc(1, 2);
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0 && n < 65536) { free(syn_text); syn_text = calloc(1, (size_t)n + 1); if (syn_text) { size_t got = fread(syn_text, 1, (size_t)n, f); syn_text[got] = 0; } else syn_text = calloc(1, 2); }
    fclose(f);
}
// Different words for the same thing count as the same word when rules are matched: pee, peeing and urine are one, hurts, burns and sore are one.
static const struct { const char *w, *c; } CLS[] = {
    {"hurt","pain"},{"hurts","pain"},{"hurting","pain"},{"pain","pain"},{"pains","pain"},{"painful","pain"},{"ache","pain"},{"aches","pain"},{"aching","pain"},{"achy","pain"},
    {"sore","pain"},{"soreness","pain"},{"burn","pain"},{"burns","pain"},{"burning","pain"},{"sting","pain"},{"stings","pain"},{"stinging","pain"},{"tender","pain"},{"throbbing","pain"},{"cramp","pain"},{"cramps","pain"},{"cramping","pain"},
    {"pee","urine"},{"peeing","urine"},{"urine","urine"},{"urinate","urine"},{"urinating","urine"},{"urination","urine"},{"wee","urine"},{"weeing","urine"},{"piss","urine"},{"pissing","urine"},{"tinkle","urine"},{"bladder","urine"},
    {"poop","stool"},{"pooping","stool"},{"poo","stool"},{"stool","stool"},{"stools","stool"},{"feces","stool"},{"faeces","stool"},{"bowel","stool"},{"bowels","stool"},{"crap","stool"},
    {"vomit","vomit"},{"vomiting","vomit"},{"puke","vomit"},{"puking","vomit"},{"barf","vomit"},{"barfing","vomit"},{"throwing","vomit"},
    {"stomach","belly"},{"tummy","belly"},{"belly","belly"},{"abdomen","belly"},{"abdominal","belly"},{"gut","belly"},{"abdo","belly"},
    {"breath","breath"},{"breathe","breath"},{"breathing","breath"},{"breathless","breath"},{"wheeze","breath"},{"wheezing","breath"},{"gasping","breath"},
    {"bleed","blood"},{"bleeding","blood"},{"bleeds","blood"},{"blood","blood"},{"bloody","blood"},
    {"fever","fever"},{"feverish","fever"},{"temperature","fever"},{"chills","fever"},{"shivering","fever"},
    {"swollen","swell"},{"swelling","swell"},{"swell","swell"},{"swells","swell"},{"puffy","swell"},
    {"itch","itch"},{"itchy","itch"},{"itching","itch"},{"scratchy","itch"},
    {"cough","cough"},{"coughing","cough"},{"hacking","cough"},
    {"dizzy","dizzy"},{"dizziness","dizzy"},{"lightheaded","dizzy"},{"faint","dizzy"},{"fainting","dizzy"},
    {"tired","tired"},{"exhausted","tired"},{"fatigue","tired"},{"weak","tired"},{"weakness","tired"},
    {"rash","rash"},{"spots","rash"},{"hives","rash"},{"bumps","rash"},{"blisters","rash"},
    {"eye","eye"},{"eyes","eye"},{"ear","ear"},{"ears","ear"},{"tooth","tooth"},{"teeth","tooth"},{"throat","throat"},
    {"cut","wound"},{"gash","wound"},{"laceration","wound"},{"wound","wound"},{"slice","wound"},
    {"thirsty","thirst"},{"thirst","thirst"},{"dehydrated","thirst"},{"dehydration","thirst"},
};
static const char *SKIPW = " i my me it its to when a an the is are am was have have has had in on of and or with from for at do does did im ive while if so that this there when whenever feel feels feeling got get gets really very bad ";
static void norm_tok(char *w) {
    for (unsigned i = 0; i < sizeof CLS / sizeof CLS[0]; i++) if (!strcmp(w, CLS[i].w)) { snprintf(w, 28, "%s", CLS[i].c); return; }
    stem_word(w);
}
static bool tok_skip(const char *w) { char p[40]; snprintf(p, sizeof p, " %s ", w); return strstr(SKIPW, p) != NULL; }
static int build_terms(const char *q, qterm_t *t) {
    int n = 0;
    char low[80];
    snprintf(low, sizeof low, "%.79s", q);
    for (char *p = low; *p; p++) *p = (char)tolower((unsigned char)*p);
    char qset[24][28];
    int nq = 0;
    for (const char *p = low; *p;) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        const char *s = p;
        while (*p && isalnum((unsigned char)*p)) p++;
        n = add_term(t, n, s, (int)(p - s), 10);
        char w[28];
        int len = (int)(p - s);
        if (len < 1 || len > 24) continue;
        memcpy(w, s, (size_t)len); w[len] = 0;
        if (tok_skip(w)) continue;
        norm_tok(w);
        bool dup = false;
        for (int i = 0; i < nq; i++) if (!strcmp(qset[i], w)) dup = true;
        if (!dup && nq < 24) snprintf(qset[nq++], 28, "%s", w);
    }
    syn_load();
    for (const char *line = syn_text; line && *line;) {
        const char *eol = strchr(line, '\n');
        size_t ll = eol ? (size_t)(eol - line) : strlen(line);
        const char *eq = memchr(line, '=', ll);
        if (eq && line[0] != '#') {
            // the rule fires when every word on its left side, or its class, is somewhere in what was typed
            int need = 0, found = 0;
            for (const char *p = line; p < eq;) {
                while (p < eq && !isalnum((unsigned char)*p)) p++;
                const char *s = p;
                while (p < eq && isalnum((unsigned char)*p)) p++;
                int len = (int)(p - s);
                if (len < 1 || len > 24) continue;
                char w[28];
                for (int i = 0; i < len; i++) w[i] = (char)tolower((unsigned char)s[i]);
                w[len] = 0;
                if (tok_skip(w)) continue;
                norm_tok(w);
                need++;
                for (int i = 0; i < nq; i++) if (!strcmp(qset[i], w)) { found++; break; }
            }
            if (need > 0 && found == need) {
                for (const char *p = eq + 1; p < line + ll;) {
                    while (p < line + ll && !isalnum((unsigned char)*p)) p++;
                    const char *s = p;
                    while (p < line + ll && isalnum((unsigned char)*p)) p++;
                    n = add_term(t, n, s, (int)(p - s), 8);
                }
            }
        }
        line = eol ? eol + 1 : NULL;
    }
    return n;
}
typedef struct { uint32_t doc1; uint16_t score; uint8_t hits; } acc_t;   // doc1 is the document number plus one, 0 is empty
#define ACC_N 131072
static acc_t *acc;
static void acc_add(uint32_t doc, int w, int is_new_term_hit) {
    uint32_t h = (doc * 2654435761u) & (ACC_N - 1);
    for (int probe = 0; probe < ACC_N; probe++, h = (h + 1) & (ACC_N - 1)) {
        if (acc[h].doc1 == 0) { acc[h].doc1 = doc + 1; acc[h].score = (uint16_t)w; acc[h].hits = (uint8_t)is_new_term_hit; return; }
        if (acc[h].doc1 == doc + 1) { unsigned s = acc[h].score + (unsigned)w; acc[h].score = s > 65000 ? 65000 : (uint16_t)s; acc[h].hits += (uint8_t)is_new_term_hit; return; }
    }
}
static void res_offer(uint8_t pi, uint32_t doc, int score, const char *title) {
    int at = -1;
    if (nres < MAX_RES) at = nres++;
    else if (score > res[MAX_RES - 1].score) at = MAX_RES - 1;
    if (at < 0) return;
    res[at].pack = pi; res[at].doc = true; res[at].idx = doc; res[at].score = score;
    snprintf(res[at].title, sizeof res[at].title, "%.75s", title);
    while (at > 0 && res[at].score > res[at - 1].score) { res_t t = res[at]; res[at] = res[at - 1]; res[at - 1] = t; at--; }
}
// In Search everything, a pack whose title has a word you typed ranks above the big general packs. "honda fit" finds the Honda Fit manual first.
#define TITLE_BONUS 150
static int title_bonus(int pi) {
    char low[80], title[96];
    snprintf(low, sizeof low, "%.79s", query);
    snprintf(title, sizeof title, " %.90s ", packs[pi].title);
    for (char *c = low; *c; c++) *c = (char)tolower((unsigned char)*c);
    for (char *c = title; *c; c++) *c = isalnum((unsigned char)*c) ? (char)tolower((unsigned char)*c) : ' ';
    int hits = 0;
    for (const char *q = low; *q;) {
        while (*q && !isalnum((unsigned char)*q)) q++;
        char w[28];
        int n = 0;
        while (*q && isalnum((unsigned char)*q)) { if (n < 24) w[n++] = *q; q++; }
        w[n] = 0;
        if (n < 3 || tok_skip(w)) continue;
        char pat[32];
        snprintf(pat, sizeof pat, " %s ", w);
        if (strstr(title, pat)) hits++;
    }
    return hits * TITLE_BONUS;
}
static void text_search_pack(int pi, const qterm_t *t, int nt) {
    if (!packs[pi].words) return;
    FILE *fi = open_in(pi, "words.idx"), *fs = open_in(pi, "words.str"), *fp = open_in(pi, "post.dat");
    if (!fi || !fs || !fp) { if (fi) fclose(fi); if (fs) fclose(fs); if (fp) fclose(fp); return; }
    memset(acc, 0, sizeof(acc_t) * ACC_N);
    for (int k = 0; k < nt; k++) {
        uint32_t lo = 0, hi = packs[pi].words;
        uint32_t rec[3] = {0};
        bool found = false;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            char w[40] = {0};
            if (fseek(fi, (long)mid * 12, SEEK_SET) || fread(rec, 12, 1, fi) != 1) break;
            fseek(fs, (long)rec[0], SEEK_SET);
            size_t got = fread(w, 1, sizeof w - 1, fs);
            w[got] = 0;
            int c = strcmp(w, t[k].w);
            if (c == 0) { found = true; break; }
            if (c < 0) lo = mid + 1; else hi = mid;
        }
        if (!found) continue;
        uint32_t cnt = rec[2] > 2000 ? 2000 : rec[2];
        static uint32_t post[2000];
        fseek(fp, (long)rec[1], SEEK_SET);
        cnt = (uint32_t)fread(post, 4, cnt, fp);
        for (uint32_t i = 0; i < cnt; i++) acc_add(post[i] >> 8, (int)(post[i] & 255) * t[k].wt / 10, 1);
    }
    fclose(fi); fclose(fs); fclose(fp);
    // the best few of this pack, matching more of the words ranks higher
    int tbonus = search_all ? title_bonus(pi) : 0;
    static int bestscore[8];
    static uint32_t bestdoc[8];
    int nb = 0;
    for (int h = 0; h < ACC_N; h++) {
        if (!acc[h].doc1) continue;
        int sc = (acc[h].score + (acc[h].hits > 1 ? (acc[h].hits - 1) * 60 : 0)) * packs[pi].weight / 100 + tbonus;
        int at = -1;
        if (nb < 8) at = nb++;
        else if (sc > bestscore[7]) at = 7;
        if (at < 0) continue;
        bestscore[at] = sc; bestdoc[at] = acc[h].doc1 - 1;
        while (at > 0 && bestscore[at] > bestscore[at - 1]) { int ts = bestscore[at]; bestscore[at] = bestscore[at - 1]; bestscore[at - 1] = ts; uint32_t td = bestdoc[at]; bestdoc[at] = bestdoc[at - 1]; bestdoc[at - 1] = td; at--; }
    }
    if (nb == 0) return;
    if (!pack_use(pi) || !f_doc) return;
    for (int i = 0; i < nb; i++) {
        rec_t r; char title[80] = "";
        if (rec_at(f_doc, bestdoc[i], &r)) str_get(r.key_off, NULL, 0, title, sizeof title);
        res_offer((uint8_t)pi, bestdoc[i], bestscore[i], title);
    }
}
static void text_search(void) {
    nres = 0; rsel = 0;
    qterm_t t[MAX_TERMS];
    int nt = build_terms(query, t);
    if (nt == 0) return;
    if (!acc) acc = heap_caps_malloc(sizeof(acc_t) * ACC_N, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!acc) return;
    if (search_all) for (int pi = 0; pi < npacks; pi++) text_search_pack(pi, t, nt);
    else text_search_pack(search_pack, t, nt);
}

// Raw deflate to memory. The decompressor state is about 10 KB, so it lives on the heap, the main task stack is not that big.
static size_t inflate_raw(void *out, size_t out_len, const void *in, size_t in_len) {
    tinfl_decompressor *d = heap_caps_malloc(sizeof *d, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!d) return (size_t)-1;
    tinfl_init(d);
    size_t in_n = in_len, out_n = out_len;
    tinfl_status st = tinfl_decompress(d, (const mz_uint8 *)in, &in_n, (mz_uint8 *)out, (mz_uint8 *)out, &out_n, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(d);
    return st == TINFL_STATUS_DONE ? out_n : (size_t)-1;
}

static bool article_open(uint8_t pi, bool doc, uint32_t idx) {
    if (!pack_use(pi)) return false;
    rec_t r;
    if (!rec_at(doc ? f_doc : f_idx, idx, &r) || r.rlen > 2u * 1024 * 1024 || r.clen > 2u * 1024 * 1024) return false;
    int fileno = (int)(r.art_off >> 48);
    long off = (long)(r.art_off & 0xFFFFFFFFFFFFull);
    if (fileno != dat_no) {
        if (f_dat) fclose(f_dat);
        char name[24];
        snprintf(name, sizeof name, "articles.%03d", fileno);
        f_dat = open_in(pi, name);
        dat_no = f_dat ? fileno : -1;
        if (!f_dat) return false;
    }
    uint8_t *comp = heap_caps_malloc(r.clen + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *text = heap_caps_malloc(r.rlen + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool ok = comp && text && fseek(f_dat, off, SEEK_SET) == 0 && fread(comp, 1, r.clen, f_dat) == r.clen;
    if (ok) ok = inflate_raw(text, r.rlen, comp, r.clen) == r.rlen;
    if (ok) {
        text[r.rlen] = 0;
        ok = doc_alloc(r.rlen);
        if (ok) {
            markup_to_doc(text, r.rlen, true, true);
            str_get(r.key_off, NULL, 0, doc_title, sizeof doc_title);
            doc_article = true;
            doc_wrap();
            cur.pack = pi; cur.doc = doc; cur.idx = idx;
        }
    }
    free(comp);
    free(text);
    return ok;
}

// ----- pictures inside articles -----
static int img_link = -1;   // which link of the article showed the current picture
// The picture on screen. It starts fitted to the screen. Zoom steps go to bigger, and the arrows or a finger drag move around it. Large pictures,
// like pages of a manual, are decoded at the size the view needs, so the fitted view is quick and a zoomed view is sharp.
static bool show_image(int pi, uint32_t n) {
    if (pi < 0 || pi >= npacks || (int)n >= packs[pi].images) return false;
    FILE *fi = open_in(pi, "images.idx"), *fd = open_in(pi, "images.dat");
    uint8_t rec[12];
    bool ok = fi && fd && fseek(fi, (long)n * 12, SEEK_SET) == 0 && fread(rec, 12, 1, fi) == 1;
    uint64_t off = 0; uint32_t len = 0;
    if (ok) { memcpy(&off, rec, 8); memcpy(&len, rec + 8, 4); ok = len > 0 && len < 3000000 && fseek(fd, (long)off, SEEK_SET) == 0; }
    uint8_t *jpg = ok ? heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    ok = jpg && fread(jpg, 1, len, fd) == len;
    if (fi) fclose(fi);
    if (fd) fclose(fd);
    if (!ok) { free(jpg); return false; }
    return imgview_open(jpg, len);
}
static int pic_link_step(int from, int dir) {   // the next link that is a picture, in the article order
    for (int i = from + dir; i >= 0 && i < nlinks; i += dir) if (!strncmp(linkstr + links[i].tgt, "img:", 4)) return i;
    return from;
}

// Follows a link by its key, in the pack that is open. Exact match only.
static bool follow_link(int li) {
    const char *key = linkstr + links[li].tgt;
    if (!strncmp(key, "img:", 4)) {   // a picture, shown full screen over the article
        if (!show_image(cur.pack, (uint32_t)atoi(key + 4))) return false;
        img_link = li;
        state = ST_IMAGE;
        return true;
    }
    int target = cur.pack;
    const char *gt = strchr(key, '>');   // packfolder>key reaches into another pack
    if (gt) {
        char name[48];
        snprintf(name, sizeof name, "%.*s", (int)(gt - key) < 47 ? (int)(gt - key) : 47, key);
        key = gt + 1;
        target = -1;
        for (int k = 0; k < npacks; k++) {
            const char *base = strrchr(packs[k].dir, '/');
            if (base && !strcmp(base + 1, name)) target = k;
        }
        if (target < 0) return false;
    }
    if (!pack_use((uint8_t)target)) return false;
    uint32_t i = lower_bound(key);
    if (i >= packs[cur_pack].n) return false;
    rec_t r; char k[128];
    if (!rec_at(f_idx, i, &r)) return false;
    str_get(r.key_off, k, sizeof k, NULL, 0);
    if (strcmp(k, key) != 0) return false;
    hist_t here = cur; here.top = dtop;
    if (nback < 16) back[nback++] = here;
    return article_open((uint8_t)target, false, i);
}

// ---------- browser ----------
typedef struct { char name[64]; bool dir, pack, all, hub, files, term, loader; long size; } ent_t;
static const char *(*files_handler)(void);
void reader_set_files_handler(const char *(*h)(void)) { files_handler = h; }
static bool doc_from_files;   // the document on screen came from the file manager, closing it goes back there
static void run_files_row(void);
static ent_t *ents;
static int nents, bsel;
static char cwd[256] = LIB;

static int ent_cmp(const void *a, const void *b) {
    const ent_t *x = a, *y = b;
    if (x->all != y->all) return x->all ? -1 : 1;
    if (x->files != y->files) return x->files ? -1 : 1;   // Files comes right under Search
    if (x->term != y->term) return x->term ? -1 : 1;     // and the way back to the terminal right under Files
    if (x->loader != y->loader) return x->loader ? -1 : 1;
    if (x->hub != y->hub) return x->hub ? -1 : 1;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}
static void browse(const char *dir) {
    list_gen++;
    if (!ents) ents = heap_caps_calloc(MAX_ENT, sizeof(ent_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    snprintf(cwd, sizeof cwd, "%.255s", dir);
    nents = 0;
    bsel = 0;
    DIR *d = opendir(dir);
    if (!d && strcmp(dir, LIB)) return;   // the top of the library still gets its own rows when there is no card, or it cannot be read
    struct dirent *e;
    while (d && (e = readdir(d)) && nents < MAX_ENT) {
        if (e->d_name[0] == '.') continue;
        char p[400];
        snprintf(p, sizeof p, "%.255s/%.100s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        ent_t *x = &ents[nents];
        memset(x, 0, sizeof *x);   // a slot used before must not keep its old search or topics flag
        snprintf(x->name, sizeof x->name, "%.63s", e->d_name);
        x->dir = S_ISDIR(st.st_mode);
        x->size = (long)st.st_size;
        x->pack = false;
        if (x->dir) {
            char pp[480];
            snprintf(pp, sizeof pp, "%.399s/pack.txt", p);
            struct stat s2;
            x->pack = stat(pp, &s2) == 0;
        } else {
            if (!strcasecmp(x->name, "synonyms.txt")) continue;
            const char *ex = ext_of(x->name);
            if (strcasecmp(ex, "txt") && strcasecmp(ex, "md") && strcasecmp(ex, "markdown") && strcasecmp(ex, "html") && strcasecmp(ex, "htm") && strcasecmp(ex, "text")) continue;
        }
        nents++;
    }
    if (d) closedir(d);
    if (!strcmp(dir, LIB)) {
        scan_packs();
        int searchable = 0;
        for (int i = 0; i < npacks; i++) if (packs[i].words) searchable++;
        for (int i = 0; i < npacks; i++) {
            const char *base = strrchr(packs[i].dir, '/');
            if (!base || strcmp(base + 1, "topics")) continue;
            for (int j = 0; j < nents; j++) if (ents[j].pack && !strcmp(ents[j].name, "topics")) { ents[j] = ents[--nents]; break; }   // the folder itself is not listed twice
            if (nents < MAX_ENT) {
                memset(&ents[nents], 0, sizeof(ent_t));
                snprintf(ents[nents].name, sizeof ents[nents].name, "Quick topics, first aid and survival");
                ents[nents].dir = true; ents[nents].hub = true;
                nents++;
            }
        }
        if (searchable && nents < MAX_ENT) {
            memset(&ents[nents], 0, sizeof(ent_t));
            snprintf(ents[nents].name, sizeof ents[nents].name, "Search everything, %d packs", searchable);
            ents[nents].dir = true; ents[nents].all = true;
            nents++;
        }
        if (nents < MAX_ENT) {   // the loader and the terminal are always there, they work without a card
            memset(&ents[nents], 0, sizeof(ent_t));
            snprintf(ents[nents].name, sizeof ents[nents].name, "Open the loader (restarts)");
            ents[nents].dir = true; ents[nents].loader = true;
            nents++;
        }
        if (nents < MAX_ENT) {
            memset(&ents[nents], 0, sizeof(ent_t));
            snprintf(ents[nents].name, sizeof ents[nents].name, "Back to the terminal (restarts)");
            ents[nents].dir = true; ents[nents].term = true;
            nents++;
        }
        if (files_handler && sdcard_ok() && nents < MAX_ENT) {   // Files only when there is a card to look at
            memset(&ents[nents], 0, sizeof(ent_t));
            snprintf(ents[nents].name, sizeof ents[nents].name, "Files, pictures, sounds and the whole card");
            ents[nents].dir = true; ents[nents].files = true;
            nents++;
        }
    }
    qsort(ents, nents, sizeof(ent_t), ent_cmp);
}
static void go_up(void) {
    if (!strcmp(cwd, LIB)) return;   // the top of the library, only the menu leaves to the terminal
    char *s = strrchr(cwd, '/');
    if (s && s > cwd) { *s = 0; browse(cwd); }
}

static void to_loader(void) {
    esp_ota_set_boot_partition(esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL));
    esp_restart();
}

// ---------- drawing ----------
static void draw_browse(void) {
    clear();
    char b[300];
    snprintf(b, sizeof b, "Library  %.200s", cwd + strlen(SDCARD_MOUNT));
    header(b);
    const char *hint = "Enter open   Esc up   GUI menu   t theme";
    last_foot_n = foot_n_for(hint);
    int visible = rows - 1 - last_foot_n;
    fix_top(bsel, &btop, visible, nents);
    int top = btop;
    display_set_scrollbar(top, visible, nents);
    if (!sdcard_ok()) put_flow(2, 1, "No card, or it could not be read. FAT32 and exFAT cards work.", 0);
    else if (nents == 0) { int r = put_flow(2, 1, "No library on this card yet.", ATTR_BOLD); put_flow(r + 1, 1, "Copy the library folder onto the card from a computer, or send it over Wi-Fi from Mode 1. GUI opens the menu.", 0); }
    for (int i = 0; i < visible && top + i < nents; i++) {
        ent_t *x = &ents[top + i];
        if (x->hub) snprintf(b, sizeof b, " [topics] %.200s", x->name);
        else if (x->all) snprintf(b, sizeof b, " [search] %.200s", x->name);
        else if (x->files) snprintf(b, sizeof b, " [files]  %.200s", x->name);
        else if (x->term || x->loader) snprintf(b, sizeof b, " [exit]   %.200s", x->name);
        else if (x->pack) snprintf(b, sizeof b, " [pack] %.200s", x->name);
        else if (x->dir) snprintf(b, sizeof b, " [dir]  %.200s", x->name);
        else snprintf(b, sizeof b, " %.200s  (%ld KB)", x->name, (x->size + 1023) / 1024);
        int off = top + i == bsel ? marquee_off(list_gen * 4096 + bsel, (int)strlen(b), TC) : 0;
        put(1 + i, 0, b + off, 0);
        if (top + i == bsel) rowattr(1 + i, ATTR_REVERSE);
    }
    footer(hint);
}
static void draw_text(void) {
    clear();
    header(doc_title);
    if (last_find[0] && find_total > 0) {
        char fb[24];
        snprintf(fb, sizeof fb, " %d of %d ", find_idx, find_total);
        put(0, TC - (int)strlen(fb), fb, ATTR_BOLD | ATTR_REVERSE);
    }
    char b[160];
    snprintf(b, sizeof b, "100%%  %s", doc_article ? "Tab link   Enter follow   Backspace back   / find" : "/ find   n next   Esc back");
    last_foot_n = foot_n_for(b);
    int vis = content_rows();
    // every match of the find text on screen is highlighted, the one you are on is bold and underlined
    static uint8_t hl[8192];
    int32_t rs = drows[dtop], re = drows[dtop + vis < dnrows ? dtop + vis : dnrows];
    int fl = (int)strlen(last_find);
    bool use_hl = fl > 0 && re - rs <= (int32_t)sizeof hl;
    if (use_hl) {
        memset(hl, 0, (size_t)(re - rs));
        for (int32_t i = rs - fl + 1 < 0 ? 0 : rs - fl + 1; i < re; i++)
            if (match_at(i, fl)) for (int j = 0; j < fl; j++) { int32_t k = i + j; if (k >= rs && k < re) hl[k - rs] = i == find_at ? 2 : 1; }
    }
    for (int r = 0; r < vis && dtop + r < dnrows; r++) {
        int32_t s = drows[dtop + r], e = drows[dtop + r + 1];
        for (int32_t k = s, c = 0; k < e && c < TC; k++) {
            if (dcp[k] == '\n') break;
            terminal_cell_t *cell = &scr[(1 + r) * cols + c++];
            cell->cp = dcp[k];
            cell->attrs = dat[k];
            if (sellink >= 0 && k >= links[sellink].s && k < links[sellink].e) cell->attrs |= ATTR_REVERSE;
            if (k >= sel_a && k < sel_b) cell->attrs |= ATTR_REVERSE;
            if (use_hl && hl[k - rs]) cell->attrs |= hl[k - rs] == 2 ? (ATTR_REVERSE | ATTR_BOLD | ATTR_UNDERLINE) : ATTR_REVERSE;
        }
    }
    display_set_scrollbar(dtop, vis, dnrows);
    int last = dtop + vis < dnrows ? dtop + vis : dnrows;
    snprintf(b, sizeof b, "%d%%  %s", dnrows ? last * 100 / dnrows : 100, doc_article ? "Tab link   Enter follow   Backspace back   / find" : "/ find   n next   Esc back");
    footer(b);
}
static void draw_find(void) {
    draw_text();
    for (int c = 0; c < cols; c++) { scr[(rows - 1) * cols + c] = (terminal_cell_t){' ', DISPLAY_DEFAULT_FG, DISPLAY_DEFAULT_BG, ATTR_REVERSE}; }
    char b[80];
    snprintf(b, sizeof b, "Find: %s", find_buf);
    put(rows - 1, 1, b, ATTR_REVERSE);
    if (find_buf[0] && find_total == 0) put(rows - 1, TC - 9, " No match ", ATTR_REVERSE | ATTR_BOLD);
    if (fcur > (int)strlen(find_buf)) fcur = (int)strlen(find_buf);
    display_set_cursor(7 + fcur, rows - 1, CURSOR_BLOCK);
}
// Inside a pack the search box sits on top and every title is listed below it. Typing jumps the list to those letters and Enter
// searches the words. Search everything has no title list, it only shows results.
static bool pack_browse_view(void) { return !search_all && !(text_mode && results_fresh); }
static void title_at(int i, char *out, size_t cap) {
    rec_t r; char key[128];
    out[0] = 0;
    if (rec_at(f_idx, (uint32_t)i, &r)) str_get(r.key_off, key, sizeof key, out, cap);
}
static void draw_search(void) {
    clear();
    char b[200];
    if (search_all) snprintf(b, sizeof b, "Search everything, %d packs", npacks);
    else snprintf(b, sizeof b, "%.50s, %u articles", packs[search_pack].title, (unsigned)packs[search_pack].docs);
    header(b);
    snprintf(b, sizeof b, " Search: %s", query);
    put(2, 0, b, ATTR_BOLD);
    bool bv = pack_browse_view();
    last_foot_n = 0;   // no bottom line on the search screen
    int first = bv ? 3 : 4;
    if (searching) { put(3, 1, " Searching, please wait ... ", ATTR_REVERSE | ATTR_BOLD); first = 5; }
    int rvis = rows - last_foot_n - first;
    if (rvis < 1) rvis = 1;
    s_first = first; s_rvis = rvis;
    if (bv) {
        if (cur_pack != search_pack) pack_use(search_pack);
        int n = (int)packs[search_pack].n;
        if (tsel >= n) tsel = n ? n - 1 : 0;
        fix_top(tsel, &ttop, rvis, n);
        display_set_scrollbar(ttop, rvis, n);
        for (int j = 0; j < rvis && ttop + j < n; j++) {
            char t[76];
            title_at(ttop + j, t, sizeof t);
            snprintf(b, sizeof b, " %.150s", t);
            int off = ttop + j == tsel ? marquee_off(0x10000 + tsel, (int)strlen(b), TC) : 0;
            put(first + j, 0, b + off, 0);
            if (ttop + j == tsel && (engaged || !query[0])) rowattr(first + j, ATTR_REVERSE);
        }
    } else {
        fix_top(rsel, &rtop, rvis, nres);
        display_set_scrollbar(rtop, rvis, nres);
        for (int j = 0; j < rvis && rtop + j < nres; j++) {
            int i = rtop + j;
            snprintf(b, sizeof b, " %.150s", res[i].title);
            int tl = 0;
            if (search_all) { tl = (int)strlen(packs[res[i].pack].title); if (tl > 16) tl = 16; }
            int off = i == rsel ? marquee_off(0x20000 + rsel, (int)strlen(b), TC - (tl ? tl + 2 : 0)) : 0;
            put(first + j, 0, b + off, 0);
            if (search_all) {
                const char *tag = packs[res[i].pack].title;
                char tb[20];
                snprintf(tb, sizeof tb, "%.16s", tag);
                if (cols > 40) put(first + j, TC - tl - 1, tb, ATTR_DIM);
            }
            if (i == rsel) rowattr(first + j, ATTR_REVERSE);
        }
        if (nres == 0 && query[0]) put_flow(first + 1, 1, results_fresh ? "Nothing found. Try other words." : "Press Enter to search.", ATTR_DIM);
        if (!query[0]) put_flow(first + 1, 1, "Type symptoms, a medicine or a question, then Enter.", ATTR_DIM);
    }
    if (qcur > (int)strlen(query)) qcur = (int)strlen(query);
    display_set_cursor(9 + qcur, 2, CURSOR_BLOCK);
}
static void draw_msg(void) {
    clear();
    display_set_scrollbar(0, 0, 0);
    header("Reader");
    put_flow(2, 1, msg_text, 0);
    footer("Any key continues");
}
static void draw_screen(void) {
    switch (state) {
        case ST_BROWSE: draw_browse(); break;
        case ST_TEXT: draw_text(); break;
        case ST_FIND: draw_find(); break;
        case ST_SEARCH: draw_search(); break;
        case ST_MSG: draw_msg(); break;
        case ST_IMAGE: break;
        case ST_MENU: draw_menu(); break;
    }
}
static void draw(void) {
    if (!back_buf || state == ST_IMAGE) { return; }
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
// ---------- copy and paste ----------
static int64_t sel_clear_at;
static char last_tap_text[CLIP_MAX_LEN + 1];
static bool word_cp(uint16_t cp) {
    if (cp <= 0x20) return false;
    switch (cp) { case '(': case ')': case '[': case ']': case '{': case '}': case '<': case '>': case '"': case '\'': case '`': case ',': case ';': case '|': return false; }
    return true;
}
static int32_t row_end(int r) {   // end of a row without its line break
    int32_t e = drows[r + 1];
    while (e > drows[r] && (dcp[e - 1] == '\n' || dcp[e - 1] == ' ')) e--;
    return e;
}
static bool row_blank(int r) { return row_end(r) <= drows[r]; }
// Text of characters a up to b of the article, as UTF-8.
static size_t range_text(int32_t a, int32_t b, char *out, size_t cap) {
    size_t n = 0;
    for (int32_t i = a; i < b && n + 4 < cap; i++) {
        uint16_t cp = dcp[i];
        if (cp < 0x80) out[n++] = (char)cp;
        else if (cp < 0x800) { out[n++] = (char)(0xC0 | (cp >> 6)); out[n++] = (char)(0x80 | (cp & 0x3F)); }
        else { out[n++] = (char)(0xE0 | (cp >> 12)); out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[n++] = (char)(0x80 | (cp & 0x3F)); }
    }
    while (n && (out[n - 1] == '\n' || out[n - 1] == ' ')) n--;
    out[n] = 0;
    return n;
}
// unit 2 word, 3 line, 4 paragraph, 5 everything on the screen. Selects it and returns false when there is no text there.
static bool select_unit(int row, int col, int unit) {
    if (row < 0 || row >= dnrows) return false;
    int32_t a = -1, b = -1;
    if (unit == 2) {
        int32_t s0 = drows[row], e0 = row_end(row), k = s0 + col;
        if (k >= e0 || !word_cp(dcp[k])) return false;
        a = k; b = k + 1;
        while (a > s0 && word_cp(dcp[a - 1])) a--;
        while (b < e0 && word_cp(dcp[b])) b++;
    } else if (unit == 3) {
        if (row_blank(row)) return false;
        a = drows[row]; b = row_end(row);
    } else if (unit == 4) {
        if (row_blank(row)) return false;
        int r0 = row, r1 = row;
        while (r0 > 0 && !row_blank(r0 - 1)) r0--;
        while (r1 + 1 < dnrows && !row_blank(r1 + 1)) r1++;
        a = drows[r0]; b = row_end(r1);
    } else {
        int r0 = dtop, r1 = dtop + content_rows() - 1;
        if (r1 >= dnrows) r1 = dnrows - 1;
        while (r0 <= r1 && row_blank(r0)) r0++;
        while (r1 >= r0 && row_blank(r1)) r1--;
        if (r0 > r1) return false;
        a = drows[r0]; b = row_end(r1);
    }
    static char text[CLIP_MAX_LEN + 1];
    if (!range_text(a, b, text, sizeof text)) return false;
    sel_a = a; sel_b = b;
    // a run of taps upgrades word to line to paragraph, so the smaller copy made a moment ago is replaced, not left behind
    if (last_tap_text[0]) { int i = clipboard_find(last_tap_text); if (i >= 0) clipboard_remove(i); }
    clipboard_add(text);
    snprintf(last_tap_text, sizeof last_tap_text, "%s", text);
    sel_clear_at = esp_timer_get_time() + 1500000;
    return true;
}
// Paste, copy and cut for the search and find text boxes. Paste takes the newest clipboard entry as one line.
static bool field_clip(uint16_t code, char *buf, size_t cap, int *cur) {
    size_t n = strlen(buf);
    if (code == 25) {   // Ctrl V
        if (!clipboard_count()) return true;
        const char *t = clipboard_get(0);
        for (; *t && n + 1 < cap; t++) {
            unsigned char ch = (unsigned char)*t;
            if (ch == '\n' || ch == '\t' || ch == '\r') ch = ' ';
            if (ch < 32 || ch > 126) continue;
            memmove(buf + *cur + 1, buf + *cur, n - (size_t)*cur + 1);
            buf[(*cur)++] = (char)ch;
            n++;
        }
        return true;
    }
    if (code == 6 || code == 27) {   // Ctrl C copies the text, Ctrl X also clears it
        if (n) clipboard_add(buf);
        if (code == 27) { buf[0] = 0; *cur = 0; }
        return true;
    }
    return false;
}
static bool match_at(int32_t i, int n) {
    if (i < 0 || i + n > dn) return false;
    for (int k = 0; k < n; k++) if (tolower(dcp[i + k] < 128 ? (int)dcp[i + k] : '?') != tolower((unsigned char)last_find[k])) return false;
    return true;
}
// Counts every match once, and works out which one find_at is.
static void find_recount(void) {
    int n = (int)strlen(last_find);
    find_total = 0; find_idx = 0;
    if (n == 0) return;
    for (int32_t i = 0; i + n <= dn; i++) if (match_at(i, n)) { find_total++; if (i <= find_at) find_idx = find_total; }
}
// Moves to the next (dir 1) or previous (dir -1) match, wrapping around the article. The first search starts at the top of the screen.
static bool find_step(int dir, bool from_view) {
    int n = (int)strlen(last_find);
    if (n == 0 || dnrows == 0) return false;
    int32_t from = from_view || find_at < 0 ? drows[dtop] : find_at + dir;
    if (from_view && dir < 0) from = drows[dtop] - 1;
    for (int pass = 0; pass < 2; pass++) {
        if (dir > 0) {
            int32_t lo = pass == 0 ? from : 0, hi = pass == 0 ? dn : from;
            for (int32_t i = lo < 0 ? 0 : lo; i < hi && i + n <= dn; i++) if (match_at(i, n)) { find_at = i; goto found; }
        } else {
            int32_t hi = pass == 0 ? from : dn - n, lo = pass == 0 ? 0 : from;
            if (hi > dn - n) hi = dn - n;
            for (int32_t i = hi; i >= lo && i >= 0; i--) if (match_at(i, n)) { find_at = i; goto found; }
        }
    }
    return false;
found:
    {
        int r = row_of_char(find_at);
        if (r < dtop || r >= dtop + content_rows()) { dtop = r > 2 ? r - 2 : 0; doc_scroll(0); }
    }
    find_recount();
    return true;
}
static int next_link(int dir) {
    if (nlinks == 0) return -1;
    int32_t from = drows[dtop], to = drows[dtop + content_rows() < dnrows ? dtop + content_rows() : dnrows];
    int cand = -1;
    if (sellink < 0 || links[sellink].s < from || links[sellink].s >= to) {   // none chosen on this screen yet
        for (int i = 0; i < nlinks; i++) if (links[i].s >= from && links[i].s < to) { cand = i; if (dir > 0) break; }
        return cand;
    }
    int n = sellink + dir;
    return n >= 0 && n < nlinks ? n : sellink;
}
static void show_link(int i) {
    sellink = i;
    if (i < 0) return;
    int r = row_of_char(links[i].s);
    if (r < dtop || r >= dtop + content_rows()) { dtop = r > 2 ? r - 2 : 0; doc_scroll(0); }
}
static void key_text(uint16_t code) {
    int page = content_rows() - 1;
    char c = a11_hid_to_char(code, shift, false);
    if (code == HID_UP) doc_scroll(alt ? -page : -1);
    else if (code == HID_DOWN) doc_scroll(alt ? page : 1);
    else if (code == HID_PGUP) doc_scroll(-page);
    else if (code == HID_SPACE && shift) doc_scroll(-page);
    else if (code == HID_PGDN || code == HID_SPACE) doc_scroll(page);
    else if (code == HID_HOME || (alt && code == HID_LEFT)) dtop = 0;
    else if (code == HID_END || (alt && code == HID_RIGHT)) doc_scroll(dnrows);
    else if (c == '/') { find_buf[0] = 0; fcur = 0; find_anchor = dtop; state = ST_FIND; }
    else if (c == 'n' || c == '.' || c == '>') { if (!find_step(1, false)) msg_show(ST_TEXT, "Not found."); }
    else if (c == 'p' || c == 'N' || c == ',' || c == '<') { if (!find_step(-1, false)) msg_show(ST_TEXT, "Not found."); }
    else if (code == HID_TAB && doc_article) show_link(next_link(shift ? -1 : 1));
    else if (code == HID_ENTER && doc_article && sellink >= 0) {
        if (!follow_link(sellink)) msg_show(ST_TEXT, "That page is not in this pack.");
    }
    else if (code == HID_BACKSPACE && doc_article) {
        if (nback > 0) { nback--; hist_t h = back[nback]; if (article_open(h.pack, h.doc, h.idx)) { dtop = h.top; doc_scroll(0); } }
        else state = hub_mode ? ST_BROWSE : ST_SEARCH;
    }
    else if (code == HID_ESC) {
        if (doc_article && !hub_mode) state = ST_SEARCH;
        else state = ST_BROWSE;
        if (state == ST_BROWSE) { doc_free(); hub_mode = false; if (doc_from_files) { doc_from_files = false; run_files_row(); } }
    }
    else if (c == 't') apply_theme(theme_idx + 1);
}
// The find text works as you type: after every key the article jumps to the first match from where you started.
static void live_find(void) {
    snprintf(last_find, sizeof last_find, "%s", find_buf);
    find_at = -1; find_total = find_idx = 0;
    dtop = find_anchor;
    doc_scroll(0);
    if (last_find[0]) find_step(1, true);
}
static void key_find(uint16_t code) {
    char c = a11_hid_to_char(code, shift, false);
    size_t n = strlen(find_buf);
    if (fcur > (int)n) fcur = (int)n;
    if (ctrl) { if (field_clip(code, find_buf, sizeof find_buf, &fcur)) live_find(); return; }
    if (code == HID_ESC) {   // cancels: the screen goes back to where it was and the highlights go
        last_find[0] = 0; find_at = -1; find_total = find_idx = 0;
        dtop = find_anchor; doc_scroll(0);
        state = ST_TEXT;
    }
    else if (code == HID_ENTER) state = ST_TEXT;   // keeps the highlights, , and . or n and p move between matches
    else if (code == HID_DOWN) find_step(1, false);
    else if (code == HID_UP) find_step(-1, false);
    else if (code == HID_LEFT) { if (alt) fcur = 0; else if (fcur > 0) fcur--; }
    else if (code == HID_RIGHT) { if (alt) fcur = (int)n; else if (fcur < (int)n) fcur++; }
    else if (code == HID_BACKSPACE) { if (fcur > 0) { memmove(find_buf + fcur - 1, find_buf + fcur, n - (size_t)fcur + 1); fcur--; live_find(); } }
    else if (c >= 32 && c < 127 && n < sizeof find_buf - 1) { memmove(find_buf + fcur + 1, find_buf + fcur, n - (size_t)fcur + 1); find_buf[fcur++] = c; live_find(); }
}
// The typed text changed: the old results are gone, and inside a pack the title list jumps to the first title at or after those letters.
static void query_changed(void) {
    nres = 0; rsel = 0; results_fresh = false; engaged = false;
    if (!search_all && pack_use(search_pack)) {
        char q[60];
        size_t ql = strlen(query);
        for (size_t i = 0; i <= ql; i++) q[i] = (char)tolower((unsigned char)query[i]);
        tsel = (int)lower_bound(q);
        if (tsel >= (int)packs[search_pack].n) tsel = (int)packs[search_pack].n - 1;
        if (tsel < 0) tsel = 0;
    }
}
static void open_title(int i) {
    nback = 0;
    if (pack_use(search_pack) && article_open((uint8_t)search_pack, false, (uint32_t)i)) state = ST_TEXT;
    else msg_show(ST_SEARCH, "Could not read that article.");
}
static void key_search(uint16_t code) {
    char c = a11_hid_to_char(code, shift, false);
    size_t n = strlen(query);
    if (qcur > (int)n) qcur = (int)n;
    if (ctrl) {
        if (field_clip(code, query, sizeof query, &qcur)) query_changed();
        return;
    }
    bool bv = pack_browse_view();
    int pg = alt ? (rows > 8 ? rows - 6 : 1) : 1;   // Alt+Up and Alt+Down page
    if (code == HID_ESC) { pack_close(); state = ST_BROWSE; browse(cwd); }
    else if (code == HID_LEFT) { if (alt) qcur = 0; else if (qcur > 0) qcur--; }   // Alt+Left is Home
    else if (code == HID_RIGHT) { if (alt) qcur = (int)n; else if (qcur < (int)n) qcur++; }   // Alt+Right is End
    else if (code == HID_UP && bv) { tsel = tsel > pg ? tsel - pg : 0; engaged = true; }
    else if (code == HID_DOWN && bv) { int tn = (int)packs[search_pack].n; tsel = tsel + pg < tn ? tsel + pg : (tn ? tn - 1 : 0); engaged = true; }
    else if (code == HID_UP && !n && last_query[0] && search_all) { snprintf(query, sizeof query, "%s", last_query); qcur = (int)strlen(query); query_changed(); }
    else if (code == HID_UP) rsel = rsel > pg ? rsel - pg : 0;
    else if (code == HID_DOWN) rsel = rsel + pg < nres ? rsel + pg : (nres ? nres - 1 : 0);
    else if (code == HID_BACKSPACE) {
        if (qcur > 0) { memmove(query + qcur - 1, query + qcur, n - (size_t)qcur + 1); qcur--; }
        query_changed();
    }
    else if (code == HID_ENTER) {
        if (bv && (engaged || !query[0] || !text_mode)) open_title(tsel);
        else if (text_mode && !results_fresh) {
            searching = true; draw(); vTaskDelay(pdMS_TO_TICKS(80));   // show that Enter was taken before the search blocks
            snprintf(last_query, sizeof last_query, "%s", query);
            text_search(); results_fresh = true; searching = false;
        }
        else if (nres) {
            nback = 0;
            if (article_open(res[rsel].pack, res[rsel].doc, res[rsel].idx)) state = ST_TEXT;
            else msg_show(ST_SEARCH, "Could not read that article.");
        }
    }
    else if (c >= 32 && c < 127 && n < sizeof query - 1) {
        memmove(query + qcur + 1, query + qcur, n - (size_t)qcur + 1);
        query[qcur++] = c;
        query_changed();
    }
}
static void open_entry(int i) {
    if (i < 0 || i >= nents) return;
    ent_t *x = &ents[i];
    char p[400];
    snprintf(p, sizeof p, "%.255s/%.100s", cwd, x->name);
    if (x->hub) {
        for (int k = 0; k < npacks; k++) {
            const char *base = strrchr(packs[k].dir, '/');
            if (base && !strcmp(base + 1, "topics")) { nback = 0; hub_mode = true; if (article_open((uint8_t)k, true, 0)) state = ST_TEXT; else msg_show(ST_BROWSE, "The topics pack is incomplete."); }
        }
        return;
    }
    if (x->term) { quit_app = APP_BACK; return; }
    if (x->loader) { to_loader(); return; }
    if (x->files && files_handler) { run_files_row(); return; }
    if (x->all) { search_all = true; text_mode = true; query[0] = 0; qcur = 0; nres = 0; rsel = 0; results_fresh = false; state = ST_SEARCH; }
    else if (x->pack) {
        int pi = -1;
        for (int k = 0; k < npacks; k++) if (!strcmp(packs[k].dir, p)) pi = k;
        if (pi < 0 && npacks < MAX_PACKS && info_read(p, &packs[npacks])) pi = npacks++;
        if (pi >= 0 && pack_use(pi)) { search_pack = pi; search_all = false; text_mode = packs[pi].words > 0; query[0] = 0; qcur = 0; nres = 0; rsel = 0; tsel = ttop = 0; engaged = false; results_fresh = false; state = ST_SEARCH; }
        else msg_show(ST_BROWSE, "That pack is incomplete.");
    } else if (x->dir) browse(p);
    else if (doc_open_file(p, x->name)) state = ST_TEXT;
    else msg_show(ST_BROWSE, "Could not open that file.");
}
// The file manager runs, and when it returns the list comes back, or the document it asked for opens and closing that goes back to the file manager.
static void run_files_row(void) {
    const char *doc = files_handler();
    char path[400] = "";
    if (doc) snprintf(path, sizeof path, "%s", doc);
    font_idx = settings_get("font", font_idx);
    display_set_font(font_idx, &cols, &rows);
    scr_alloc();
    pack_close();   // another card may be in now
    state = ST_BROWSE;
    browse(cwd);
    if (path[0]) {
        const char *base = strrchr(path, '/');
        if (doc_open_file(path, base ? base + 1 : path)) { state = ST_TEXT; doc_from_files = true; }
        else msg_show(ST_BROWSE, "Could not open that file.");
    }
}
static void key_browse(uint16_t code) {
    if (alt && code == HID_UP) code = HID_PGUP;   // Alt+Up and Alt+Down are Page Up and Page Down
    else if (alt && code == HID_DOWN) code = HID_PGDN;
    if (alt && code == HID_LEFT) bsel = 0;   // Alt+Left and Alt+Right are Home and End
    else if (alt && code == HID_RIGHT) bsel = nents ? nents - 1 : 0;
    else if (code == HID_UP && bsel > 0) bsel--;
    else if (code == HID_DOWN && bsel + 1 < nents) bsel++;
    else if (code == HID_PGUP) bsel = bsel > rows - 2 - last_foot_n ? bsel - (rows - 2 - last_foot_n) : 0;
    else if (code == HID_PGDN) bsel = bsel + rows - 2 - last_foot_n < nents ? bsel + rows - 2 - last_foot_n : (nents ? nents - 1 : 0);
    else if (code == HID_ENTER) open_entry(bsel);
    else if (code == HID_ESC || code == HID_BACKSPACE) go_up();
    else if (a11_hid_to_char(code, false, false) == 't') apply_theme(theme_idx + 1);
}

static bool power_off_pending;   // never set now, Mode 1 does the sleeping so the PIN lock stays in one place
static void power_off(void) { quit_app = APP_SLEEP; }

static void menu_toggle(void) {
    if (state == ST_MENU) { state = menu_back; return; }
    if (state == ST_IMAGE) { imgview_close(); menu_back = ST_TEXT; }
    else menu_back = state == ST_FIND || state == ST_MSG ? ST_BROWSE : state;
    menu_sel = 0;
    state = ST_MENU;
}
static void key_menu(uint16_t code) {
    if (code == HID_UP) menu_sel = menu_sel > 0 ? menu_sel - 1 : 5;       // wraps round
    else if (code == HID_DOWN) menu_sel = menu_sel < 5 ? menu_sel + 1 : 0;
    else if (code == HID_LEFT || code == HID_RIGHT) {
        int d = code == HID_RIGHT ? 1 : -1;
        if (menu_sel == 2) apply_theme(theme_idx + d);
        else if (menu_sel == 3) { apply_font(font_idx + d); if (dnrows || dn) doc_wrap(); }
        else if (menu_sel == 4) apply_brightness(bright_idx + d);
    }
    else if (code == HID_ENTER) { if (menu_sel == 0) quit_app = APP_BACK; else if (menu_sel == 1) to_loader(); else if (menu_sel == 5) state = menu_back; }
    else if (code == HID_ESC) state = menu_back;
}

static void handle_key(a11_event_t ev) {
    uint16_t code = ev.code;
    if (code == A11_HID_CAPS_LOCK) code = A11_HID_LEFT_CTRL;   // Caps acts as Ctrl, as in Mode 1
    if (code == A11_HID_LEFT_SHIFT) { shift = ev.down; if (ev.down) gui_tap = false; return; }
    if (code == A11_HID_LEFT_CTRL) { ctrl = ev.down; if (ev.down) gui_tap = false; return; }
    if (code == A11_HID_LEFT_ALT) { alt = ev.down; if (ev.down) gui_tap = false; return; }
    if (code == A11_HID_LEFT_GUI) {
        gui = ev.down;
        if (ev.down) gui_tap = true;
        else if (gui_tap) { gui_tap = false; if (!power_off_pending) menu_toggle(); }   // GUI pressed and let go on its own opens the menu
        return;
    }
    if (!ev.down) return;
    gui_tap = false;
    if (power_off_pending) { if (code == A11_KEY_ON) { power_off_pending = false; display_sleep(false); draw(); } return; }
    if (code == A11_KEY_OFF) { if (state == ST_IMAGE) { imgview_close(); state = ST_TEXT; } power_off(); return; }
    if (code == A11_KEY_BACKLIGHT) { apply_brightness(bright_idx == 0 ? 5 : bright_idx - 1); return; }
    if (gui && code == HID_ESC) to_loader();
    if (ctrl && (code == HID_MINUS || code == HID_EQUALS)) {
        apply_font(font_idx + (code == HID_EQUALS ? 1 : -1));
        if (dnrows || dn) doc_wrap();
        if (state == ST_BROWSE) browse(cwd);
        return;
    }
    switch (state) {
        case ST_MENU: key_menu(code); break;
        case ST_BROWSE: key_browse(code); break;
        case ST_TEXT: key_text(code); break;
        case ST_FIND: key_find(code); break;
        case ST_SEARCH: key_search(code); break;
        case ST_MSG: state = msg_back; break;
        case ST_IMAGE: {
            iv_action_t act = imgview_key(code);
            if (act == IV_NEXT || act == IV_PREV) {   // fitted: next or previous picture of this article
                int n = pic_link_step(img_link, act == IV_PREV ? -1 : 1);
                if (n != img_link && show_image(cur.pack, (uint32_t)atoi(linkstr + links[n].tgt + 4))) { img_link = n; sellink = n; }
            } else if (act == IV_CLOSE) { imgview_close(); state = ST_TEXT; sellink = img_link; }
            break;
        }
    }
}

static bool sb_active(void) {
    if (state == ST_TEXT || state == ST_FIND) return dnrows > content_rows();
    if (state == ST_BROWSE) return nents > rows - 1 - last_foot_n;
    if (state == ST_SEARCH) return pack_browse_view() ? (int)packs[search_pack].n > s_rvis : nres > s_rvis;
    return false;
}
static void sb_jump(int y) {
    int H = BOARD_LCD_HEIGHT;
    if (y < 0) y = 0;
    if (y > H - 1) y = H - 1;
    if (state == ST_TEXT || state == ST_FIND) {
        int span = dnrows - content_rows();
        dtop = span > 0 ? (int)((long)span * y / (H - 1)) : 0;
    } else if (state == ST_BROWSE) {
        int vis = rows - 1 - last_foot_n, span = nents - vis;
        btop = span > 0 ? (int)((long)span * y / (H - 1)) : 0;
        if (bsel < btop) bsel = btop;
        if (bsel >= btop + vis) bsel = btop + vis - 1;
    } else if (state == ST_SEARCH && pack_browse_view()) {
        int vis = s_rvis, span = (int)packs[search_pack].n - vis;
        ttop = span > 0 ? (int)((long)span * y / (H - 1)) : 0;
        if (tsel < ttop) tsel = ttop;
        if (tsel >= ttop + vis) tsel = ttop + vis - 1;
    } else if (state == ST_SEARCH) {
        int vis = s_rvis, span = nres - vis;
        rtop = span > 0 ? (int)((long)span * y / (H - 1)) : 0;
        if (rsel < rtop) rsel = rtop;
        if (rsel >= rtop + vis) rsel = rtop + vis - 1;
    }
}
static int down_x, down_y;
static int64_t down_t;
static bool held_down, hold_fired;   // a finger is down, and holding it already copied a link
// The link under a touch, or -1. A fingertip is wider than one letter, so one cell either side counts too.
static int link_at(int x, int y) {
    int cw, ch;
    display_cell_size(&cw, &ch);
    int r = y / ch - 1;
    if (r < 0 || r >= content_rows() || dtop + r >= dnrows) return -1;
    int32_t s0 = drows[dtop + r], e0 = drows[dtop + r + 1];
    int32_t k = s0 + (x - 1) / cw;
    int best = -1;
    for (int i = 0; i < nlinks; i++) {
        if (links[i].e <= s0 || links[i].s >= e0) continue;
        if (k >= links[i].s && k < links[i].e) return i;
        if (best < 0 && k + 1 >= links[i].s && k - 1 < links[i].e) best = i;
    }
    return best;
}
// Holding a finger on a link copies its words and flashes it, without opening it.
static void hold_check(void) {
    if (!held_down || hold_fired || state != ST_TEXT || !doc_article) return;
    int64_t now = esp_timer_get_time();
    if (now - down_t < 600000) return;
    int i = link_at(down_x, down_y);
    if (i < 0) return;
    hold_fired = true;
    static char text[CLIP_MAX_LEN + 1];
    if (range_text(links[i].s, links[i].e, text, sizeof text)) {
        int old = clipboard_find(text);
        if (old >= 0) clipboard_remove(old);
        clipboard_add(text);
    }
    sel_a = links[i].s; sel_b = links[i].e;
    sel_clear_at = now + 700000;
    last_tap_text[0] = 0;
}
static void handle_touch(touch_event_t te) {
    static int start_y, acc;
    int cw, ch;
    display_cell_size(&cw, &ch);
    if (state == ST_IMAGE) {   // a tap zooms in or back out, a drag moves around when zoomed in, holding a finger down closes the picture
        if (imgview_touch(te) == IV_CLOSE) { imgview_close(); state = ST_TEXT; sellink = img_link; }
        return;
    }
    // Dragging the scroll bar on the right edge jumps to the matching place.
    static bool sbdrag;
    if (te.type == TOUCH_DOWN) { sbdrag = te.x >= BOARD_LCD_WIDTH - 3 * display_scrollbar_width() && sb_active(); }
    if (sbdrag) {
        if (te.type == TOUCH_UP) { sbdrag = false; return; }
        sb_jump(te.y);
        return;
    }
    static int last_tap_x, last_tap_y, tap_count;
    static int64_t last_tap_time;
    int64_t now = esp_timer_get_time();
    if (te.type == TOUCH_DOWN) { start_y = te.y; acc = 0; down_x = te.x; down_y = te.y; down_t = now; held_down = true; hold_fired = false; return; }
    if (te.type == TOUCH_MOVE && (abs(te.x - down_x) > 14 || abs(te.y - down_y) > 14)) held_down = false;   // dragging, not holding
    if (te.type == TOUCH_UP) {
        held_down = false;
        if (hold_fired) { hold_fired = false; return; }   // the hold already copied the link, letting go must not open it
        bool tap = abs(te.x - down_x) < 14 && abs(te.y - down_y) < 14 && now - down_t < 450000;
        bool on_link = false;
        if (tap && state == ST_TEXT && doc_article) {   // a tap follows the link under the finger
            int best = link_at(te.x, te.y);
            if (best >= 0) {
                on_link = true;
                sellink = best;
                draw(); vTaskDelay(pdMS_TO_TICKS(40));   // the link goes reversed at once, so you know the tap landed while the page loads
                if (!follow_link(best)) msg_show(ST_TEXT, "That page is not in this pack.");
            }
        }
        // Taps on plain text copy, like the terminal: two taps a word, three a line, four a paragraph, five the screen.
        if (tap && state == ST_TEXT && !on_link) {
            bool again = last_tap_time && now - last_tap_time < 450000 && abs(te.x - last_tap_x) < 40 && abs(te.y - last_tap_y) < 40;
            tap_count = again ? (tap_count < 5 ? tap_count + 1 : 5) : 1;
            last_tap_time = now; last_tap_x = te.x; last_tap_y = te.y;
            if (tap_count == 1) { sel_a = sel_b = -1; last_tap_text[0] = 0; }
            else {
                if (tap_count == 2) last_tap_text[0] = 0;
                int r = te.y / ch - 1;
                if (r >= 0 && r < content_rows()) select_unit(dtop + r, (te.x - 1) / cw, tap_count);
            }
        }
        if (tap && state == ST_SEARCH) {   // a tap opens the row
            int r = te.y / ch - s_first;
            if (r >= 0 && r < s_rvis) {
                if (pack_browse_view()) { int i = ttop + r; if (i < (int)packs[search_pack].n) { tsel = i; engaged = true; open_title(i); } }
                else if (rtop + r < nres) {
                    rsel = rtop + r;
                    nback = 0;
                    if (article_open(res[rsel].pack, res[rsel].doc, res[rsel].idx)) state = ST_TEXT;
                    else msg_show(ST_SEARCH, "Could not read that article.");
                }
            }
        }
        if (tap && state == ST_BROWSE) {   // a tap opens the row
            int top = btop;
            int r = te.y / ch - 1;
            if (r >= 0 && top + r < nents) { bsel = top + r; open_entry(bsel); }
        }
        return;
    }
    acc += te.y - start_y;
    start_y = te.y;
    while (acc >= ch || acc <= -ch) {
        int dir = acc > 0 ? 1 : -1;   // finger down shows earlier text
        acc -= dir * ch;
        if (state == ST_TEXT) doc_scroll(-dir);
        else if (state == ST_BROWSE) { bsel += -dir; if (bsel < 0) bsel = 0; if (bsel >= nents) bsel = nents ? nents - 1 : 0; }
        else if (state == ST_SEARCH && pack_browse_view()) { tsel += -dir; engaged = true; int tn = (int)packs[search_pack].n; if (tsel < 0) tsel = 0; if (tsel >= tn) tsel = tn ? tn - 1 : 0; }
        else if (state == ST_SEARCH) { rsel += -dir; if (rsel < 0) rsel = 0; if (rsel >= nres) rsel = nres ? nres - 1 : 0; }
    }
}

app_exit_t reader_app(QueueHandle_t q, QueueHandle_t tq) {
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
    if (state == ST_BROWSE || state == ST_MENU) { state = ST_BROWSE; browse(cwd[0] ? cwd : LIB); }
    ESP_LOGI(TAG, "reader ready, %d entries", nents);
    while (!quit_app) {
        a11_event_t ev;
        touch_event_t te;
        bool changed = false;
        while (xQueueReceive(tq, &te, 0) == pdTRUE) { handle_touch(te); changed = true; }
        if (held_down && !hold_fired) { hold_check(); if (hold_fired) changed = true; }
        if (xQueueReceive(q, &ev, pdMS_TO_TICKS(40)) == pdTRUE) { handle_key(ev); changed = true; }
        static int64_t last_draw;
        if (sel_clear_at && esp_timer_get_time() > sel_clear_at) { sel_clear_at = 0; sel_a = sel_b = -1; last_tap_text[0] = 0; changed = true; }
        if (quit_app) break;
        {   // no card, or one that could not be read, while the library list shows: look again now and then, so putting a good card in just works
            static int64_t last_look;
            int64_t tn = esp_timer_get_time();
            if (state == ST_BROWSE && !strcmp(cwd, LIB) && !sdcard_ok() && tn - last_look > 3000000 && sdcard_async_state() == 0) { last_look = tn; sdcard_mount_async(); }
            if (sdcard_async_state() >= 2) { bool found = sdcard_async_state() == 2; sdcard_async_ack(); if (found && state == ST_BROWSE) { browse(cwd); changed = true; } }
        }
        if (changed || esp_timer_get_time() - last_draw > (marquee_on ? 200000 : 1000000)) { draw(); last_draw = esp_timer_get_time(); }
    }
    if (state == ST_IMAGE) { imgview_close(); state = ST_TEXT; }
    if (state == ST_MENU) state = menu_back;
    // What is open stays open for next time. Only the screen copies go, Mode 1 puts its own back.
    free(scr); free(back_buf);
    scr = back_buf = NULL;
    return quit_app;
}
