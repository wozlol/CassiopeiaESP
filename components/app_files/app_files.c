// File manager for the SD card, a screen inside Mode 1. Same look and keys as the reader and the media viewer.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "a11_keys.h"
#include "app_files.h"
#include "app_imgview.h"
#include "board.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdcard.h"
#include "settings.h"
#include "themes.h"
#include "status_led.h"
#include "touch.h"

#define ROOT SDCARD_MOUNT
#define MAX_ENT 400
#define HID_ENTER 40
#define HID_ESC 41
#define HID_BACKSPACE 42
#define HID_MINUS 45
#define HID_EQUALS 46
#define HID_PGUP 75
#define HID_PGDN 78
#define HID_RIGHT 79
#define HID_LEFT 80
#define HID_DOWN 81
#define HID_UP 82
static const int BRIGHT[] = {10, 25, 40, 60, 80, 100};

typedef enum { ST_LIST, ST_CONFIRM, ST_NEWDIR, ST_MSG, ST_PICTURE } state_t;
static state_t state = ST_LIST;
static char msg_text[200];
static terminal_cell_t *scr, *back_buf;
static int cols, rows, font_idx, theme_idx, bright_idx;
#define TC (cols - 1)
static bool shift, ctrl, alt, gui, gui_tap;
static app_exit_t quit_app;

typedef struct { char name[64]; bool dir; long size; } ent_t;
static ent_t *ents;
static int nents, bsel, btop;
static char cwd[256] = ROOT;
static char cut_path[400];   // what x marked, moved by p
static char del_path[400], del_name[64];
static bool del_dir;
static char nd_buf[48];
static int last_foot_n = 1;
static int pic_idx;   // the picture on screen, an index into ents

static void scr_alloc(void) {
    free(scr); free(back_buf);
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
static void header(const char *t) { put(0, 1, t, ATTR_BOLD); rowattr(0, ATTR_REVERSE); }
static int wrap_len(const char *p, int width) {
    int n = (int)strlen(p);
    if (n <= width) return n;
    int k = width;
    while (k > 0 && p[k] != ' ') k--;
    return k > 0 ? k : width;
}
static int wrap_rows(const char *s, int width) {
    if (width < 8) width = 8;
    int n = 0;
    for (const char *p = s; *p; ) { p += wrap_len(p, width); n++; while (*p == ' ') p++; }
    return n ? n : 1;
}
static int foot_n_for(const char *t) { int n = wrap_rows(t, TC - 2); return n > 3 ? 3 : n; }
static int put_flow(int r, int c, const char *s, uint8_t attrs) {
    int width = TC - c - 1;
    if (width < 8) width = 8;
    for (const char *p = s; *p; ) {
        int k = wrap_len(p, width);
        char line[200];
        snprintf(line, sizeof line, "%.*s", k < 190 ? k : 190, p);
        put(r++, c, line, attrs);
        p += k;
        while (*p == ' ') p++;
    }
    return r;
}
static void footer(const char *t) {
    int n = foot_n_for(t);
    last_foot_n = n;
    int r0 = rows - n;
    for (int r = r0; r < rows; r++) rowattr(r, ATTR_REVERSE);
    int r = r0;
    for (const char *p = t; *p && r < rows; ) {
        int k = wrap_len(p, TC - 2);
        char line[160];
        snprintf(line, sizeof line, "%.*s", k < 150 ? k : 150, p);
        put(r++, 1, line, ATTR_REVERSE);
        p += k;
        while (*p == ' ') p++;
    }
}
static void fix_top(int sel, int *top, int vis, int n) {
    if (*top > n - vis) *top = n - vis;
    if (*top < 0) *top = 0;
    if (sel < *top) *top = sel;
    if (sel >= *top + vis) *top = sel - vis + 1;
}
static void apply_font(int idx) {
    if (idx < 0 || idx >= display_font_count()) return;
    font_idx = idx;
    display_set_font(idx, &cols, &rows);
    scr_alloc();
    settings_set("font", idx);
}
static void apply_brightness(int idx) {
    bright_idx = idx < 0 ? 5 : idx > 5 ? 0 : idx;
    display_set_backlight(BRIGHT[bright_idx]);
    status_led_set_brightness(BRIGHT[bright_idx]);   // the back light dims with the screen
    settings_set("bl", bright_idx);
}
static void msg_show(const char *fmt, const char *a) { snprintf(msg_text, sizeof msg_text, fmt, a ? a : ""); state = ST_MSG; }

// ---------- the list ----------
static int ent_cmp(const void *a, const void *b) {
    const ent_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}
static void browse(const char *dir) {
    if (!ents) ents = heap_caps_calloc(MAX_ENT, sizeof(ent_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    snprintf(cwd, sizeof cwd, "%s", dir);
    nents = 0; bsel = 0; btop = 0;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && nents < MAX_ENT) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof ents[0].name) continue;
        char p[400];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        ent_t *x = &ents[nents++];
        snprintf(x->name, sizeof x->name, "%.63s", e->d_name);
        x->dir = S_ISDIR(st.st_mode);
        x->size = x->dir ? 0 : (long)st.st_size;
    }
    closedir(d);
    qsort(ents, nents, sizeof(ent_t), ent_cmp);
}
static bool is_jpeg(const char *n) {   // a picture the viewer can try: JPEG or PNG
    const char *e = strrchr(n, '.');
    return e && (!strcasecmp(e, ".jpg") || !strcasecmp(e, ".jpeg") || !strcasecmp(e, ".png"));
}
static bool show_picture(int idx) {
    char p[400];
    snprintf(p, sizeof p, "%.255s/%.100s", cwd, ents[idx].name);
    return imgview_file(p);
}
static int next_picture(int from, int dir) {
    for (int i = from + dir; i >= 0 && i < nents; i += dir) if (!ents[i].dir && is_jpeg(ents[i].name)) return i;
    return from;
}
static void leave_picture(void) { imgview_close(); state = ST_LIST; }
static void go_up(void) {
    if (!strcmp(cwd, ROOT)) { quit_app = APP_BACK; return; }   // Esc at the top of the card leaves the file manager
    char *s = strrchr(cwd, '/');
    if (s && s > cwd) { *s = 0; browse(cwd); }
}
static bool rm_tree(const char *path) {   // a file, or a folder with everything in it
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0;
    DIR *d = opendir(path);
    if (!d) return false;
    struct dirent *e;
    bool ok = true;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[400];
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        if (!rm_tree(p)) ok = false;
    }
    closedir(d);
    return ok && rmdir(path) == 0;
}

