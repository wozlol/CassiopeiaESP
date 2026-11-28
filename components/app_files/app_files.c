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

typedef enum { ST_LIST, ST_CONFIRM, ST_NEWDIR, ST_MSG, ST_PICTURE, ST_NOCARD, ST_THUMBS } state_t;
static state_t state = ST_LIST;
static char msg_text[200];
static terminal_cell_t *scr, *back_buf;
static int cols, rows, font_idx, theme_idx, bright_idx;
#define TC (cols - 1)
static bool shift, ctrl, alt, gui, gui_tap;
static app_exit_t quit_app;

typedef struct { char name[64]; bool dir; bool heart; long size; } ent_t;   // heart: it is in the folder's hearts.txt
static ent_t *ents;
static int nents, bsel, btop;
static char cwd[256] = ROOT;
static char cut_path[400];   // what x marked, moved by p
static char del_path[400], del_name[64];
static bool del_dir;
static char nd_buf[48];
static int last_foot_n = 1;
static char pending_doc[400];
const char *files_take_doc(void) { static char out[400]; if (!pending_doc[0]) return NULL; snprintf(out, sizeof out, "%s", pending_doc); pending_doc[0] = 0; return out; }
static bool ejected_hold(void);
static bool ejected;   // the card was let go on purpose, the screen says it can be taken out
static void (*sound_handler)(const char *path);
void files_set_sound_handler(void (*h)(const char *path)) { sound_handler = h; }
static int pic_idx;
// thumbnails: the pictures of the folder, 4 across, scrolled by dragging
#define TCOLS 4
#define TCW 120
#define TCH 106
#define TIW 116
#define TIH 102
#define TMAX_KEEP 96
static int *tpics, ntp, tsel, toff;   // indices into ents, how many, the selected one, scroll offset in pixels
static uint16_t **tthumb; static uint8_t *tfail;
static uint16_t *tscreen;
static bool from_thumbs;              // the picture on screen was opened from the thumbnails, closing it goes back to them   // the picture on screen, an index into ents

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
// ---------- favorites: hearts.txt in the folder lists the pictures that were hearted, one name a line, easy to read on a computer ----------
static void hearts_load(const char *dir) {
    char p[400];
    snprintf(p, sizeof p, "%.255s/hearts.txt", dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        for (int i = 0; i < nents; i++) if (!ents[i].dir && !strcmp(ents[i].name, line)) { ents[i].heart = true; break; }
    }
    fclose(f);
}
// Adds the picture to hearts.txt, or takes it out if it is there. The rest of the file is kept as it is.
static void hearts_toggle(int idx) {
    char p[400];
    snprintf(p, sizeof p, "%.255s/hearts.txt", cwd);
    char *text = heap_caps_calloc(1, 40000, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT), *out = heap_caps_calloc(1, 40000, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!text || !out) { free(text); free(out); return; }
    FILE *f = fopen(p, "r");
    size_t n = 0;
    if (f) { n = fread(text, 1, 39000, f); fclose(f); }
    text[n] = 0;
    bool had = false;
    size_t on = 0;
    for (char *line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        if (!strcmp(line, ents[idx].name)) { had = true; continue; }
        on += (size_t)snprintf(out + on, 39900 - on, "%s\n", line);
    }
    if (!had) on += (size_t)snprintf(out + on, 39900 - on, "%s\n", ents[idx].name);
    if (on == 0) remove(p);
    else { FILE *g = fopen(p, "w"); if (g) { fwrite(out, 1, on, g); fclose(g); } }
    ents[idx].heart = !had;
    free(text); free(out);
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
        memset(x, 0, sizeof *x);   // a slot used before must not keep its old heart
        snprintf(x->name, sizeof x->name, "%.63s", e->d_name);
        x->dir = S_ISDIR(st.st_mode);
        x->size = x->dir ? 0 : (long)st.st_size;
    }
    closedir(d);
    qsort(ents, nents, sizeof(ent_t), ent_cmp);
    hearts_load(dir);
}
static bool is_jpeg(const char *n) {   // a picture the viewer can try: JPEG, PNG, BMP or GIF
    const char *e = strrchr(n, '.');
    return e && (!strcasecmp(e, ".jpg") || !strcasecmp(e, ".jpeg") || !strcasecmp(e, ".png") || !strcasecmp(e, ".bmp") || !strcasecmp(e, ".gif"));
}
static bool is_sound(const char *n) {
    const char *e = strrchr(n, '.');
    return e && (!strcasecmp(e, ".wav") || !strcasecmp(e, ".mp3"));
}
static bool is_doc(const char *n) {   // plain text and the markup the reader can show
    static const char *const EXT[] = {".txt", ".md", ".markdown", ".html", ".htm", ".text", ".log", ".csv", ".json", ".xml", ".ini", ".cfg", ".conf", ".yaml", ".yml", ".c", ".h", ".cpp", ".py", ".js", ".sh", ".tsv", ".srt", ".nfo", ".readme"};
    const char *e = strrchr(n, '.');
    if (!e) return false;
    for (size_t i = 0; i < sizeof EXT / sizeof EXT[0]; i++) if (!strcasecmp(e, EXT[i])) return true;
    return false;
}
static bool show_picture(int idx) {
    free(tscreen); tscreen = NULL;   // the thumbnail frame is not needed while a picture is open
    char p[400];
    snprintf(p, sizeof p, "%.255s/%.100s", cwd, ents[idx].name);
    bool ok = imgview_file(p);
    if (ok && ents[idx].heart) imgview_set_heart(true);
    return ok;
}
static int next_picture(int from, int dir) {
    for (int i = from + dir; i >= 0 && i < nents; i += dir) if (!ents[i].dir && is_jpeg(ents[i].name)) return i;
    return from;
}
static void thumbs_free(void) {
    if (tthumb) for (int i = 0; i < ntp; i++) free(tthumb[i]);
    free(tthumb); free(tfail); free(tpics); free(tscreen);
    tthumb = NULL; tfail = NULL; tpics = NULL; tscreen = NULL; ntp = 0;
}
static void thumbs_scroll_to_sel(void) {
    int row = tsel / TCOLS;
    if (row * TCH < toff) toff = row * TCH;
    if ((row + 1) * TCH > toff + 320) toff = (row + 1) * TCH - 320;
    int maxoff = ((ntp + TCOLS - 1) / TCOLS) * TCH - 320;
    if (toff > maxoff) toff = maxoff;
    if (toff < 0) toff = 0;
}
static void leave_picture(void) {
    imgview_close();
    if (from_thumbs && ntp) {
        for (int k = 0; k < ntp; k++) if (tpics[k] == pic_idx) tsel = k;
        thumbs_scroll_to_sel();
        state = ST_THUMBS;
    } else state = ST_LIST;
}
static bool thumbs_enter(int from_idx) {
    thumbs_free();
    int cnt = 0;
    for (int i = 0; i < nents; i++) if (!ents[i].dir && is_jpeg(ents[i].name)) cnt++;
    if (cnt == 0) return false;
    tpics = heap_caps_calloc((size_t)cnt, sizeof(int), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    tthumb = heap_caps_calloc((size_t)cnt, sizeof(uint16_t *), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    tfail = heap_caps_calloc((size_t)cnt, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tpics || !tthumb || !tfail) { thumbs_free(); return false; }
    ntp = 0; tsel = 0;
    for (int i = 0; i < nents; i++) if (!ents[i].dir && is_jpeg(ents[i].name)) { if (i == from_idx) tsel = ntp; tpics[ntp++] = i; }
    toff = 0;
    thumbs_scroll_to_sel();
    display_set_scrollbar(0, 0, 0);
    state = ST_THUMBS;
    return true;
}
static void thumbs_leave(void) {
    if (ntp) bsel = tpics[tsel];
    thumbs_free();
    display_raw(false);
    state = ST_LIST;
}
// A folder that is mostly pictures opens as thumbnails by itself.
static void auto_thumbs(void) {
    int pics = 0;
    for (int i = 0; i < nents; i++) if (!ents[i].dir && is_jpeg(ents[i].name)) pics++;
    if (pics >= 4 && pics * 2 >= nents) thumbs_enter(-1);
}
static void thumbs_draw(void) {
    if (!tscreen) tscreen = heap_caps_malloc((size_t)480 * 320 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tscreen) return;
    memset(tscreen, 0, (size_t)480 * 320 * 2);
    for (int i = 0; i < ntp; i++) {
        int cx = (i % TCOLS) * TCW, cy = (i / TCOLS) * TCH - toff;
        if (cy + TCH <= 0 || cy >= 320) continue;
        for (int y = 2; y < TCH - 2; y++) {
            int sy = cy + y;
            if (sy < 0 || sy >= 320) continue;
            uint16_t *dst = tscreen + (size_t)sy * 480 + cx + 2;
            if (tthumb[i]) { if (y - 2 < TIH) memcpy(dst, tthumb[i] + (size_t)(y - 2) * TIW, TIW * 2); }
            else for (int x = 0; x < TIW; x++) dst[x] = tfail[i] ? 0x0000 : 0x0821;   // dark gray while it waits, black when it could not be made
        }
        if (ents[tpics[i]].heart) imgview_draw_heart(tscreen, 480, 320, cx + TCW - 9 * 2 - 8, cy + 8, 2);
        if (i == tsel) {   // a white frame around the selected one
            for (int x = 0; x < TCW; x++) for (int t = 0; t < 2; t++) {
                int yt = cy + t, yb = cy + TCH - 1 - t;
                if (yt >= 0 && yt < 320) tscreen[(size_t)yt * 480 + cx + x] = 0xFFFF;
                if (yb >= 0 && yb < 320) tscreen[(size_t)yb * 480 + cx + x] = 0xFFFF;
            }
            for (int y = 0; y < TCH; y++) for (int t = 0; t < 2; t++) {
                int sy = cy + y;
                if (sy < 0 || sy >= 320) continue;
                tscreen[(size_t)sy * 480 + cx + t] = 0xFFFF;
                tscreen[(size_t)sy * 480 + cx + TCW - 1 - t] = 0xFFFF;
            }
        }
    }
    display_raw(true);
    display_draw_pixels(0, 0, 480, 320, tscreen);
}
// Makes one thumbnail that is on screen and not made yet, the selected one first. True when it made one, so the screen is drawn again.
static bool thumbs_step(void) {
    int first = (toff / TCH) * TCOLS, last = ((toff + 319) / TCH + 1) * TCOLS;
    if (last > ntp) last = ntp;
    int pick = -1;
    if (tsel >= first && tsel < last && !tthumb[tsel] && !tfail[tsel]) pick = tsel;
    for (int i = first; pick < 0 && i < last; i++) if (!tthumb[i] && !tfail[i]) pick = i;
    if (pick < 0) return false;
    int made = 0;
    for (int i = 0; i < ntp; i++) if (tthumb[i]) made++;
    if (made >= TMAX_KEEP) {   // keep memory in check, the one furthest from the screen goes
        int far = -1, fd = -1, mid = (first + last) / 2;
        for (int i = 0; i < ntp; i++) if (tthumb[i] && abs(i - mid) > fd) { fd = abs(i - mid); far = i; }
        if (far >= 0) { free(tthumb[far]); tthumb[far] = NULL; }
    }
    char p[400];
    snprintf(p, sizeof p, "%.255s/%.100s", cwd, ents[tpics[pick]].name);
    tthumb[pick] = imgview_thumb(p, TIW, TIH);
    if (!tthumb[pick]) tfail[pick] = 1;
    return true;
}
static void go_up(void) {
    if (!strcmp(cwd, ROOT)) { quit_app = APP_BACK; return; }   // Esc at the top of the card leaves the file manager
    char *s = strrchr(cwd, '/');
    if (s && s > cwd) { *s = 0; browse(cwd); auto_thumbs(); }
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
    if (state == ST_NOCARD) {
        header("Files");
        put_flow(2, 1, ejected ? "The card is safe to take out. Put a card in and it opens by itself." : "No card. Put a card in and it opens by itself.", ATTR_BOLD);
        footer("Esc goes back");
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
    const char *hint = cut_path[0] ? "Enter open   d delete   p paste here   x cut again   n new folder   e eject   Esc up" : "Enter open   d delete   x cut   n new folder   v thumbnails   e eject card   Esc up   GUI back";
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
    if (!back_buf || state == ST_PICTURE || state == ST_THUMBS) return;
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
    else if (code == HID_ENTER && nents && ents[bsel].dir) { char p[400]; snprintf(p, sizeof p, "%.255s/%.100s", cwd, ents[bsel].name); browse(p); auto_thumbs(); }
    else if (c == 'v') thumbs_enter(nents ? bsel : -1);
    else if (code == HID_ENTER && nents && is_jpeg(ents[bsel].name)) {
        pic_idx = bsel;
        state = ST_PICTURE;
        if (!show_picture(bsel)) { leave_picture(); msg_show("%s", imgview_error()); }
    }
    else if (code == HID_ENTER && nents && is_sound(ents[bsel].name) && sound_handler) {
        char p[400];
        snprintf(p, sizeof p, "%.255s/%.100s", cwd, ents[bsel].name);
        sound_handler(p);   // the player runs, and when it returns the file list comes back
        font_idx = settings_get("font", font_idx);
        display_set_font(font_idx, &cols, &rows);
        scr_alloc();
    }
    else if (code == HID_ENTER && nents && is_doc(ents[bsel].name)) {
        snprintf(pending_doc, sizeof pending_doc, "%.255s/%.100s", cwd, ents[bsel].name);
        quit_app = APP_BACK;   // the reader shows it and comes back here when it is closed
    }
    else if (code == HID_ENTER && nents) msg_show("%s is not a kind of file this can open.", ents[bsel].name);
    else if (c == 'e') { ejected = true; sdcard_unmount(); state = ST_NOCARD; }   // lets go of the card so it can be taken out safely
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
        case ST_NOCARD: if (code == HID_ESC) quit_app = APP_BACK; break;
        case ST_THUMBS: {
            char tc = a11_hid_to_char(code, shift, false);
            if (code == HID_LEFT && tsel > 0) tsel--;
            else if (code == HID_RIGHT && tsel + 1 < ntp) tsel++;
            else if (code == HID_UP && tsel >= TCOLS) tsel -= TCOLS;
            else if (code == HID_DOWN && tsel + TCOLS < ntp) tsel += TCOLS;
            else if (code == HID_DOWN && tsel + 1 < ntp) tsel = ntp - 1;
            else if (code == HID_PGUP) tsel = tsel >= 12 ? tsel - 12 : 0;
            else if (code == HID_PGDN) tsel = tsel + 12 < ntp ? tsel + 12 : ntp - 1;
            else if (code == HID_ENTER) {
                pic_idx = tpics[tsel]; from_thumbs = true; state = ST_PICTURE;
                if (!show_picture(pic_idx)) { from_thumbs = false; imgview_close(); thumbs_leave(); msg_show("%s", imgview_error()); }
            }
            else if (code == HID_ESC || code == HID_BACKSPACE || tc == 'v') { thumbs_leave(); break; }
            thumbs_scroll_to_sel();
            break;
        }
        case ST_PICTURE: {
            iv_action_t act = imgview_key(code);
            if (act == IV_NEXT || act == IV_PREV) { int n = next_picture(pic_idx, act == IV_PREV ? -1 : 1); if (n != pic_idx) { pic_idx = n; bsel = n; if (!show_picture(n)) { leave_picture(); msg_show("%s", imgview_error()); } } }
            else if (act == IV_HEART) { hearts_toggle(pic_idx); imgview_set_heart(ents[pic_idx].heart); }
            else if (act == IV_CLOSE) { leave_picture(); from_thumbs = from_thumbs && state == ST_THUMBS; }
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
    if (state == ST_THUMBS) {   // drag to scroll under the finger, tap to open
        static int x0, y0, off0; static bool moved;
        if (te.type == TOUCH_DOWN) { x0 = te.x; y0 = te.y; off0 = toff; moved = false; }
        else if (te.type == TOUCH_MOVE) {
            if (abs(te.y - y0) > 10 || abs(te.x - x0) > 10) moved = true;
            if (moved) {
                int maxoff = ((ntp + TCOLS - 1) / TCOLS) * TCH - 320;
                toff = off0 - (te.y - y0);
                if (toff > maxoff) toff = maxoff;
                if (toff < 0) toff = 0;
            }
        } else if (te.type == TOUCH_UP && !moved) {
            int col = x0 / TCW, row = (y0 + toff) / TCH, i = row * TCOLS + col;
            if (col < TCOLS && i >= 0 && i < ntp) {
                tsel = i; pic_idx = tpics[i]; from_thumbs = true; state = ST_PICTURE;
                if (!show_picture(pic_idx)) { from_thumbs = false; imgview_close(); thumbs_leave(); msg_show("%s", imgview_error()); }
            }
        }
        return;
    }
    if (te.type == TOUCH_DOWN) { start_y = te.y; return; }
    if (te.type == TOUCH_UP && abs(te.y - start_y) < 8 && state == ST_LIST) {
        int r = te.y / ch - 1;
        if (r >= 0 && btop + r < nents) {
            bsel = btop + r;
            if (ents[bsel].dir) { char p[400]; snprintf(p, sizeof p, "%.255s/%.100s", cwd, ents[bsel].name); browse(p); }
        }
    }
}

static bool ejected_hold(void) { return false; }   // a card put in after an eject opens on its own, taking it out and putting it back is the same thing
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
        static int64_t last_probe;
        if (now - last_probe > 1200000) {   // a card taken out, or a card put in
            last_probe = now;
            if (state != ST_NOCARD && !sdcard_present()) { if (state == ST_PICTURE) imgview_close(); sdcard_unmount(); ejected = false; state = ST_NOCARD; cut_path[0] = 0; changed = true; }
            else if (state == ST_NOCARD && !ejected_hold() && sdcard_mount()) { snprintf(cwd, sizeof cwd, "%s", ROOT); browse(cwd); state = ST_LIST; ejected = false; changed = true; }
        }
        if (state == ST_THUMBS) {
            if (changed) thumbs_draw();
            else if (thumbs_step()) thumbs_draw();   // one more thumbnail is ready, the screen fills in as they come
            last_draw = now;
        } else if (changed || now - last_draw > 1000000) { draw(); last_draw = now; }
    }
    if (state == ST_PICTURE) { from_thumbs = false; leave_picture(); }
    if (state == ST_THUMBS) thumbs_leave();
    free(scr); free(back_buf);
    scr = back_buf = NULL;
    display_set_scrollbar(0, 0, 0);
    return quit_app;
}