// ---------- drawing ----------
static void draw_screen(void) {
    clear();
    char b[320];
    if (state == ST_CONFIRM) {
        header("Delete");
        snprintf(b, sizeof b, "Delete %s%s?", del_name, del_dir ? " and everything in it" : "");
        int r = put_flow(2, 1, b, ATTR_BOLD);
        put_flow(r + 1, 1, "This cannot be undone.", 0);
        footer("y deletes it   any other key cancels");
        return;
    }
    if (state == ST_NEWDIR) {
        header("New folder");
        put(2, 1, "Name for the new folder:", 0);
        snprintf(b, sizeof b, " %s_", nd_buf);
        put(3, 1, b, ATTR_BOLD);
        footer("Enter makes it   Esc cancels");
        return;
    }
    if (state == ST_MSG) {
        header("Files");
        put_flow(2, 1, msg_text, 0);
        footer("Any key to go on");
        return;
    }
    snprintf(b, sizeof b, "Files  %.200s", cwd + strlen(ROOT));
    header(b);
    const char *hint = cut_path[0] ? "Enter open   d delete   p paste here   x cut again   n new folder   Esc up" : "Enter open   d delete   x cut   n new folder   Esc up   GUI back";
    last_foot_n = foot_n_for(hint);
    int visible = rows - 1 - last_foot_n;
    fix_top(bsel, &btop, visible, nents);
    display_set_scrollbar(btop, visible, nents);
    if (!sdcard_ok()) put_flow(2, 1, "No SD card, or it is not FAT32.", 0);
    else if (nents == 0) put_flow(2, 1, "Nothing here.", ATTR_BOLD);
    for (int i = 0; i < visible && btop + i < nents; i++) {
        ent_t *x = &ents[btop + i];
        if (x->dir) snprintf(b, sizeof b, " [dir]  %.200s", x->name);
        else if (x->size >= 1024 * 1024) snprintf(b, sizeof b, " %.200s  (%.1f MB)", x->name, x->size / 1048576.0);
        else snprintf(b, sizeof b, " %.200s  (%ld KB)", x->name, (x->size + 1023) / 1024);
        put(1 + i, 0, b, 0);
        if (btop + i == bsel) rowattr(1 + i, ATTR_REVERSE);
    }
    footer(hint);
}
static void draw(void) {
    if (!back_buf || state == ST_PICTURE) return;
    terminal_cell_t *live = scr;
    scr = back_buf;
    draw_screen();
    scr = live;
    memcpy(live, back_buf, (size_t)cols * rows * sizeof *scr);
}

// ---------- keys ----------
static void to_loader(void) {
    esp_ota_set_boot_partition(esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL));
    esp_restart();
}
static void key_list(uint16_t code) {
    char c = a11_hid_to_char(code, shift, false);
    int vis = rows - 1 - last_foot_n;
    if (code == HID_UP && bsel > 0) bsel--;
    else if (code == HID_DOWN && bsel + 1 < nents) bsel++;
    else if (code == HID_PGUP) bsel = bsel > vis ? bsel - vis : 0;
    else if (code == HID_PGDN) bsel = bsel + vis < nents ? bsel + vis : (nents ? nents - 1 : 0);
    else if (code == HID_ENTER && nents && ents[bsel].dir) { char p[400]; snprintf(p, sizeof p, "%.255s/%.100s", cwd, ents[bsel].name); browse(p); }
    else if (code == HID_ENTER && nents && is_jpeg(ents[bsel].name)) {
        pic_idx = bsel;
        state = ST_PICTURE;
        if (!show_picture(bsel)) { leave_picture(); msg_show("%s", imgview_error()); }
    }
    else if (code == HID_ENTER && nents) msg_show("%s is not a picture. Open sounds in Media and documents in Reader.", ents[bsel].name);
    else if (code == HID_ESC || code == HID_BACKSPACE) go_up();
    else if (c == 'd' && nents) {
        snprintf(del_name, sizeof del_name, "%s", ents[bsel].name);
        snprintf(del_path, sizeof del_path, "%.255s/%.63s", cwd, ents[bsel].name);
        del_dir = ents[bsel].dir;
        state = ST_CONFIRM;
    }
    else if (c == 'x' && nents) { snprintf(cut_path, sizeof cut_path, "%.255s/%.63s", cwd, ents[bsel].name); }
    else if (c == 'p' && cut_path[0]) {
        const char *base = strrchr(cut_path, '/');
        base = base ? base + 1 : cut_path;
        char dst[400];
        snprintf(dst, sizeof dst, "%.255s/%.63s", cwd, base);
        struct stat st;
        if (!strcmp(dst, cut_path)) cut_path[0] = 0;
        else if (strncmp(dst, cut_path, strlen(cut_path)) == 0 && dst[strlen(cut_path)] == '/') msg_show("A folder cannot be moved into itself.", NULL);
        else if (stat(dst, &st) == 0) msg_show("There is already something called %s here. Nothing was moved.", base);
        else if (rename(cut_path, dst) == 0) { cut_path[0] = 0; browse(cwd); }
        else msg_show("Could not move %s.", base);
    }
    else if (c == 'n') { nd_buf[0] = 0; state = ST_NEWDIR; }
}
static void key_newdir(uint16_t code) {
    char c = a11_hid_to_char(code, shift, false);
    size_t n = strlen(nd_buf);
    if (code == HID_ESC) state = ST_LIST;
    else if (code == HID_BACKSPACE) { if (n) nd_buf[n - 1] = 0; }
    else if (code == HID_ENTER) {
        if (!n) { state = ST_LIST; return; }
        char p[400];
        snprintf(p, sizeof p, "%.255s/%s", cwd, nd_buf);
        struct stat st;
        if (stat(p, &st) == 0) msg_show("There is already something called %s here.", nd_buf);
        else if (mkdir(p, 0777) == 0) { state = ST_LIST; browse(cwd); }
        else msg_show("Could not make %s.", nd_buf);
    }
    else if (c >= 32 && c < 127 && c != '/' && c != '\\' && c != ':' && c != '*' && c != '?' && c != '"' && c != '<' && c != '>' && c != '|' && n < sizeof nd_buf - 1) { nd_buf[n] = c; nd_buf[n + 1] = 0; }
}
static void handle_key(a11_event_t ev) {
    uint16_t code = ev.code;
    if (code == A11_HID_CAPS_LOCK) code = A11_HID_LEFT_CTRL;
    if (code == A11_HID_LEFT_SHIFT) { shift = ev.down; if (ev.down) gui_tap = false; return; }
    if (code == A11_HID_LEFT_CTRL) { ctrl = ev.down; if (ev.down) gui_tap = false; return; }
    if (code == A11_HID_LEFT_ALT) { alt = ev.down; if (ev.down) gui_tap = false; return; }
    if (code == A11_HID_LEFT_GUI) {
        gui = ev.down;
        if (ev.down) gui_tap = true;
        else if (gui_tap) { gui_tap = false; quit_app = APP_BACK; }   // GUI on its own goes back to the terminal
        return;
    }
    if (!ev.down) return;
    gui_tap = false;
    if (code == A11_KEY_OFF) { quit_app = APP_SLEEP; return; }
    if (code == A11_KEY_BACKLIGHT) { apply_brightness(bright_idx == 0 ? 5 : bright_idx - 1); return; }
    if (gui && code == HID_ESC) to_loader();
    if (ctrl && state != ST_PICTURE && (code == HID_MINUS || code == HID_EQUALS)) { apply_font(font_idx + (code == HID_EQUALS ? 1 : -1)); if (state == ST_LIST) browse(cwd); return; }
    switch (state) {
        case ST_LIST: key_list(code); break;
        case ST_NEWDIR: key_newdir(code); break;
        case ST_MSG: state = ST_LIST; break;
        case ST_PICTURE: {
            iv_action_t act = imgview_key(code);
            if (act == IV_NEXT || act == IV_PREV) { int n = next_picture(pic_idx, act == IV_PREV ? -1 : 1); if (n != pic_idx) { pic_idx = n; bsel = n; if (!show_picture(n)) { leave_picture(); msg_show("%s", imgview_error()); } } }
            else if (act == IV_CLOSE) leave_picture();
            break;
        }
        case ST_CONFIRM:
            if (a11_hid_to_char(code, false, false) == 'y') {
                bool ok = rm_tree(del_path);
                state = ST_LIST;
                browse(cwd);
                if (!ok) msg_show("Some of %s could not be deleted.", del_name);
            } else state = ST_LIST;
            break;
    }
}
static void handle_touch(touch_event_t te) {
    static int start_y;
    int cw, ch;
    display_cell_size(&cw, &ch);
    if (state == ST_PICTURE) { if (imgview_touch(te) == IV_CLOSE) leave_picture(); return; }
    if (te.type == TOUCH_DOWN) { start_y = te.y; return; }
    if (te.type == TOUCH_UP && abs(te.y - start_y) < 8 && state == ST_LIST) {
        int r = te.y / ch - 1;
        if (r >= 0 && btop + r < nents) {
            bsel = btop + r;
            if (ents[bsel].dir) { char p[400]; snprintf(p, sizeof p, "%.255s/%.100s", cwd, ents[bsel].name); browse(p); }
        }
    }
}

app_exit_t files_app(QueueHandle_t q, QueueHandle_t tq) {
    quit_app = APP_NONE;
    shift = ctrl = alt = gui = gui_tap = false;
    state = ST_LIST;
    font_idx = settings_get("font", 0);
    theme_idx = settings_get("theme", 0);
    bright_idx = settings_get("bl", 5);
    if (bright_idx < 0 || bright_idx > 5) bright_idx = 5;
    display_set_theme(themes_get(theme_idx % themes_count()));
    display_set_backlight(BRIGHT[bright_idx]);
    display_set_font(font_idx, &cols, &rows);
    scr_alloc();
    sdcard_mount();
    browse(cwd[0] ? cwd : ROOT);
    int64_t last_draw = 0;
    while (!quit_app) {
        a11_event_t ev;
        touch_event_t te;
        bool changed = false;
        while (xQueueReceive(tq, &te, 0) == pdTRUE) { handle_touch(te); changed = true; }
        if (xQueueReceive(q, &ev, pdMS_TO_TICKS(40)) == pdTRUE) { handle_key(ev); changed = true; }
        if (quit_app) break;
        int64_t now = esp_timer_get_time();
        if (changed || now - last_draw > 1000000) { draw(); last_draw = now; }
    }
    if (state == ST_PICTURE) leave_picture();
    free(scr); free(back_buf);
    scr = back_buf = NULL;
    display_set_scrollbar(0, 0, 0);
    return quit_app;
}
