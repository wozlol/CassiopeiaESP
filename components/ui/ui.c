// Device UI. A GUI tap opens the menu, Ctrl plus GUI opens the clipboard manager, Ctrl plus minus and equals change
// the font, the backlight key steps brightness, GUI plus Escape returns to the loader. While a screen is open it draws
// into its own cell buffer and the terminal keeps running underneath, closing hands the display back.
#include "ui.h"
#include <stdarg.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "a11_font.h"
#include "clipboard.h"
#include "tunnel.h"
#include "webadmin.h"
#include "app_switch.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "touch.h"
#include "status_led.h"
#include "driver/usb_serial_jtag.h"
#include "board.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "settings.h"
#include "themes.h"
#include "wrap.h"
#include "vterm.h"
#include "vtkeys.h"
#include "a11_id.h"
#include "keystore.h"
#include "sdcard.h"
#include <sys/stat.h>
#include "esp_random.h"
#include "monocypher.h"
#include "net.h"
#include "sessions.h"

#define HID_ENTER 40
#define HID_ESC 41
#define HID_BACKSPACE 42
#define HID_MINUS 45
#define HID_EQUALS 46
#define HID_RIGHT 79
#define HID_LEFT 80
#define HID_DOWN 81
#define HID_UP 82

typedef enum { UI_OFF, UI_MENU, UI_CLIP, UI_CLIP_NEW, UI_GRAB, UI_STATUS, UI_WIFI, UI_INPUT, UI_PC, UI_FIND, UI_SESS, UI_BUSY, UI_MSG, UI_CONFIRM, UI_SEC, UI_SEC_CAPTURE, UI_LOCK, UI_CLIP_EDIT, UI_BATT, UI_HIST, UI_DROP, UI_THEMED, UI_THEME_LOAD, UI_TS, UI_SYNC, UI_WEB } ui_state_t;
enum { M_SESS, M_CLIP, M_LIBRARY, M_FILES, M_WIFI, M_PC, M_THEME, M_THEMED, M_TS, M_FONT, M_BRIGHT, M_BG, M_CAPSKEY, M_WEB, M_BATT, M_SEC, M_STATUS, M_LOADER, M_COUNT };
typedef enum { IN_WIFI_PASS, IN_PC_HOST, IN_SESS_NAME, IN_PIN_NEW, IN_PIN_CONFIRM, IN_PC_AWAY, IN_THEME_NAME, IN_TS_KEY } input_kind_t;
typedef enum { BUSY_WIFI, BUSY_FIND, BUSY_LIST, BUSY_PAIR, BUSY_ATTACH, BUSY_KILL, BUSY_HIST, BUSY_DROPLIST, BUSY_DROPGET, BUSY_SYNC } busy_t;

static const char *TAG = "ui";
static const int BRIGHT[] = {10, 25, 40, 60, 80, 100};
#define N_BRIGHT 6

static ui_callbacks_t cb;
static ui_state_t state = UI_OFF;
static bool shift, ctrl, caps, gui, gui_tap;
static bool hold_guard;   // after the secret press, the keys still held down are not a shortcut until they have been let go
static int64_t hold_guard_t;
static int theme_idx, font_idx, bright_idx;
static terminal_cell_t *ov;
static int ocols, orows, term_cols, term_rows;
static int menu_sel, clip_sel, grab_row;
static char input[CLIP_MAX_LEN + 1];
static int input_len;
// networking screens
static net_ap_t aps[16];
static int nap, wifi_sel, sess_sel, nsl;
static char wifi_target[33], busy_msg[100], msg_text[360], in_prompt[80];
static input_kind_t in_kind;
static bool in_secret, in_reveal;
static ui_state_t in_back, msg_back;
static busy_t busy_kind;
static sess_info_t sl[16];
static id_computer_t pc;
static int pc_slot, pc_sel;   // which of the four computers is active, and which is highlighted on the Computer screen
static const char *ls_key(void) { static char k[16]; if (pc_slot == 0) return "last_sess"; snprintf(k, sizeof k, "last_sess%d", pc_slot); return k; }
static char last_session[41], prev_session[41], kill_target[41];
static bool alt, flat_bg, sel_active, restore_done;
static int sel_ar, sel_ac, sel_hr, sel_hc, reconnects;
static int64_t retry_at, boot_at;
// sleep, lock, keys
static bool cm_active, rscroll, sel_drag;
static int cm_x, cm_y, tap_count, edit_index, edit_len, edit_pos, edit_top;
static int64_t sel_clear_at, last_tap_time, down_t;
static int down_x, down_y, last_tap_x, last_tap_y;
static char last_tap_text[CLIP_MAX_LEN + 1], edit_buf[CLIP_MAX_LEN + 1];
static bool asleep, shift_other_key;
static const int AUTOSLEEP_MIN[] = {0, 1, 2, 5, 10, 15, 30, 60};   // idle minutes before sleeping on battery, 0 is never
#define N_AUTOSLEEP 8
static int autosleep_idx = 5, batt_sel;
static bool nosleep_usb = true;
static int64_t last_input_us;
static int wake_mask, capskey, sec_sel, lock_len, lock_fail;
static int64_t shift_down_t, last_tap_t, lockout_until;
static char lock_buf[16], new_pin[16], lock_msg[60];
static const struct { const char *name; uint16_t code; } CAPS_ACTIONS[] = {
    {"Ctrl", 224}, {"Esc", 41}, {"Alt", 226}, {"Tab", 43}, {"Caps Lock", 57}, {"GUI", 227},
};
#define N_CAPS_ACTIONS 6
#define MOD_SHIFT 1
#define MOD_CTRL 2
#define MOD_ALT 4
#define MOD_GUI 8

bool ui_shift(void) { return shift; }
bool ui_ctrl(void) { return ctrl; }
bool ui_caps(void) { return caps; }
bool ui_active(void) { return state != UI_OFF; }
bool ui_alt(void) { return alt; }

// Overlay drawing

static void ov_alloc(int c, int r) {
    free(ov);
    ov = heap_caps_calloc((size_t)c * r, sizeof(terminal_cell_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ocols = c;
    orows = r;
}

static void ov_clear(void) {
    for (int i = 0; i < ocols * orows; i++) ov[i] = (terminal_cell_t){' ', DISPLAY_DEFAULT_FG, DISPLAY_DEFAULT_BG, 0};
}

static uint16_t utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    uint16_t cp;
    if (s[0] < 0x80) { cp = s[0]; *p += 1; }
    else if ((s[0] & 0xE0) == 0xC0 && s[1]) { cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F); *p += 2; }
    else if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) { cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); *p += 3; }
    else { cp = '?'; *p += 1; }
    return cp;
}

static void ov_put(int r, int c, const char *s, uint8_t attrs) {
    if (r < 0 || r >= orows) return;
    while (*s && c < ocols) {
        uint16_t cp = utf8_next(&s);
        if (cp < 32) cp = ' ';
        if (c >= 0) { terminal_cell_t *t = &ov[r * ocols + c]; t->cp = cp; t->attrs = attrs; }
        c++;
    }
}

static void ov_row_attr(int r, uint8_t attrs) {
    for (int c = 0; c < ocols; c++) ov[r * ocols + c].attrs |= attrs;
}

// Every screen writes through these, so text word wraps at the grid width of the font in use.
// Writes s from row r, column c. Lines that wrap continue under the value (see wrap_hang). Stops at row end.
// Returns the next free row. With draw false it only counts, so a caller can measure first.
static int ov_flow(int r, int c, const char *s, uint8_t attrs, int end, bool draw) {
    int cols = ocols - c - (c > 0 ? 1 : 0);
    if (cols < 4) return r;
    int hang = wrap_hang(s, cols);
    int skip;
    bool first = true;
    while (*s && r < end) {
        int w = wrap_line(s, first ? cols : cols - hang, &skip);
        if (w <= 0) break;
        char line[160];
        int n = w < (int)sizeof line - 1 ? w : (int)sizeof line - 1;
        memcpy(line, s, (size_t)n);
        line[n] = 0;
        if (draw) ov_put(r, c + (first ? 0 : hang), line, attrs);
        r++;
        s += w + skip;
        first = false;
    }
    return r;
}

static int ov_text(int r, int c, const char *s, int end) { return ov_flow(r, c, s, 0, end, true); }

static int ov_height(const char *s, int c) { return ov_flow(0, c, s, 0, 1000, false); }

// The title bar. Returns the first row under it.
static int header(const char *title) {
    char b[160];
    snprintf(b, sizeof b, " CassiopeiaESP  %s", title);
    int r = ov_flow(0, 0, b, 0, orows / 3 + 1, true);
    for (int i = 0; i < r; i++) ov_row_attr(i, ATTR_REVERSE);
    return r;
}

// The key hints along the bottom, on as many rows as the width needs.
static char foot_lines[WRAP_FOOT_MAX][WRAP_FOOT_LEN];
static int foot_h(const char *hint) { return wrap_footer(hint, ocols, foot_lines, WRAP_FOOT_MAX); }

static void footer(const char *hint) {
    int n = foot_h(hint);
    for (int i = 0; i < n; i++) {
        int r = orows - n + i;
        ov_put(r, 0, foot_lines[i], 0);
        ov_row_attr(r, ATTR_REVERSE);
    }
}

// A scrolling list. Each item wraps and may take several rows, a selected item is highlighted on every row.
typedef void (*item_fn)(int i, char *out, size_t cap);

static void draw_list(int r0, int end, int total, int sel, item_fn fn) {
    int avail = end - r0;
    if (avail < 1 || total < 1) return;
    char b[200];
    int top = sel, used;
    fn(sel, b, sizeof b);
    used = ov_height(b, 0);
    if (used > avail) used = avail;
    while (top > 0) {
        fn(top - 1, b, sizeof b);
        int h = ov_height(b, 0);
        if (used + h > avail) break;
        used += h;
        top--;
    }
    int r = r0;
    for (int k = top; k < total && r < end; k++) {
        fn(k, b, sizeof b);
        int r2 = ov_flow(r, 0, b, 0, end, true);
        if (k == sel) for (int i = r; i < r2; i++) ov_row_attr(i, ATTR_REVERSE);
        r = r2;
    }
}

// Applying settings

static void redraw(void);
static void sel_clear(void);
static void unlock_now(void);
static void deep_sleep_now(void);
static void draw_sec(void);
static void draw_batt(void);
static void draw_drop(void);
static void draw_themed(void);
static void draw_ts(void);
static void draw_web(void);
static void key_web(uint16_t code);
static int web_sel;
static void key_ts(uint16_t code);
static int ts_sel;
static void key_themed(uint16_t code);
static void draw_theme_load(void);
static void key_theme_load(uint16_t code);
static void themed_enter(void);
static void key_drop(uint16_t code);
static void drop_enter(void);
static void hist_build(void);
static void draw_hist(void);
static void key_hist(uint16_t code);
static void hist_start(const char *name, bool from_term);
static bool on_usb_power(void);
static void go_sleep(void);
static void draw_sec_capture(void);
static void draw_lock(void);
static void draw_edit(void);
static void key_edit(uint16_t code);
static int step(int v, int d, int lo, int hi);
static void apply_theme(void) { display_set_theme(themes_get(theme_idx)); settings_set("theme", theme_idx); }
static void apply_brightness(void) { display_set_backlight(BRIGHT[bright_idx]); status_led_set_brightness(BRIGHT[bright_idx]); settings_set("bl", bright_idx); }

static void apply_font(int idx) {
    if (idx < 0 || idx >= display_font_count()) return;
    font_idx = idx;
    bool attached = (sess_state() == SESS_ACTIVE || sess_state() == SESS_CONNECTING) && sess_kind() == SESS_ATTACH;
    char name[41];
    snprintf(name, sizeof name, "%s", sess_attached_name());
    display_set_font(idx, &term_cols, &term_rows);
    if (attached) sess_end();
    cb.terminal_reset(term_cols, term_rows);
    ov_alloc(term_cols, term_rows);
    if (state != UI_OFF) display_set_text_buffer(ov, ocols, orows);
    settings_set("font", idx);
    if (attached) sess_begin(SESS_ATTACH, name);   // the computer repaints the screen for the new size
}

static void open_ui(ui_state_t s) {
    if (state == UI_OFF) {
        display_set_text_buffer(ov, ocols, orows);
        display_set_cursor(0, 0, CURSOR_NONE);
    }
    state = s;
}

static void close_ui(void) {
    state = UI_OFF;
    display_set_text_buffer(vterm_buffer(), term_cols, term_rows);
    vterm_cursor_refresh();
}

static void launch_app(int which);
static void go_sleep(void);
static void to_loader(void) {
    esp_ota_set_boot_partition(esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL));
    esp_restart();
}

// Screens

static const char *const *menu_items_now;
static void menu_item(int i, char *out, size_t cap) { snprintf(out, cap, " %s", menu_items_now[i]); }

static void draw_menu(void) {
    ov_clear();
    const char *hint = " Up Down move   Left Right change   Enter select   Esc close";
    int r0 = header("Menu"), end = orows - foot_h(hint);
    int cw = A11_FONTS[font_idx * 2].w, ch = A11_FONTS[font_idx * 2].h;
    const char *items[M_COUNT];
    char b[M_COUNT][64];
    items[M_SESS] = "Sessions";
    items[M_CLIP] = "Clipboard";
    static char wifi_line[100], pc_line[100];
    snprintf(wifi_line, sizeof wifi_line, "Wi-Fi       %s", net_state() == NET_CONNECTED ? net_ssid() : net_state_text());
    items[M_WIFI] = wifi_line;
    snprintf(pc_line, sizeof pc_line, "Computer    %s", pc.fp[0] ? pc.name : "not paired");
    items[M_PC] = pc_line;
    snprintf(b[M_THEME], 64, "Theme       < %s >", themes_get(theme_idx)->name); items[M_THEME] = b[M_THEME];
    snprintf(b[M_FONT], 64, "Font        < %dx%d  %dx%d cells >", cw, ch, term_cols, term_rows); items[M_FONT] = b[M_FONT];
    snprintf(b[M_BRIGHT], 64, "Brightness  < %d%% >", BRIGHT[bright_idx]); items[M_BRIGHT] = b[M_BRIGHT];
    snprintf(b[M_BG], 64, "Backgrounds < %s >", flat_bg ? "Plain" : "Program colors"); items[M_BG] = b[M_BG];
    snprintf(b[M_CAPSKEY], 64, "Caps key    < acts as %s >", CAPS_ACTIONS[capskey].name); items[M_CAPSKEY] = b[M_CAPSKEY];
    items[M_THEMED] = "Theme editor";
    static char ts_line[80];
    snprintf(ts_line, sizeof ts_line, "Tailscale   %s", tunnel_state_text());
    items[M_TS] = ts_line;
    static char web_line[80];
    snprintf(web_line, sizeof web_line, "Web admin    %s", webadmin_state_text());
    items[M_WEB] = web_line;
    items[M_BATT] = "Battery and power";
    items[M_SEC] = "Security";
    items[M_STATUS] = "Status";
    items[M_LIBRARY] = "Library, reader and media (restarts)";
    items[M_FILES] = "Files";
    items[M_LOADER] = "Back to the loader";
    menu_items_now = items;
    draw_list(r0, end, M_COUNT, menu_sel, menu_item);
    footer(hint);
}

static void clip_item(int i, char *out, size_t cap) { snprintf(out, cap, " %2d  %.90s", i + 1, clipboard_get(i)); }

static void draw_clip(void) {
    ov_clear();
    char b[140];
    const char *hint = " Enter paste   e edit   n new   c grab line   Backspace delete   Esc close";
    snprintf(b, sizeof b, "Clipboard %d of %d", clipboard_count() ? clip_sel + 1 : 0, clipboard_count());
    int r0 = header(b), end = orows - foot_h(hint);
    if (clipboard_count() == 0) ov_text(r0 + 1, 1, "Empty. n adds text, c grabs a line from the screen.", end);
    else draw_list(r0, end, clipboard_count(), clip_sel, clip_item);
    footer(hint);
}

// Typed text, wrapped at the width. When it is longer than the rows left, the end stays in view with the cursor.
static void draw_typed(int row0, int end, bool hide) {
    int width = ocols - 2, avail = end - row0;
    if (width < 1 || avail < 1) return;
    int lines = input_len / width + 1;
    int skip = lines > avail ? lines - avail : 0;
    for (int i = skip * width; i < input_len; i++) {
        int k = i - skip * width;
        char one[2] = {hide ? '*' : input[i], 0};
        ov_put(row0 + k / width, 1 + k % width, one, 0);
    }
    int k = input_len - skip * width;
    display_set_cursor(1 + k % width, row0 + k / width, CURSOR_BLOCK);
}

static void draw_new(void) {
    ov_clear();
    const char *hint = " Enter save   Esc cancel";
    int end = orows - foot_h(hint);
    int r = ov_text(header("New clipboard entry") + 1, 1, "Type the text, Enter saves, Esc cancels.", end);
    draw_typed(r + 1, end, false);
    footer(hint);
}

static void draw_grab(void) {
    const terminal_cell_t *t = vterm_buffer();
    memcpy(ov, t, (size_t)ocols * orows * sizeof(terminal_cell_t));
    for (int c = 0; c < ocols; c++) ov[grab_row * ocols + c].attrs ^= ATTR_REVERSE;
}

static void draw_status(void) {
    ov_clear();
    const char *hint = " Any key goes back";
    int end = orows - foot_h(hint);
    int r = header("Status") + 1;
    char b[100];
    int64_t up = esp_timer_get_time() / 1000000;
    snprintf(b, sizeof b, " Uptime        %lld s", (long long)up); r = ov_text(r, 0, b, end);
    snprintf(b, sizeof b, " Internal RAM  %u KB free", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024)); r = ov_text(r, 0, b, end);
    snprintf(b, sizeof b, " Lowest RAM    %u KB free at the worst moment since start", (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024)); r = ov_text(r, 0, b, end);
    snprintf(b, sizeof b, " Extra RAM     %u KB free", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024)); r = ov_text(r, 0, b, end);
    snprintf(b, sizeof b, " Clipboard     %d entries", clipboard_count()); r = ov_text(r, 0, b, end);
    snprintf(b, sizeof b, " Grid          %dx%d", term_cols, term_rows); r = ov_text(r, 0, b, end);
    snprintf(b, sizeof b, " Wi-Fi         %s %s", net_state_text(), net_ip()); r = ov_text(r, 0, b, end);
    snprintf(b, sizeof b, " Computer      %s", pc.fp[0] ? pc.name : "not paired"); r = ov_text(r, 0, b, end);
    {
        char tip[24];
        snprintf(b, sizeof b, " Tailscale     %s%s%s", tunnel_state_text(), tunnel_ip(tip, sizeof tip) ? "  " : "", tunnel_ip(tip, sizeof tip) ? tip : "");
        r = ov_text(r, 0, b, end);
        if (sess_state() == SESS_ACTIVE || sess_state() == SESS_DONE) { snprintf(b, sizeof b, " Last link     %s", sess_via_away() ? "through Tailscale, the away address" : "the home address"); r = ov_text(r, 0, b, end); }
    }
    {
        int mv, pct;
        if (status_led_battery(&mv, &pct)) snprintf(b, sizeof b, " Battery       %d.%02d V  about %d%%%s", mv / 1000, (mv % 1000) / 10, pct, status_led_charging() ? "  charging" : "");
        else snprintf(b, sizeof b, " Battery       not seen");
        r = ov_text(r, 0, b, end);
    }
    {
        int asleep, wakes, secs;
        if (a11_keys_nrf_sleep(&asleep, &wakes, &secs)) snprintf(b, sizeof b, " Keyboard chip %d.%d%% asleep, %d wakes in %d s", asleep / 10, asleep % 10, wakes, secs);
        else snprintf(b, sizeof b, " Keyboard chip measuring, or not on I2C");
        r = ov_text(r, 0, b, end);
    }
    snprintf(b, sizeof b, " Session       %s", sess_state() == SESS_ACTIVE && sess_kind() == SESS_ATTACH ? sess_attached_name() : "none"); r = ov_text(r, 0, b, end);
    footer(hint);
}


// Networking screens. The whole path is Sessions: Wi-Fi if needed, find the computer, pair, pick a session.

static bool scanning, after_wifi;
static net_found_t found[6];
static int nfound, find_sel;

static void msg_show(ui_state_t back, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg_text, sizeof msg_text, fmt, ap);
    va_end(ap);
    msg_back = back;
    if (state == UI_OFF) { display_set_text_buffer(ov, ocols, orows); display_set_cursor(0, 0, CURSOR_NONE); }
    state = UI_MSG;
}

static void input_start(input_kind_t k, const char *prompt, const char *initial, bool secret, ui_state_t back) {
    in_kind = k;
    snprintf(in_prompt, sizeof in_prompt, "%s", prompt);
    snprintf(input, sizeof input, "%s", initial);
    input_len = strlen(input);
    in_secret = secret;
    in_reveal = false;
    in_back = back;
    state = UI_INPUT;
}

static void draw_msg(void) {
    ov_clear();
    const char *hint = " Any key goes back";
    int end = orows - foot_h(hint);
    ov_text(header("Message") + 1, 1, msg_text, end);
    footer(hint);
}

static void draw_input(void) {
    ov_clear();
    const char *hint = in_secret ? " Enter done   Tab show or hide   Esc cancel" : " Enter done   Esc cancel";
    int end = orows - foot_h(hint);
    int r = ov_text(header("Type") + 1, 1, in_prompt, end);
    draw_typed(r + 1, end, in_secret && !in_reveal);
    footer(hint);
}

static void ap_item(int i, char *out, size_t cap) { snprintf(out, cap, " %.32s%s", aps[i].ssid, aps[i].open ? "  (open)" : ""); }

static void draw_wifi(void) {
    ov_clear();
    char b[140];
    const char *hint = " Enter pick   r look again   f forget   Esc back";
    int r0 = header("Wi-Fi"), end = orows - foot_h(hint);
    if (net_state() == NET_CONNECTED) snprintf(b, sizeof b, " Connected to %s", net_ssid());
    else if (net_has_saved() || net_state() != NET_IDLE) snprintf(b, sizeof b, " %s: %s", net_ssid(), net_state_text());
    else snprintf(b, sizeof b, " Pick your Wi-Fi network");
    int r = ov_text(r0, 0, b, end) + 1;
    if (scanning) { ov_text(r, 1, "Looking for networks ...", end); footer(" Please wait"); return; }
    if (nap == 0) ov_text(r, 1, "No networks found, press r to look again.", end);
    else draw_list(r, end, nap, wifi_sel, ap_item);
    footer(hint);
}

static void wifi_scan(void) {
    scanning = true;
    redraw();
    nap = net_scan(aps, 16);
    wifi_sel = 0;
    scanning = false;
}

static void pc_item(int i, char *out, size_t cap) {
    id_computer_t c;
    if (i == pc_slot) c = pc;
    else id_computer_slot_load(i, &c);
    if (!c.fp[0]) snprintf(out, cap, " %d  (empty, press Enter to add a computer)", i + 1);
    else snprintf(out, cap, " %d%s %.30s   %.28s%s%.20s", i + 1, i == pc_slot ? " *" : "  ", c.name, c.host, c.away[0] ? "   away " : "", c.away);
}

static void draw_pc(void) {
    ov_clear();
    const char *hint = " Enter use   a add or pair again   e away address   f forget   Esc back";
    int end = orows - foot_h(hint);
    int r = header("Computers") + 1;
    r = ov_text(r, 1, "* is the one in use. Enter switches to a computer.", end);
    draw_list(r + 1, end, ID_MAX_COMPUTERS, pc_sel, pc_item);
    footer(hint);
}

static void found_item(int i, char *out, size_t cap) { snprintf(out, cap, " %.40s   %.15s", found[i].name, found[i].host); }

static void draw_find(void) {
    ov_clear();
    const char *hint = nfound ? " Enter pair   r look again   a type address   Esc back" : " r look again   a type address   Esc back";
    int end = orows - foot_h(hint);
    int r = header("Find your computer") + 1;
    if (nfound == 0) {
        r = ov_text(r, 1, "No new helper found on this Wi-Fi. Computers already paired are not listed.", end) + 1;
        ov_text(r, 1, "Press a to type the computer's address, or run a11helper on it and press r to look again.", end);
    } else draw_list(r, end, nfound, find_sel, found_item);
    footer(hint);
}

static void sess_item(int k, char *b, size_t cap) {
    if (k == nsl) snprintf(b, cap, " +  New session");
    else snprintf(b, cap, " %c  %-20.20s %d window%s %s", strcmp(sl[k].name, sess_attached_name()) == 0 && sess_state() == SESS_ACTIVE ? '*' : ' ',
                  sl[k].name, sl[k].windows, sl[k].windows == 1 ? "" : "s", sl[k].attached ? "in use" : "");
}

static void draw_sess(void) {
    ov_clear();
    char b[140];
    const char *hint = " Enter open   h history   k close   r refresh   Tab next computer   Esc back";
    snprintf(b, sizeof b, "Sessions on %s", pc.name);
    int r0 = header(b), end = orows - foot_h(hint);
    draw_list(r0, end, nsl + 1, sess_sel, sess_item);
    footer(hint);
}

static void draw_confirm(void) {
    ov_clear();
    char b[140];
    const char *hint = " y yes, end it   n no";
    int end = orows - foot_h(hint);
    int r = header("Close session") + 1;
    snprintf(b, sizeof b, "End the session %.40s on the computer?", kill_target);
    r = ov_text(r, 1, b, end);
    ov_text(r, 1, "Whatever is running in it stops.", end);
    footer(hint);
}

static void draw_busy(void) {
    ov_clear();
    const char *hint = " Esc cancels";
    int end = orows - foot_h(hint);
    int r = header("Working") + 1;
    if (busy_kind == BUSY_PAIR && sess_pair_code()[0]) {
        r = ov_text(r, 1, "On your computer, check this code and type y, then press Enter:", end);
        ov_flow(r + 1, 3, sess_pair_code(), ATTR_BOLD, end, true);
    } else ov_text(r, 1, busy_msg, end);
    footer(hint);
}

static void list_enter(void);
static void find_run(void);
static void sess_enter(void);

static void pair_start(const char *host, uint16_t port, const char *name) {
    snprintf(pc.host, sizeof pc.host, "%s", host);
    pc.port = port;
    snprintf(pc.name, sizeof pc.name, "%s", name);
    pc.fp[0] = 0;
    sess_end();
    sess_set_target(pc.host, port, "");
    sess_begin(SESS_PAIR, NULL);
    if (sess_state() == SESS_ERROR) { msg_show(UI_FIND, "%s", sess_error()); return; }
    busy_kind = BUSY_PAIR;
    snprintf(busy_msg, sizeof busy_msg, "Contacting %s ...", pc.name);
    state = UI_BUSY;
}

// The computer does not know this device (new key, or the computer forgot it). Pair again on the spot instead of showing an error.
static void auto_pair(void) {
    sess_end();
    if (state == UI_OFF) { display_set_text_buffer(ov, ocols, orows); display_set_cursor(0, 0, CURSOR_NONE); }
    if (!pc.host[0]) { msg_show(UI_MENU, "Not paired with a computer yet. Open the GUI menu, Computer, and press A to pair"); return; }
    pair_start(pc.host, pc.port, pc.name);
    if (state == UI_BUSY) snprintf(busy_msg, sizeof busy_msg, "%.20s does not know this device yet. Pairing now ...", pc.name);
}

static void find_run(void) {
    busy_kind = BUSY_FIND;
    snprintf(busy_msg, sizeof busy_msg, "Looking for your computer ...");
    state = UI_BUSY;
    redraw();
    nfound = net_discover(found, 6, 1800);
    find_sel = 0;
    // Computers already paired in another place are left out, so adding a second computer never pairs the first one again.
    int keep = 0;
    for (int i = 0; i < nfound; i++) {
        bool used = false;
        for (int k = 0; k < ID_MAX_COMPUTERS && !used; k++) {
            if (k == pc_slot) continue;
            id_computer_t o;
            if (id_computer_slot_load(k, &o) && o.fp[0] && !strcmp(o.host, found[i].host)) used = true;
        }
        if (!used) found[keep++] = found[i];
    }
    nfound = keep;
    if (nfound == 1) pair_start(found[0].host, found[0].port, found[0].name);
    else state = UI_FIND;
}

// The one entry point: Wi-Fi if needed, then pairing if needed, then the session list.
static void sess_enter(void) {
    if (state == UI_OFF) { display_set_text_buffer(ov, ocols, orows); display_set_cursor(0, 0, CURSOR_NONE); }
    if (net_state() != NET_CONNECTED) {
        if (net_has_saved() && net_state() != NET_FAILED) {
            busy_kind = BUSY_WIFI;
            snprintf(busy_msg, sizeof busy_msg, "Connecting to Wi-Fi ...");
            state = UI_BUSY;
            return;
        }
        after_wifi = true;
        state = UI_WIFI;
        nap = 0;
        wifi_scan();
        return;
    }
    if (!pc.fp[0]) { find_run(); return; }
    list_enter();
}

static void list_enter(void) {
    sess_end();
    sess_set_target(pc.host, pc.port, pc.fp);
    sess_begin(SESS_LIST, NULL);
    if (sess_state() == SESS_ERROR) { msg_show(UI_MENU, "%s", sess_error()); return; }
    busy_kind = BUSY_LIST;
    snprintf(busy_msg, sizeof busy_msg, "Asking %s for its sessions ...", pc.name);
    state = UI_BUSY;
}

static void attach_to(const char *name) {
    for (const char *p = name; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.')) {
            msg_show(UI_SESS, "Session names can only have letters, numbers, dash, underscore and dot. Type a different name");
            return;
        }
    if (!name[0]) { msg_show(UI_SESS, "The name is empty. Type a name"); return; }
    sess_end();
    sess_set_target(pc.host, pc.port, pc.fp);
    sess_begin(SESS_ATTACH, name);
    if (sess_state() == SESS_ERROR) { msg_show(UI_SESS, "%s", sess_error()); return; }
    if (last_session[0] && strcmp(last_session, name) != 0) snprintf(prev_session, sizeof prev_session, "%s", last_session);
    snprintf(last_session, sizeof last_session, "%s", name);
    settings_set_str(ls_key(), name);
    sel_clear();
    cb.terminal_reset(term_cols, term_rows);   // a blank terminal, the computer repaints it once attached
    display_set_text_buffer(ov, ocols, orows);
    display_set_cursor(0, 0, CURSOR_NONE);
    busy_kind = BUSY_ATTACH;
    snprintf(busy_msg, sizeof busy_msg, "Opening %s on %s ...", name, pc.name);
    state = UI_BUSY;
    redraw();
}

// Picks the session up again after the link dropped, without clearing the screen.
static void reattach(void) {
    sess_end();
    sess_set_target(pc.host, pc.port, pc.fp);
    sess_begin(SESS_ATTACH, last_session);
}

// ---------- Web admin ----------
// A page on the handheld for the SD card files and the clipboard. It listens on the Tailscale address, and only also on the home Wi-Fi if allowed.
enum { W_ON, W_LAN, W_ASK, W_NEWCODE, W_COUNT };
static int web_rows(void) { return webadmin_ask_code() ? W_COUNT : W_NEWCODE; }   // the new code row only matters when a code is asked for
static void web_item(int i, char *out, size_t cap) {
    if (i == W_ON) snprintf(out, cap, " Web admin             < %s >", webadmin_enabled() ? "On" : "Off");
    else if (i == W_LAN) snprintf(out, cap, " Also on the home Wi-Fi  < %s >", webadmin_lan() ? "Yes" : "No");
    else if (i == W_ASK) snprintf(out, cap, " Ask for a code         < %s >", webadmin_ask_code() ? "Yes" : "No");
    else snprintf(out, cap, " Make a new code");
}
static void draw_web(void) {
    ov_clear();
    const char *hint = " Up Down move   Left Right change   Enter select   Esc back";
    int end = orows - foot_h(hint);
    int r = header("Web admin") + 1;
    char b[200], a[4][48];
    snprintf(b, sizeof b, " Status  %s", webadmin_state_text());
    r = ov_text(r, 0, b, end);
    int n = webadmin_addresses(a);
    if (n) {
        r = ov_text(r + 1, 0, " Open one of these in a browser:", end);
        for (int i = 0; i < n; i++) { snprintf(b, sizeof b, "   %s", a[i]); r = ov_text(r, 0, b, end); }
    } else r = ov_text(r + 1, 0, " Not reachable yet. It needs Tailscale connected, or turn on the home Wi-Fi below.", end);
    if (webadmin_ask_code()) { snprintf(b, sizeof b, " The page asks for this code:  %s", webadmin_code()); r = ov_text(r + 1, 0, b, end); }
    draw_list(r + 1, end, web_rows(), web_sel, web_item);
    footer(hint);
}
static void key_web(uint16_t code) {
    if (web_sel >= web_rows()) web_sel = web_rows() - 1;
    if (code == HID_UP) web_sel = step(web_sel, -1, 0, web_rows() - 1);
    else if (code == HID_DOWN) web_sel = step(web_sel, 1, 0, web_rows() - 1);
    else if (code == HID_LEFT || code == HID_RIGHT || code == HID_ENTER) {
        if (web_sel == W_ON) webadmin_set_enabled(!webadmin_enabled());
        else if (web_sel == W_LAN) webadmin_set_lan(!webadmin_lan());
        else if (web_sel == W_ASK) webadmin_set_ask_code(!webadmin_ask_code());
        else if (code == HID_ENTER && web_sel == W_NEWCODE) { webadmin_new_code(); msg_show(UI_WEB, "New code %s. Anyone logged in on the page stays logged in until the Casio restarts", webadmin_code()); return; }
    }
    else if (code == HID_ESC) state = UI_MENU;
    redraw();
}

// ---------- Tailscale ----------
// Switch it on, join with a one-time key or by a login link, and say whether the away address may be used when the home address does not answer.
enum { T_ON, T_KEY, T_AWAY, T_FORGET, T_COUNT };
static bool ts_use_away = true;
static const char *away_for_sessions(void) {
    if (!ts_use_away || tunnel_state() != TUN_CONNECTED || !pc.away[0]) return NULL;
    bool numeric = true;
    for (const char *p = pc.away; *p; p++) if (!isdigit((unsigned char)*p) && *p != '.') numeric = false;
    if (numeric) return pc.away;
    static char resolved[24];
    return tunnel_resolve(pc.away, resolved, sizeof resolved) ? resolved : NULL;   // a Tailscale name is looked up in the list of your devices
}
static void ts_item(int i, char *out, size_t cap) {
    if (i == T_ON) snprintf(out, cap, " Tailscale        < %s >", tunnel_enabled() ? "On" : "Off");
    else if (i == T_KEY) snprintf(out, cap, " Join with a key from the Tailscale admin page");
    else if (i == T_AWAY) snprintf(out, cap, " Use the away address if home does not answer   < %s >", ts_use_away ? "Yes" : "No");
    else snprintf(out, cap, " Forget this device, wipe its Tailscale keys");
}
static void draw_ts(void) {
    ov_clear();
    const char *hint = " Up Down move   Left Right change   Enter select   Esc back";
    int r0 = header("Tailscale"), end = orows - foot_h(hint);
    char b[320], ip[24], url[200];
    snprintf(b, sizeof b, " Status  %s", tunnel_state_text());
    int r = ov_text(r0, 0, b, end);
    if (tunnel_ip(ip, sizeof ip)) { snprintf(b, sizeof b, " This device  %s", ip); r = ov_text(r, 0, b, end); }
    if (tunnel_login_url(url, sizeof url)) { snprintf(b, sizeof b, " Open this on a phone or computer: %s", url); r = ov_text(r, 0, b, end); }
    snprintf(b, sizeof b, " Away address of the computer in use: %s", pc.away[0] ? pc.away : "none. Set one in the GUI menu, Computer, by pressing E on it");
    r = ov_text(r, 0, b, end);
    draw_list(r + 1, end, T_COUNT, ts_sel, ts_item);
    footer(hint);
}
static void key_ts(uint16_t code) {
    if (code == HID_UP) ts_sel = step(ts_sel, -1, 0, T_COUNT - 1);
    else if (code == HID_DOWN) ts_sel = step(ts_sel, 1, 0, T_COUNT - 1);
    else if (code == HID_LEFT || code == HID_RIGHT || code == HID_ENTER) {
        if (ts_sel == T_ON) { tunnel_set_enabled(!tunnel_enabled()); }
        else if (ts_sel == T_AWAY) { ts_use_away = !ts_use_away; settings_set("tsaway", ts_use_away); }
        else if (code == HID_ENTER && ts_sel == T_KEY) { input_start(IN_TS_KEY, "Paste or type the one-time key, it starts with tskey-", "", true, UI_TS); redraw(); return; }
        else if (code == HID_ENTER && ts_sel == T_FORGET) { tunnel_forget(); msg_show(UI_TS, "This device is wiped here. Also delete it in the Tailscale admin page, under Machines"); return; }
    }
    else if (code == HID_ESC) state = UI_MENU;
    redraw();
}

// ---------- theme editor ----------
// Edits a working copy of a theme with the colors shown live on the whole screen. Save keeps it as one of eight custom themes, Load starts from
// any theme, Copy makes a duplicate and puts its text in the clipboard, Delete removes a saved one. Colors run 0 to 255, one arrow press is 1 and with Shift 10. The panel itself shows 32 or 64 shades per color.
enum { E_NAME, E_TR, E_TG, E_TB, E_BR, E_BG, E_BB, E_FOLLOW, E_CR, E_CG, E_CB, E_SAVE, E_LOAD, E_COPY, E_DELETE, E_RESET, E_COUNT };
static int ed_rgb[9];                 // text, background and cursor, red green blue
static char ed_name[THEME_NAME_MAX + 1];
static display_theme_t ed_theme;
static int ed_sel, ed_load_sel, ed_base;
static bool ed_follow;   // the cursor takes the text color, until a cursor color row is changed or this is switched off
static int ed_chan(int row) { return row >= E_TR && row <= E_BB ? row - E_TR : row >= E_CR && row <= E_CB ? row - E_TR - 1 : -1; }   // index in ed_rgb, -1 for other rows

static int c5(color_t c) { int v = (c >> 11) & 31; return (v << 3) | (v >> 2); }   // 5 bits back to 0 to 255, so full is 255
static int c6(color_t c) { int v = (c >> 5) & 63; return (v << 2) | (v >> 4); }
static int c5b(color_t c) { int v = c & 31; return (v << 3) | (v >> 2); }
static color_t ed_color(int k) { return RGB565(ed_rgb[k * 3], ed_rgb[k * 3 + 1], ed_rgb[k * 3 + 2]); }
static void ed_apply(void) {
    ed_theme = (display_theme_t){ed_name, ed_color(0), ed_color(1), ed_color(2)};
    display_set_theme(&ed_theme);
}
static void ed_set_name(const char *name) {
    snprintf(ed_name, sizeof ed_name, "%.19s", name);
    for (char *p = ed_name; *p; p++) if (*p == '|') *p = '-';
}
static void ed_from(const display_theme_t *t) {
    ed_rgb[0] = c5(t->fg); ed_rgb[1] = c6(t->fg); ed_rgb[2] = c5b(t->fg);
    ed_rgb[3] = c5(t->bg); ed_rgb[4] = c6(t->bg); ed_rgb[5] = c5b(t->bg);
    ed_rgb[6] = c5(t->cursor); ed_rgb[7] = c6(t->cursor); ed_rgb[8] = c5b(t->cursor);
    ed_follow = t->cursor == t->fg;
    ed_set_name(t->name);
    ed_apply();
}
static void themed_enter(void) {
    ed_base = theme_idx;
    ed_from(themes_get(theme_idx));
    ed_sel = 0;
    state = UI_THEMED;
}
static void themed_item(int i, char *out, size_t cap) {
    static const char *const LAB[] = {"Text red", "Text green", "Text blue", "Background red", "Background green", "Background blue", "Cursor red", "Cursor green", "Cursor blue"};
    if (i == E_NAME) snprintf(out, cap, " Name          %s", ed_name);
    else if (i == E_FOLLOW) snprintf(out, cap, " Cursor color    < %s >", ed_follow ? "same as the text" : "its own color");
    else if (ed_chan(i) >= 0) {
        int k = ed_chan(i), v = ed_rgb[k];
        char bar[17];
        int n = v * 16 / 256;
        for (int j = 0; j < 16; j++) bar[j] = j < n ? '#' : '-';
        bar[16] = 0;
        snprintf(out, cap, " %-17s< %3d >  %s", LAB[k], v, bar);
    }
    else if (i == E_SAVE) snprintf(out, cap, " Save as a custom theme");
    else if (i == E_LOAD) snprintf(out, cap, " Load a theme to start from");
    else if (i == E_COPY) snprintf(out, cap, " Copy, a duplicate and its text for the clipboard");
    else if (i == E_DELETE) snprintf(out, cap, " Delete this saved theme");
    else snprintf(out, cap, " Start over from the theme in use");
}
static void draw_themed(void) {
    ov_clear();
    char b[100];
    const char *hint = " Up Down move   Left Right change by 1, with Shift by 10   Enter select   Esc leave, unsaved changes are dropped";
    snprintf(b, sizeof b, "Theme editor   text #%04X  background #%04X", ed_color(0), ed_color(1));
    int r0 = header(b), end = orows - foot_h(hint);
    draw_list(r0, end, E_COUNT, ed_sel, themed_item);
    footer(hint);
}
static int ed_find_saved(void) {
    for (int i = themes_builtin_count(); i < themes_count(); i++) if (!strcmp(themes_get(i)->name, ed_name)) return i;
    return -1;
}
static void ed_save(bool as_copy) {
    if (as_copy) {
        size_t l = strlen(ed_name);
        if (l > THEME_NAME_MAX - 5) ed_name[THEME_NAME_MAX - 5] = 0;
        strcat(ed_name, " copy");
    }
    int idx = themes_custom_save(ed_name, ed_color(0), ed_color(1), ed_color(2));
    if (idx < 0) { msg_show(UI_THEMED, "All eight saved places are used. Delete one first, then save again"); return; }
    ed_set_name(themes_get(idx)->name);
    theme_idx = idx;
    ed_base = idx;
    settings_set("theme", theme_idx);
    ed_apply();
    if (as_copy) {
        char t[80];
        snprintf(t, sizeof t, "Theme %s text #%04X background #%04X cursor #%04X", ed_name, ed_color(0), ed_color(1), ed_color(2));
        clipboard_add(t);
    }
    msg_show(UI_THEMED, as_copy ? "Copied as %s, and its text is on the clipboard. It is now the theme in use." : "Saved as %s. It is now the theme in use.", ed_name);
}
static void key_themed(uint16_t code) {
    int big = shift ? 10 : 1;   // one arrow press is one number, with Shift ten
    if (code == HID_UP) ed_sel = step(ed_sel, -1, 0, E_COUNT - 1);
    else if (code == HID_DOWN) ed_sel = step(ed_sel, 1, 0, E_COUNT - 1);
    else if (code == HID_LEFT || code == HID_RIGHT) {
        int k = ed_chan(ed_sel);
        if (k >= 0) {
            ed_rgb[k] = step(ed_rgb[k], code == HID_RIGHT ? big : -big, 0, 255);
            if (k >= 6) ed_follow = false;                                      // a cursor color was chosen by hand
            else if (k < 3 && ed_follow) for (int j = 0; j < 3; j++) ed_rgb[6 + j] = ed_rgb[j];   // the cursor follows the text
            ed_apply();
        }
        else if (ed_sel == E_FOLLOW) {
            ed_follow = !ed_follow;
            if (ed_follow) for (int j = 0; j < 3; j++) ed_rgb[6 + j] = ed_rgb[j];
            ed_apply();
        }
    }
    else if (code == HID_ENTER) {
        if (ed_sel == E_FOLLOW) { ed_follow = !ed_follow; if (ed_follow) for (int j = 0; j < 3; j++) ed_rgb[6 + j] = ed_rgb[j]; ed_apply(); }
        else if (ed_sel == E_NAME) { input_start(IN_THEME_NAME, "Name for this theme", ed_name, false, UI_THEMED); redraw(); return; }
        else if (ed_sel == E_SAVE) { ed_save(false); return; }
        else if (ed_sel == E_COPY) { ed_save(true); return; }
        else if (ed_sel == E_LOAD) { ed_load_sel = theme_idx; state = UI_THEME_LOAD; display_set_theme(themes_get(ed_load_sel)); }
        else if (ed_sel == E_DELETE) {
            int i = ed_find_saved();
            if (i < 0) msg_show(UI_THEMED, "Only saved themes can be deleted, and the built-in ones stay. Choose a saved one");
            else {
                themes_custom_delete(i);
                if (theme_idx == i) theme_idx = 0; else if (theme_idx > i) theme_idx--;
                settings_set("theme", theme_idx);
                ed_base = theme_idx;
                ed_from(themes_get(theme_idx));
                msg_show(UI_THEMED, "Deleted. The theme in use is now %s.", themes_get(theme_idx)->name);
            }
        }
        else if (ed_sel == E_RESET) ed_from(themes_get(ed_base));
    }
    else if (code == HID_ESC) { display_set_theme(themes_get(theme_idx)); state = UI_MENU; }
    redraw();
}
static void theme_load_item(int i, char *out, size_t cap) {
    snprintf(out, cap, " %s%s", themes_get(i)->name, themes_is_custom(i) ? "   (saved)" : "");
}
static void draw_theme_load(void) {
    ov_clear();
    const char *hint = " Up Down preview   Enter use as the start   Esc back";
    int r0 = header("Load a theme"), end = orows - foot_h(hint);
    draw_list(r0, end, themes_count(), ed_load_sel, theme_load_item);
    footer(hint);
}
static void key_theme_load(uint16_t code) {
    int n = themes_count();
    if (code == HID_UP || code == HID_DOWN) { ed_load_sel = step(ed_load_sel, code == HID_DOWN ? 1 : -1, 0, n - 1); display_set_theme(themes_get(ed_load_sel)); }
    else if (code == HID_ENTER) { ed_from(themes_get(ed_load_sel)); state = UI_THEMED; }
    else if (code == HID_ESC) { ed_apply(); state = UI_THEMED; }
    redraw();
}

// Files the computer offers in its drop folder, pulled onto the SD card. New firmware goes to /modes and is installed from the loader.
typedef struct { char path[100]; long size; char status; } drop_ent_t;   // status: n new, c changed, s same, g just got
static drop_ent_t drops[60];
static int ndrops, drop_sel, drop_got_count;
static bool drop_all;

static void drop_item(int i, char *out, size_t cap) {
    const char *tag = drops[i].status == 'n' ? "new    " : drops[i].status == 'c' ? "changed" : drops[i].status == 'g' ? "got it " : "same   ";
    snprintf(out, cap, " %s %.50s  %ld KB", tag, drops[i].path, (drops[i].size + 1023) / 1024);
}
static void draw_drop(void) {
    ov_clear();
    char b[140];
    const char *hint = " Enter get   a get all new   r refresh   Esc back";
    int end = orows - foot_h(hint);
    snprintf(b, sizeof b, "Files from %.40s", pc.name);
    int r = header(b) + 1;
    if (ndrops == 0) {
        r = ov_text(r, 1, "Nothing in the drop folder on that computer yet.", end) + 1;
        ov_text(r, 1, "On the computer, run: a11helper send file.bin", end);
    } else draw_list(r, end, ndrops, drop_sel, drop_item);
    footer(hint);
}
static void drop_parse(void) {
    ndrops = 0;
    size_t n;
    const char *t = sess_capture_text(&n);
    for (size_t i = 0; i < n && ndrops < 60;) {
        size_t e = i;
        while (e < n && t[e] != '\n') e++;
        char line[200];
        size_t len = e - i < sizeof line - 1 ? e - i : sizeof line - 1;
        memcpy(line, t + i, len);
        line[len] = 0;
        char *tab = strchr(line, '\t');
        if (tab) {
            *tab = 0;
            drop_ent_t *d = &drops[ndrops];
            snprintf(d->path, sizeof d->path, "%.99s", line);
            d->size = atol(tab + 1);
            struct stat st;
            char full[140];
            snprintf(full, sizeof full, "/sdcard/%.120s", d->path);
            d->status = stat(full, &st) != 0 ? 'n' : (st.st_size == d->size ? 's' : 'c');
            ndrops++;
        }
        i = e + 1;
    }
    if (drop_sel >= ndrops) drop_sel = ndrops ? ndrops - 1 : 0;
}
__attribute__((unused)) static void drop_enter(void) {
    if (net_state() != NET_CONNECTED) { msg_show(UI_MENU, "Wi-Fi is not connected. Open the GUI menu, Wi-Fi, and join a network"); return; }
    if (!pc.fp[0]) { msg_show(UI_MENU, "Not paired with a computer yet. Open the GUI menu, Computer, and press A to pair"); return; }
    if (!sdcard_mount()) { msg_show(UI_MENU, "No SD card, or it is not FAT32. Put in a FAT32 card and try again"); return; }
    sess_end();
    sess_set_target(pc.host, pc.port, pc.fp);
    sess_begin(SESS_DROPLIST, NULL);
    if (sess_state() == SESS_ERROR) { msg_show(UI_MENU, "%s", sess_error()); return; }
    busy_kind = BUSY_DROPLIST;
    snprintf(busy_msg, sizeof busy_msg, "Asking %.30s for its files ...", pc.name);
    state = UI_BUSY;
}
static void drop_get(int i) {
    sess_end();
    sess_set_target(pc.host, pc.port, pc.fp);
    drop_sel = i;
    sess_begin(SESS_DROPGET, drops[i].path);
    if (sess_state() == SESS_ERROR) { msg_show(UI_DROP, "%s", sess_error()); return; }
    busy_kind = BUSY_DROPGET;
    snprintf(busy_msg, sizeof busy_msg, "Getting %.60s ...", drops[i].path);
    state = UI_BUSY;
}
static void key_drop(uint16_t code) {
    char c = a11_hid_to_char(code, shift, caps);
    if (code == HID_UP) drop_sel = step(drop_sel, -1, 0, ndrops ? ndrops - 1 : 0);
    else if (code == HID_DOWN) drop_sel = step(drop_sel, 1, 0, ndrops ? ndrops - 1 : 0);
    else if (code == HID_ENTER && ndrops) { drop_all = false; drop_got_count = 0; drop_get(drop_sel); return; }
    else if (c == 'a') {
        drop_all = true; drop_got_count = 0;
        for (int i = 0; i < ndrops; i++) if (drops[i].status == 'n' || drops[i].status == 'c') { drop_get(i); return; }
        msg_show(UI_DROP, "Nothing new to get. Everything on the computer is already on the card");
        return;
    }
    else if (c == 'r') { drop_enter(); return; }
    else if (code == HID_ESC) state = UI_MENU;
    redraw();
}
// After a file arrives or fails. In get all mode it moves on to the next one.
static void drop_after(bool ok) {
    if (ok) { drops[drop_sel].status = 'g'; drop_got_count++; }
    if (ok && drop_all) {
        for (int i = drop_sel + 1; i < ndrops; i++) if (drops[i].status == 'n' || drops[i].status == 'c') { drop_get(i); return; }
    }
    drop_all = false;
    if (ok) msg_show(UI_DROP, "Got it. To install firmware, open the GUI menu, choose Back to the loader, then pick the file");
    else msg_show(UI_DROP, "That file did not arrive complete, and nothing was kept. Try again");
}

// History of a session on the computer, the last 2000 lines tmux kept. Read only, scrolls with the same keys as the terminal.
static const char *hist_text;
static size_t hist_n;
static int32_t *hist_rows;
static int hist_nrows, hist_top;
static char hist_name[41];
static bool hist_from_term;

static void hist_build(void) {
    hist_text = sess_capture_text(&hist_n);
    free(hist_rows);
    hist_rows = NULL;
    int w = ocols > 0 ? ocols : 80, cap = 0;
    for (size_t i = 0, ls = 0; i <= hist_n; i++)
        if (i == hist_n || hist_text[i] == '\n') { size_t len = i - ls; cap += len ? (int)((len + w - 1) / w) : 1; ls = i + 1; }
    hist_rows = heap_caps_malloc(sizeof(int32_t) * (cap + 2), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    hist_nrows = 0;
    if (!hist_rows) return;
    for (size_t i = 0, ls = 0; i <= hist_n; i++) {
        if (i == hist_n || hist_text[i] == '\n') {
            size_t len = i - ls;
            if (!len) hist_rows[hist_nrows++] = (int32_t)ls;
            else for (size_t off = 0; off < len; off += w) hist_rows[hist_nrows++] = (int32_t)(ls + off);
            ls = i + 1;
        }
    }
    hist_rows[hist_nrows] = (int32_t)hist_n;
    while (hist_nrows > 0 && (hist_rows[hist_nrows - 1] >= (int32_t)hist_n || hist_text[hist_rows[hist_nrows - 1]] == '\n')) hist_nrows--;   // blank rows at the end
    int visible = orows - 2;
    hist_top = hist_nrows > visible ? hist_nrows - visible : 0;   // start at the newest
}

static void hist_scroll(int d) {
    int visible = orows - 2, maxtop = hist_nrows > visible ? hist_nrows - visible : 0;
    hist_top = step(hist_top, d, 0, maxtop);
}

static void draw_hist(void) {
    ov_clear();
    char b[100], row[200];
    int visible = orows - 2;
    int last = hist_top + visible < hist_nrows ? hist_top + visible : hist_nrows;
    snprintf(b, sizeof b, "History of %.30s   line %d of %d", hist_name, last, hist_nrows);
    header(b);
    for (int i = 0; i < visible && hist_top + i < hist_nrows; i++) {
        int32_t s0 = hist_rows[hist_top + i], e0 = hist_rows[hist_top + i + 1];
        int n = 0;
        for (int32_t k = s0; k < e0 && n < ocols && n < (int)sizeof row - 1; k++) {
            unsigned char c = (unsigned char)hist_text[k];
            if (c == '\n') break;
            row[n++] = (c < 32 || c > 126) ? ((c & 0x80) ? '?' : ' ') : (char)c;
        }
        row[n] = 0;
        ov_put(1 + i, 0, row, 0);
    }
    if (hist_nrows == 0) ov_put(2, 1, "Nothing in the history.", 0);
    footer(" Up Down scroll   PgUp PgDn page   Home End   Esc back");
}

static void hist_leave(void) {
    free(hist_rows);
    hist_rows = NULL;
    hist_nrows = 0;
    if (hist_from_term) attach_to(hist_name);   // back into the live session
    else state = UI_SESS;
}

static void key_hist(uint16_t code) {
    int page = orows - 3;
    if (code == HID_UP) hist_scroll(alt ? -page : -1);
    else if (code == HID_DOWN) hist_scroll(alt ? page : 1);
    else if (code == 75) hist_scroll(-page);          // Page Up
    else if (code == 78) hist_scroll(page);           // Page Down
    else if (code == 74 || (alt && code == HID_LEFT)) hist_top = 0;                       // Home
    else if (code == 77 || (alt && code == HID_RIGHT)) hist_scroll(hist_nrows);           // End
    else if (code == HID_ESC) { hist_leave(); redraw(); return; }
    redraw();
}

static void hist_start(const char *name, bool from_term) {
    snprintf(hist_name, sizeof hist_name, "%s", name);
    hist_from_term = from_term;
    if (state == UI_OFF) { display_set_text_buffer(ov, ocols, orows); display_set_cursor(0, 0, CURSOR_NONE); }
    sess_end();
    sess_set_target(pc.host, pc.port, pc.fp);
    sess_begin(SESS_CAPTURE, hist_name);
    if (sess_state() == SESS_ERROR) { msg_show(UI_SESS, "%s", sess_error()); return; }
    busy_kind = BUSY_HIST;
    snprintf(busy_msg, sizeof busy_msg, "Reading the history of %.30s ...", hist_name);
    state = UI_BUSY;
}

static void busy_tick(void) {
    sess_state_t st = sess_poll();
    static char shown_code[5];
    if (busy_kind == BUSY_WIFI) {
        if (net_state() == NET_CONNECTED) { sess_enter(); }
        else if (net_state() == NET_FAILED) { after_wifi = true; state = UI_WIFI; nap = 0; wifi_scan(); }
        else return;
    } else if (busy_kind == BUSY_LIST) {
        if (st == SESS_DONE) {
            nsl = sess_list(sl, 16);
            sess_end();
            sess_sel = 0;
            if (nsl == 0) { attach_to("main"); }   // nothing running yet, start the first one
            else state = UI_SESS;
        } else if (st == SESS_ERROR) { msg_show(UI_MENU, "%s", sess_error()); sess_end(); }
        else return;
    } else if (busy_kind == BUSY_PAIR) {
        if (st == SESS_DONE && sess_pair_ok()) {
            snprintf(pc.fp, sizeof pc.fp, "%s", sess_fingerprint());
            id_fp_to_code(pc.fp, pc.code, sizeof pc.code);
            id_computer_save(&pc);
            sess_end();
            sess_set_target(pc.host, pc.port, pc.fp);
            shown_code[0] = 0;
            if (last_session[0]) attach_to(last_session); else list_enter();
        } else if (st == SESS_DONE) { msg_show(UI_FIND, "The computer did not allow it. Try again and type y in the helper window"); sess_end(); shown_code[0] = 0; }
        else if (st == SESS_ERROR) { msg_show(UI_FIND, "%s", sess_error()); sess_end(); shown_code[0] = 0; }
        else {
            if (strcmp(shown_code, sess_pair_code()) != 0) { snprintf(shown_code, sizeof shown_code, "%s", sess_pair_code()); redraw(); }
            return;
        }
    } else if (busy_kind == BUSY_DROPLIST) {
        if (st == SESS_DONE) { sess_end(); drop_parse(); state = UI_DROP; }
        else if (st == SESS_ERROR && sess_auth_failed()) { auto_pair(); }
        else if (st == SESS_ERROR) { msg_show(UI_MENU, "%s", sess_error()); sess_end(); }
        else return;
    } else if (busy_kind == BUSY_DROPGET) {
        if (st == SESS_ACTIVE || st == SESS_CONNECTING) {   // show how far it has got
            static int64_t last_draw;
            int64_t now = esp_timer_get_time();
            if (now - last_draw > 400000) {
                long got, total;
                sess_drop_progress(&got, &total);
                snprintf(busy_msg, sizeof busy_msg, "Getting %.50s ... %ld of %ld KB", drops[drop_sel].path, got / 1024, total / 1024);
                last_draw = now;
                redraw();
            }
            return;
        }
        else if (st == SESS_DONE) { bool ok = sess_drop_ok(); sess_end(); drop_after(ok); }
        else if (st == SESS_ERROR && sess_auth_failed()) { auto_pair(); }
        else if (st == SESS_ERROR) { msg_show(UI_DROP, "%s", sess_error()); sess_end(); drop_all = false; }
        else return;
    } else if (busy_kind == BUSY_HIST) {
        if (st == SESS_DONE) { sess_end(); hist_build(); state = UI_HIST; }
        else if (st == SESS_ERROR && sess_auth_failed()) { auto_pair(); }
        else if (st == SESS_ERROR) { msg_show(UI_SESS, "%s", sess_error()); sess_end(); }
        else return;
    } else if (busy_kind == BUSY_KILL) {
        if (st == SESS_DONE) { sess_end(); list_enter(); return; }
        else if (st == SESS_ERROR) { msg_show(UI_SESS, "%s", sess_error()); sess_end(); }
        else return;
    } else {   // BUSY_ATTACH
        if (st == SESS_ACTIVE) { close_ui(); return; }
        else if (st == SESS_ERROR && sess_auth_failed()) { auto_pair(); }
        else if (st == SESS_ERROR) { msg_show(UI_SESS, "%s", sess_error()); sess_end(); }
        else if (st == SESS_DONE) { msg_show(UI_SESS, "The session ended. Press any key, then pick a session again from the GUI menu, Sessions"); sess_end(); }
        else return;
    }
    redraw();
}

void ui_tick(void) {
    int64_t now = esp_timer_get_time();
    static int64_t last_hb;
    if (now - last_hb > 3000000) {   // a heartbeat in the log, to see which part goes quiet when something freezes
        last_hb = now;
        ESP_LOGI(TAG, "hb state=%d sess=%d keys=%u scans=%u screen=%u cm=%d rscroll=%d io3edges=%u rdok=%u rdfail=%u io3=%d", state, sess_state(),
                 (unsigned)a11_keys_event_count(), (unsigned)a11_keys_poll_count(), (unsigned)display_frames(), cm_active, rscroll,
                 (unsigned)a11_keys_io3_edges(), (unsigned)a11_keys_read_ok(), (unsigned)a11_keys_read_fail(), a11_keys_io3_level());
    }
    if (sel_clear_at && now > sel_clear_at && !cm_active) { sel_clear_at = 0; last_tap_text[0] = 0; sel_clear(); }
    status_led_fast_sampling(state == UI_BATT);
    if (state == UI_BATT) { static int64_t last_batt; if (now - last_batt > 500000) { last_batt = now; redraw(); } }
    // Sleep after a while without keys or touch, but only on battery unless the box is unticked.
    if (!asleep && AUTOSLEEP_MIN[autosleep_idx] && now - last_input_us > AUTOSLEEP_MIN[autosleep_idx] * 60000000LL && !(nosleep_usb && on_usb_power())) {
        go_sleep();
        return;
    }
    if (state == UI_BUSY) { busy_tick(); return; }
    if (state == UI_WIFI) {
        static net_state_t last = NET_IDLE;
        if (after_wifi && net_state() == NET_CONNECTED) { after_wifi = false; sess_enter(); redraw(); return; }
        if (net_state() != last) { last = net_state(); redraw(); }
    }
    // After a restart, go back into the session that was open.
    if (!restore_done && state == UI_OFF) {
        if (net_state() == NET_CONNECTED) { restore_done = true; if (pc.fp[0] && last_session[0]) attach_to(last_session); }
        else if (now - boot_at > 25000000LL || net_state() == NET_FAILED || !net_has_saved()) restore_done = true;
    }
    if (retry_at && now > retry_at && state == UI_OFF) {
        retry_at = 0;
        reattach();
        if (sess_state() == SESS_ERROR) { sess_end(); if (++reconnects <= 8) retry_at = now + 3000000; }
    }
    if (sess_kind() == SESS_ATTACH && sess_state() != SESS_IDLE) {
        sess_state_t st = sess_poll();
        if (st == SESS_ACTIVE) reconnects = 0;
        if (st == SESS_ERROR && sess_auth_failed()) { auto_pair(); redraw(); return; }
        if (st == SESS_DONE || st == SESS_ERROR) {
            bool clean = st == SESS_DONE && sess_clean_end();
            sess_end();
            if (clean) {
                const char *m = "\r\n\x1b[33m[session ended]\x1b[0m\r\n";
                vterm_write(m, strlen(m));
                last_session[0] = 0;
                settings_set_str(ls_key(), "");
                if (state == UI_OFF) { sess_enter(); redraw(); }
            } else if (++reconnects <= 8) {
                const char *m = "\r\n\x1b[33m[connection lost, reconnecting ...]\x1b[0m\r\n";
                vterm_write(m, strlen(m));
                retry_at = now + 2500000;
            } else {
                const char *m = "\r\n\x1b[33m[could not reconnect, GUI menu then Sessions]\x1b[0m\r\n";
                vterm_write(m, strlen(m));
            }
        }
    }
}

static void ed_set_name(const char *name);
static void input_done(void) {
    switch (in_kind) {
        case IN_WIFI_PASS:
            net_connect(wifi_target, input, true);
            state = UI_WIFI;
            break;
        case IN_PC_HOST: {
            uint16_t port = 2222;
            char host[64];
            snprintf(host, sizeof host, "%.63s", input);
            char *colon = strchr(host, ':');
            if (colon) { port = (uint16_t)atoi(colon + 1); *colon = 0; }
            if (!host[0]) { state = UI_FIND; break; }
            pair_start(host, port, host);
            break;
        }
        case IN_PC_AWAY: {
            id_computer_t c;
            bool have = pc_sel == pc_slot ? (c = pc, pc.fp[0] != 0) : (id_computer_slot_load(pc_sel, &c) && c.fp[0]);
            if (!have) { state = UI_PC; break; }
            snprintf(c.away, sizeof c.away, "%.63s", input);
            id_computer_slot_save(pc_sel, &c);
            if (pc_sel == pc_slot) pc = c;
            msg_show(UI_PC, c.away[0] ? "Saved. It is used when the home address does not answer and Tailscale is connected" : "Away address cleared. The computer is reached at its home address only");
            break;
        }
        case IN_TS_KEY:
            if (strncmp(input, "tskey-", 6) != 0) { msg_show(UI_TS, "That does not look like a Tailscale key, which starts with tskey-. Check it and type it again"); break; }
            tunnel_join_with_key(input);
            msg_show(UI_TS, "Joining. This takes up to a minute. Watch the Status line on the Tailscale screen");
            break;
        case IN_THEME_NAME:
            ed_set_name(input);
            state = UI_THEMED;
            break;
        case IN_SESS_NAME:
            attach_to(input);
            break;
        case IN_PIN_NEW: {
            bool digits = input_len >= 3;
            for (int i = 0; i < input_len; i++) if (input[i] < '0' || input[i] > '9') digits = false;
            if (!digits) { msg_show(UI_SEC, "The PIN needs 3 to 15 digits. Try again"); break; }
            snprintf(new_pin, sizeof new_pin, "%.15s", input);
            input_start(IN_PIN_CONFIRM, "Type the new PIN again", "", true, UI_SEC);
            break;
        }
        case IN_PIN_CONFIRM:
            if (strcmp(input, new_pin) == 0) { if (keystore_change_pin(new_pin)) msg_show(UI_SEC, "PIN changed."); else msg_show(UI_SEC, "The PIN could not be changed because the lock is not open. Unlock it first, then try again"); }
            else msg_show(UI_SEC, "The two did not match, so the PIN was not changed. Try again");
            break;
    }
}

static void key_input(uint16_t code) {
    char c = a11_hid_to_char(code, shift, caps);
    if (code == HID_ESC) { state = in_back; }
    else if (code == HID_ENTER) { input_done(); }
    else if (code == 43 && in_secret) in_reveal = !in_reveal;
    else if (code == HID_BACKSPACE) { if (input_len) input[--input_len] = 0; }
    else if (c >= 32 && c < 127 && input_len < (int)sizeof input - 1 && input_len < 120) { input[input_len++] = c; input[input_len] = 0; }
    redraw();
}

static void key_wifi(uint16_t code) {
    char c = a11_hid_to_char(code, shift, caps);
    if (code == HID_UP) wifi_sel = step(wifi_sel, -1, 0, nap ? nap - 1 : 0);
    else if (code == HID_DOWN) wifi_sel = step(wifi_sel, 1, 0, nap ? nap - 1 : 0);
    else if (code == HID_ENTER && nap) {
        snprintf(wifi_target, sizeof wifi_target, "%s", aps[wifi_sel].ssid);
        if (aps[wifi_sel].open) { net_connect(wifi_target, "", true); }
        else { char pr[80]; snprintf(pr, sizeof pr, "Password for %.40s", wifi_target); input_start(IN_WIFI_PASS, pr, "", true, UI_WIFI); }
    }
    else if (c == 'r') wifi_scan();
    else if (c == 'f') net_forget();
    else if (code == HID_ESC) { after_wifi = false; state = UI_MENU; }
    redraw();
}

// Makes another computer the active one and lists its sessions. Each computer remembers its own last session.
static void hop_to(int slot) {
    if (slot < 0 || slot >= ID_MAX_COMPUTERS) return;
    if (slot != pc_slot) {
        id_computer_slot_save(pc_slot, &pc);   // keep the working copy
        sess_end();
        pc_slot = slot;
        id_computer_set_slot(slot);
        settings_set("pcact", slot);
        memset(&pc, 0, sizeof pc);
        id_computer_load(&pc);
        last_session[0] = 0; prev_session[0] = 0;
        settings_get_str(ls_key(), last_session, sizeof last_session);
        sess_set_target(pc.host, pc.port, pc.fp);
    }
    pc_sel = slot;
    restore_done = true;
    sess_enter();
}
static void hop_next(void) {
    for (int k = 1; k <= ID_MAX_COMPUTERS; k++) {
        int slot = (pc_slot + k) % ID_MAX_COMPUTERS;
        id_computer_t c;
        if (slot != pc_slot && id_computer_slot_load(slot, &c) && c.fp[0]) { hop_to(slot); return; }
    }
    msg_show(state == UI_OFF ? UI_OFF : UI_SESS, "Only one computer is paired. Open the GUI menu, Computer, and press A on an empty slot to add another");
}

static void key_pc(uint16_t code) {
    char c = a11_hid_to_char(code, shift, caps);
    if (code == HID_UP) pc_sel = step(pc_sel, -1, 0, ID_MAX_COMPUTERS - 1);
    else if (code == HID_DOWN) pc_sel = step(pc_sel, 1, 0, ID_MAX_COMPUTERS - 1);
    else if (code == HID_ENTER || c == 'a' || c == 'p') {
        id_computer_t sel;
        bool have = pc_sel == pc_slot ? pc.fp[0] != 0 : (id_computer_slot_load(pc_sel, &sel) && sel.fp[0]);
        if (code == HID_ENTER && have) { hop_to(pc_sel); return; }
        if (net_state() != NET_CONNECTED) msg_show(UI_PC, "Wi-Fi is not connected. Open the GUI menu, Wi-Fi, and join a network");
        else {   // add a computer, or pair this one again
            if (pc_sel != pc_slot) {
                id_computer_slot_save(pc_slot, &pc);
                sess_end();
                pc_slot = pc_sel;
                id_computer_set_slot(pc_slot);
                settings_set("pcact", pc_slot);
                memset(&pc, 0, sizeof pc);
                id_computer_load(&pc);
                last_session[0] = 0; prev_session[0] = 0;
                settings_get_str(ls_key(), last_session, sizeof last_session);
            }
            find_run();
            return;
        }
    }
    else if (c == 'e') {
        id_computer_t sel;
        bool have = pc_sel == pc_slot ? (sel = pc, pc.fp[0] != 0) : (id_computer_slot_load(pc_sel, &sel) && sel.fp[0]);
        if (!have) msg_show(UI_PC, "Pair that computer first. Press A on it in the Computer screen");
        else input_start(IN_PC_AWAY, "Away address: a Tailscale name like wozlap3, or a 100.x address. Empty clears it", sel.away, false, UI_PC);
    }
    else if (c == 'f') {
        if (pc_sel == pc_slot) { id_computer_forget(); memset(&pc, 0, sizeof pc); sess_set_target("", 2222, ""); settings_set_str(ls_key(), ""); last_session[0] = 0; }
        else id_computer_slot_forget(pc_sel);
    }
    else if (code == HID_ESC) state = UI_MENU;
    redraw();
}

static void key_find(uint16_t code) {
    char c = a11_hid_to_char(code, shift, caps);
    if (code == HID_UP) find_sel = step(find_sel, -1, 0, nfound ? nfound - 1 : 0);
    else if (code == HID_DOWN) find_sel = step(find_sel, 1, 0, nfound ? nfound - 1 : 0);
    else if (code == HID_ENTER && nfound) { pair_start(found[find_sel].host, found[find_sel].port, found[find_sel].name); }
    else if (c == 'r') { find_run(); }
    else if (c == 'a') input_start(IN_PC_HOST, "Address of the computer, like 192.168.1.60", pc.host, false, UI_FIND);
    else if (code == HID_ESC) state = UI_MENU;
    redraw();
}

static void key_sess(uint16_t code) {
    char c = a11_hid_to_char(code, shift, caps);
    if (code == HID_UP) sess_sel = step(sess_sel, -1, 0, nsl);
    else if (code == HID_DOWN) sess_sel = step(sess_sel, 1, 0, nsl);
    else if (code == HID_ENTER) {
        if (sess_sel < nsl) attach_to(sl[sess_sel].name);
        else {
            static const char *tries[] = {"main", "work", "play", "extra"};
            const char *pick = "extra";
            for (int t = 0; t < 4; t++) {
                bool used = false;
                for (int i = 0; i < nsl; i++) if (strcmp(sl[i].name, tries[t]) == 0) used = true;
                if (!used) { pick = tries[t]; break; }
            }
            input_start(IN_SESS_NAME, "Name for the new session, letters and numbers", pick, false, UI_SESS);
        }
    }
    else if (c == 'k' && sess_sel < nsl) { snprintf(kill_target, sizeof kill_target, "%s", sl[sess_sel].name); state = UI_CONFIRM; }
    else if (c == 'h' && sess_sel < nsl) { hist_start(sl[sess_sel].name, false); return; }
    else if (c == 'r') { sess_enter(); }
    else if (code == 43) { hop_next(); return; }   // next computer
    else if (code == HID_ESC) state = UI_MENU;
    redraw();
}

static void key_confirm(uint16_t code) {
    char c = a11_hid_to_char(code, shift, caps);
    if (c == 'y') {
        if (strcmp(kill_target, last_session) == 0) { last_session[0] = 0; settings_set_str(ls_key(), ""); }
        sess_end();
        sess_set_target(pc.host, pc.port, pc.fp);
        sess_begin(SESS_KILL, kill_target);
        if (sess_state() == SESS_ERROR) { msg_show(UI_SESS, "%s", sess_error()); }
        else { busy_kind = BUSY_KILL; snprintf(busy_msg, sizeof busy_msg, "Ending %s ...", kill_target); state = UI_BUSY; }
    } else if (c == 'n' || code == HID_ESC) state = UI_SESS;
    redraw();
}

static void key_busy(uint16_t code) {
    if (code == HID_ESC) { sess_end(); state = UI_MENU; }
    redraw();
}

static void key_msg(uint16_t code) { if (msg_back == UI_OFF) close_ui(); else { state = msg_back; redraw(); } }


// Editing a clipboard entry. Enter makes a new line, Ctrl S saves, Esc throws the changes away.

static int edit_layout(int width, int *starts, int max) {   // start index of each display line
    int n = 0, col = 0;
    starts[n++] = 0;
    for (int i = 0; i < edit_len && n < max; i++) {
        if (edit_buf[i] == '\n') { starts[n++] = i + 1; col = 0; }
        else if (++col == width) { starts[n++] = i + 1; col = 0; }
    }
    return n;
}

static void draw_edit(void) {
    ov_clear();
    const char *hint = " Ctrl S save   Esc throw away   Enter new line";
    int end = orows - foot_h(hint);
    int top_row = header("Edit clipboard entry");
    static int starts[CLIP_MAX_LEN + 2];
    int width = ocols - 2, rows_avail = end - top_row;
    int n = edit_layout(width, starts, CLIP_MAX_LEN + 1);
    int cur_line = 0;
    for (int l = 0; l < n; l++) if (starts[l] <= edit_pos) cur_line = l;
    if (cur_line < edit_top) edit_top = cur_line;
    if (cur_line >= edit_top + rows_avail) edit_top = cur_line - rows_avail + 1;
    for (int l = edit_top; l < n && l < edit_top + rows_avail; l++) {
        int e = (l + 1 < n) ? starts[l + 1] : edit_len;
        for (int i = starts[l], col = 0; i < e && col < width; i++) {
            if (edit_buf[i] == '\n') break;
            char one[2] = {edit_buf[i], 0};
            ov_put(top_row + l - edit_top, 1 + col++, one, 0);
        }
    }
    display_set_cursor(1 + (edit_pos - starts[cur_line]), top_row + cur_line - edit_top, CURSOR_BLOCK);
    footer(hint);
}

static void key_edit(uint16_t code) {
    static int starts[CLIP_MAX_LEN + 2];
    int width = ocols - 2;
    char c = a11_hid_to_char(code, shift, caps);
    int n = edit_layout(width, starts, CLIP_MAX_LEN + 1);
    int cur_line = 0;
    for (int l = 0; l < n; l++) if (starts[l] <= edit_pos) cur_line = l;
    int col = edit_pos - starts[cur_line];
    if (code == HID_ESC) { display_set_cursor(0, 0, CURSOR_NONE); state = UI_CLIP; }
    else if (ctrl && code == 22) {   // Ctrl S
        edit_buf[edit_len] = 0;
        if (edit_len) clipboard_set(edit_index, edit_buf);
        display_set_cursor(0, 0, CURSOR_NONE);
        state = UI_CLIP;
    }
    else if (alt && code == HID_LEFT) edit_pos = starts[cur_line];
    else if (alt && code == HID_RIGHT) { int le = (cur_line + 1 < n) ? starts[cur_line + 1] : edit_len + 1; edit_pos = le - 1 < edit_len ? le - 1 : edit_len; if (edit_pos < starts[cur_line]) edit_pos = starts[cur_line]; }
    else if (code == HID_LEFT) { if (edit_pos > 0) edit_pos--; }
    else if (code == HID_RIGHT) { if (edit_pos < edit_len) edit_pos++; }
    else if (code == HID_UP) { if (cur_line > 0) { int ls = starts[cur_line - 1], len = starts[cur_line] - ls; edit_pos = ls + (col < len ? col : (len > 0 ? len - 1 : 0)); } }
    else if (code == HID_DOWN) { if (cur_line + 1 < n) { int ls = starts[cur_line + 1], le = (cur_line + 2 < n) ? starts[cur_line + 2] : edit_len + 1; int len = le - ls; edit_pos = ls + (col < len ? col : (len > 0 ? len - 1 : 0)); if (edit_pos > edit_len) edit_pos = edit_len; } }
    else if (code == HID_BACKSPACE) { if (edit_pos > 0) { memmove(edit_buf + edit_pos - 1, edit_buf + edit_pos, edit_len - edit_pos + 1); edit_pos--; edit_len--; } }
    else if (code == HID_ENTER) { if (edit_len < CLIP_MAX_LEN) { memmove(edit_buf + edit_pos + 1, edit_buf + edit_pos, edit_len - edit_pos + 1); edit_buf[edit_pos++] = '\n'; edit_len++; } }
    else if (c >= 32 && c < 127 && edit_len < CLIP_MAX_LEN) { memmove(edit_buf + edit_pos + 1, edit_buf + edit_pos, edit_len - edit_pos + 1); edit_buf[edit_pos++] = c; edit_len++; }
    redraw();
}

static void redraw(void) {
    switch (state) {
        case UI_MENU: draw_menu(); break;
        case UI_CLIP: draw_clip(); break;
        case UI_CLIP_NEW: draw_new(); break;
        case UI_GRAB: draw_grab(); break;
        case UI_STATUS: draw_status(); break;
        case UI_BATT: draw_batt(); break;
        case UI_DROP: draw_drop(); break;
        case UI_THEMED: draw_themed(); break;
        case UI_TS: draw_ts(); break;
        case UI_WEB: draw_web(); break;
        case UI_THEME_LOAD: draw_theme_load(); break;
        case UI_HIST: draw_hist(); break;
        case UI_WIFI: draw_wifi(); break;
        case UI_INPUT: draw_input(); break;
        case UI_PC: draw_pc(); break;
        case UI_FIND: draw_find(); break;
        case UI_SESS: draw_sess(); break;
        case UI_BUSY: draw_busy(); break;
        case UI_MSG: draw_msg(); break;
        case UI_CONFIRM: draw_confirm(); break;
        case UI_SEC: draw_sec(); break;
        case UI_SEC_CAPTURE: draw_sec_capture(); break;
        case UI_LOCK: draw_lock(); break;
        case UI_CLIP_EDIT: draw_edit(); break;
        default: break;
    }
}

static void row_text(int row, char *out, size_t cap) {
    const terminal_cell_t *t = (const terminal_cell_t *)vterm_buffer() + row * term_cols;
    size_t n = 0;
    for (int c = 0; c < term_cols && n + 4 < cap; c++) {
        uint16_t cp = t[c].cp ? t[c].cp : ' ';
        if (cp < 0x80) out[n++] = (char)cp;
        else if (cp < 0x800) { out[n++] = 0xC0 | (cp >> 6); out[n++] = 0x80 | (cp & 0x3F); }
        else { out[n++] = 0xE0 | (cp >> 12); out[n++] = 0x80 | ((cp >> 6) & 0x3F); out[n++] = 0x80 | (cp & 0x3F); }
    }
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
}

// Text of columns c0 to c1 of a terminal row, trailing spaces removed.
static void row_text_range(int row, int c0, int c1, char *out, size_t cap) {
    const terminal_cell_t *t = (const terminal_cell_t *)vterm_buffer() + row * term_cols;
    size_t n = 0;
    for (int c = c0; c <= c1 && c < term_cols && n + 4 < cap; c++) {
        uint16_t cp = t[c].cp ? t[c].cp : ' ';
        if (cp < 0x80) out[n++] = (char)cp;
        else if (cp < 0x800) { out[n++] = 0xC0 | (cp >> 6); out[n++] = 0x80 | (cp & 0x3F); }
        else { out[n++] = 0xE0 | (cp >> 12); out[n++] = 0x80 | ((cp >> 6) & 0x3F); out[n++] = 0x80 | (cp & 0x3F); }
    }
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
}

// Selecting text on the screen, copying it, scrolling, and the keys that go to the session

static bool session_live(void) { return sess_state() == SESS_ACTIVE && sess_kind() == SESS_ATTACH; }

static void sel_apply(void) { display_set_selection(sel_active, sel_ar, sel_ac, sel_hr, sel_hc); }
static void sel_clear(void) { if (sel_active) { sel_active = false; display_set_selection(false, 0, 0, 0, 0); } }

static void sel_move(uint16_t code) {
    if (!sel_active) {
        int x, y;
        display_get_cursor(&x, &y);
        sel_ar = sel_hr = y; sel_ac = sel_hc = x;
        sel_active = true;
    }
    if (code == HID_RIGHT) { if (sel_hc < term_cols - 1) sel_hc++; else if (sel_hr < term_rows - 1) { sel_hr++; sel_hc = 0; } }
    else if (code == HID_LEFT) { if (sel_hc > 0) sel_hc--; else if (sel_hr > 0) { sel_hr--; sel_hc = term_cols - 1; } }
    else if (code == HID_UP) { if (sel_hr > 0) sel_hr--; }
    else if (code == HID_DOWN) { if (sel_hr < term_rows - 1) sel_hr++; }
    sel_apply();
}

static void sel_all(void) {
    sel_ar = 0; sel_ac = 0; sel_hr = term_rows - 1; sel_hc = term_cols - 1;
    sel_active = true;
    sel_apply();
}

static size_t sel_text(char *text, size_t cap) {
    int a = sel_ar * term_cols + sel_ac, b = sel_hr * term_cols + sel_hc;
    int lo = a < b ? a : b, hi = a < b ? b : a;
    int r0 = lo / term_cols, c0 = lo % term_cols, r1 = hi / term_cols, c1 = hi % term_cols;
    size_t n = 0;
    for (int r = r0; r <= r1; r++) {
        char line[400];
        row_text_range(r, r == r0 ? c0 : 0, r == r1 ? c1 : term_cols - 1, line, sizeof line);
        size_t l = strlen(line);
        if (n + l + 2 >= cap) break;
        memcpy(text + n, line, l);
        n += l;
        if (r < r1) text[n++] = '\n';
    }
    while (n && (text[n - 1] == '\n' || text[n - 1] == ' ')) n--;
    text[n] = 0;
    return n;
}

static void sel_copy(void) {
    static char text[CLIP_MAX_LEN + 1];
    if (sel_text(text, sizeof text)) clipboard_add(text);
    sel_clear();
}

// Words, lines, paragraphs and the whole screen, for taps and for the copy mode

static const terminal_cell_t *cells_of(int r) { return (const terminal_cell_t *)vterm_buffer() + r * term_cols; }

static bool is_word_cp(uint16_t cp) {
    if (cp <= 0x20) return false;
    switch (cp) {
        case '(': case ')': case '[': case ']': case '{': case '}': case '<': case '>': case '"': case '\'': case '`': case ',': case ';': case '|':
            return false;
    }
    return true;
}

static int last_text_col(int r) {
    const terminal_cell_t *t = cells_of(r);
    for (int c = term_cols - 1; c >= 0; c--) if (t[c].cp > 0x20) return c;
    return -1;
}

static void select_range(int r0, int c0, int r1, int c1) {
    sel_ar = r0; sel_ac = c0; sel_hr = r1; sel_hc = c1;
    sel_active = true;
    sel_apply();
}

// unit 2 word, 3 line, 4 paragraph, 5 whole screen. Returns false when there is no text there.
static bool select_unit(int r, int c, int unit) {
    if (r < 0 || r >= term_rows) return false;
    if (unit == 2) {
        const terminal_cell_t *t = cells_of(r);
        if (c < 0 || c >= term_cols || !is_word_cp(t[c].cp)) return false;
        int c0 = c, c1 = c;
        while (c0 > 0 && is_word_cp(t[c0 - 1].cp)) c0--;
        while (c1 < term_cols - 1 && is_word_cp(t[c1 + 1].cp)) c1++;
        select_range(r, c0, r, c1);
        return true;
    }
    if (unit == 3) {
        int e = last_text_col(r);
        if (e < 0) return false;
        select_range(r, 0, r, e);
        return true;
    }
    if (unit == 4) {
        if (last_text_col(r) < 0) return false;
        int r0 = r, r1 = r;
        while (r0 > 0 && last_text_col(r0 - 1) >= 0) r0--;
        while (r1 < term_rows - 1 && last_text_col(r1 + 1) >= 0) r1++;
        select_range(r0, 0, r1, last_text_col(r1));
        return true;
    }
    int r0 = 0, r1 = term_rows - 1;
    while (r0 <= r1 && last_text_col(r0) < 0) r0++;
    while (r1 >= r0 && last_text_col(r1) < 0) r1--;
    if (r0 > r1) return false;
    select_range(r0, 0, r1, last_text_col(r1));
    return true;
}

// Copies the selection. When a tap sequence is upgrading from a word to a line and so on, the smaller copy it made a
// moment ago is replaced instead of left behind.
static void copy_selection_tap(void) {
    static char text[CLIP_MAX_LEN + 1];
    if (!sel_text(text, sizeof text)) return;
    if (last_tap_text[0]) { int i = clipboard_find(last_tap_text); if (i >= 0) clipboard_remove(i); }
    clipboard_add(text);
    snprintf(last_tap_text, sizeof last_tap_text, "%s", text);
    sel_clear_at = esp_timer_get_time() + 1500000;
}

static void send_keys(vtkey_t k, uint8_t mods) {
    uint8_t buf[VTKEYS_MAX_LEN];
    size_t n = vtkeys_encode(k, mods, vterm_app_cursor_keys(), buf, sizeof buf);
    if (n) cb.send((const char *)buf, n);
}

// One notch of the mouse wheel, the way the program asked for it. tmux with mouse on turns it into scrolling its history.
static void wheel(bool up) {
    char b[24];
    int len;
    if (vterm_mouse_sgr()) len = snprintf(b, sizeof b, "\x1b[<%d;1;1M", up ? 64 : 65);
    else { b[0] = 27; b[1] = '['; b[2] = 'M'; b[3] = 32 + (up ? 64 : 65); b[4] = 33; b[5] = 33; len = 6; }
    cb.send(b, len);
}

static void exit_remote_scroll(void) {
    if (!rscroll) return;
    rscroll = false;
    cb.send("\x1b>", 2);   // history bottom, then one wheel step down leaves tmux scroll mode
    wheel(false);
}

// tmux redraws the whole screen for every scroll step, so scroll commands to the computer are spaced out. Without this a
// held key or a quick flick floods the link faster than the screen can take it.
static bool scroll_allowed(void) {
    static int64_t last;
    int64_t now = esp_timer_get_time();
    if (now - last < 110000) return false;
    last = now;
    return true;
}

static void scroll_lines(int dir) {   // dir 1 goes back in time
    if (session_live() && vterm_mouse_reporting()) { if (!scroll_allowed()) return; wheel(dir > 0); if (dir > 0) rscroll = true; }
    else vterm_scroll(dir);
}
static void scroll_page(int dir) {
    ESP_LOGI(TAG, "page %s, remote %d", dir > 0 ? "up" : "down", session_live() && vterm_mouse_reporting());
    if (session_live() && vterm_mouse_reporting()) { if (!scroll_allowed()) return; send_keys(dir > 0 ? VTKEY_PGUP : VTKEY_PGDN, 0); if (dir > 0) rscroll = true; }
    else vterm_scroll_page(dir);
}
static void paste(const char *text) {
    // Terminals take a carriage return for a new line. Programs that asked for bracketed paste get the text wrapped so
    // a pasted block is not taken for typing.
    char buf[CLIP_MAX_LEN + 1];
    size_t n = 0;
    for (const char *p = text; *p && n < sizeof buf - 1; p++) buf[n++] = (*p == '\n') ? '\r' : *p;
    bool wrap = sess_state() == SESS_ACTIVE && sess_kind() == SESS_ATTACH && vterm_bracketed_paste();
    if (wrap) cb.send("\x1b[200~", 6);
    cb.send(buf, n);
    if (wrap) cb.send("\x1b[201~", 6);
}

static int step(int v, int d, int lo, int hi) { v += d; return v < lo ? lo : v > hi ? hi : v; }

static void key_menu(uint16_t code) {
    switch (code) {
        case HID_UP: menu_sel = menu_sel > 0 ? menu_sel - 1 : M_COUNT - 1; break;   // wraps round, like the loader
        case HID_DOWN: menu_sel = menu_sel < M_COUNT - 1 ? menu_sel + 1 : 0; break;
        case HID_LEFT:
        case HID_RIGHT: {
            int d = code == HID_RIGHT ? 1 : -1;
            if (menu_sel == M_THEME) { theme_idx = (theme_idx + d + themes_count()) % themes_count(); apply_theme(); }
            else if (menu_sel == M_FONT) apply_font(font_idx + d);
            else if (menu_sel == M_BRIGHT) { bright_idx = step(bright_idx, d, 0, N_BRIGHT - 1); apply_brightness(); }
            else if (menu_sel == M_CAPSKEY) { capskey = (capskey + d + N_CAPS_ACTIONS) % N_CAPS_ACTIONS; settings_set("capskey", capskey); }
            else if (menu_sel == M_BG) { flat_bg = !flat_bg; display_set_flat_backgrounds(flat_bg); settings_set("flatbg", flat_bg); }
            break;
        }
        case HID_ENTER:
            if (menu_sel == M_SESS) sess_enter();
            else if (menu_sel == M_CLIP) { clip_sel = 0; state = UI_CLIP; }
            else if (menu_sel == M_WIFI) { state = UI_WIFI; nap = 0; wifi_scan(); }
            else if (menu_sel == M_PC) state = UI_PC;
            else if (menu_sel == M_THEMED) { themed_enter(); }
            else if (menu_sel == M_TS) { ts_sel = 0; state = UI_TS; }
            else if (menu_sel == M_WEB) { web_sel = 0; state = UI_WEB; }
            else if (menu_sel == M_BATT) { batt_sel = 0; state = UI_BATT; }
            else if (menu_sel == M_SEC) { sec_sel = 0; state = UI_SEC; }
            else if (menu_sel == M_STATUS) state = UI_STATUS;
            else if (menu_sel == M_FILES) { launch_app(2); return; }
            else if (menu_sel == M_LIBRARY) { if (!app_switch("library")) msg_show(UI_MENU, "The Library is not installed yet. Open the loader and install library.bin from the card"); return; }
            else if (menu_sel == M_LOADER) to_loader();
            break;
        case HID_ESC: close_ui(); return;
    }
    redraw();
}

static void key_clip(uint16_t code) {
    int n = clipboard_count();
    char c = a11_hid_to_char(code, shift, caps);
    if (code == HID_UP) clip_sel = step(clip_sel, -1, 0, n ? n - 1 : 0);
    else if (code == HID_DOWN) clip_sel = step(clip_sel, 1, 0, n ? n - 1 : 0);
    else if (code == HID_ENTER && n) { char *copy = strdup(clipboard_get(clip_sel)); close_ui(); if (copy) { paste(copy); free(copy); } return; }
    else if (code == HID_BACKSPACE && n) { clipboard_remove(clip_sel); clip_sel = step(clip_sel, 0, 0, clipboard_count() ? clipboard_count() - 1 : 0); }
    else if (c == 'n') { input_len = 0; input[0] = 0; state = UI_CLIP_NEW; }
    else if (c == 'e' && n) { edit_index = clip_sel; snprintf(edit_buf, sizeof edit_buf, "%s", clipboard_get(clip_sel)); edit_len = strlen(edit_buf); edit_pos = edit_len; edit_top = 0; state = UI_CLIP_EDIT; }
    else if (c == 'c') { grab_row = term_rows - 1; state = UI_GRAB; }
    else if (code == HID_ESC) { close_ui(); return; }
    redraw();
}

static void key_new(uint16_t code) {
    char c = a11_hid_to_char(code, shift, caps);
    if (code == HID_ESC) { display_set_cursor(0, 0, CURSOR_NONE); state = UI_CLIP; }
    else if (code == HID_ENTER) {
        if (input_len) { clipboard_add(input); clip_sel = 0; }
        display_set_cursor(0, 0, CURSOR_NONE);
        state = UI_CLIP;
    } else if (code == HID_BACKSPACE) { if (input_len) input[--input_len] = 0; }
    else if (c >= 32 && c < 127 && input_len < CLIP_MAX_LEN) { input[input_len++] = c; input[input_len] = 0; }
    redraw();
}

static void key_grab(uint16_t code) {
    if (code == HID_UP) grab_row = step(grab_row, -1, 0, term_rows - 1);
    else if (code == HID_DOWN) grab_row = step(grab_row, 1, 0, term_rows - 1);
    else if (code == HID_ENTER) {
        char text[CLIP_MAX_LEN + 1];
        row_text(grab_row, text, sizeof text);
        clipboard_add(text);
        clip_sel = 0;
        state = UI_CLIP;
    } else if (code == HID_ESC) state = UI_CLIP;
    redraw();
}

void ui_init(const ui_callbacks_t *callbacks, int *cols, int *rows) {
    cb = *callbacks;
    settings_init();
    esp_vfs_littlefs_conf_t conf = {.base_path = "/store", .partition_label = "storage", .format_if_mount_failed = true, .dont_mount = false};
    if (esp_vfs_littlefs_register(&conf) != ESP_OK) ESP_LOGW(TAG, "storage partition did not mount, clipboard will not be saved");
    clipboard_init();
    id_init();
    tunnel_init();
    webadmin_start();
    ts_use_away = settings_get("tsaway", 1) != 0;
    sess_set_away_provider(away_for_sessions);
    pc_slot = settings_get("pcact", 0);
    if (pc_slot < 0 || pc_slot >= ID_MAX_COMPUTERS) pc_slot = 0;
    id_computer_set_slot(pc_slot);
    id_computer_load(&pc);
    sess_init();
    sess_set_target(pc.host, pc.port, pc.fp);
    net_init();
    theme_idx = settings_get("theme", 0);
    font_idx = settings_get("font", 0);
    bright_idx = settings_get("bl", N_BRIGHT - 1);
    if (bright_idx < 0 || bright_idx >= N_BRIGHT) bright_idx = N_BRIGHT - 1;
    flat_bg = settings_get("flatbg", 0) != 0;
    capskey = settings_get("capskey", 0);
    if (capskey < 0 || capskey >= N_CAPS_ACTIONS) capskey = 0;
    wake_mask = settings_get("wakemask", MOD_CTRL | MOD_GUI);
    autosleep_idx = settings_get("autosleep", 5);
    if (autosleep_idx < 0 || autosleep_idx >= N_AUTOSLEEP) autosleep_idx = 5;
    nosleep_usb = settings_get("nosleepusb", 1) != 0;
    last_input_us = esp_timer_get_time();
    display_set_flat_backgrounds(flat_bg);
    settings_get_str(ls_key(), last_session, sizeof last_session);
    boot_at = esp_timer_get_time();
    display_set_theme(themes_get(theme_idx));
    display_set_backlight(BRIGHT[bright_idx]);
    status_led_set_brightness(BRIGHT[bright_idx]);
    display_set_font(font_idx, &term_cols, &term_rows);
    ov_alloc(term_cols, term_rows);
    *cols = term_cols;
    *rows = term_rows;
}


// Caps Lock, shown by the cursor shape so you can see it without looking at the keys
static void set_caps(bool on) {
    caps = on;
    display_set_cursor_override(on ? CURSOR_UNDERSCORE : -1);
}

// PIN, kept as a salted hash. Until one is set the PIN is 333.
static void to_hex(const uint8_t *in, size_t n, char *out) { for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", in[i]); }
static void pin_hash(const char *pin, const uint8_t salt[8], uint8_t out[32]) {
    uint8_t msg[8 + 16];
    size_t n = strlen(pin);
    if (n > 16) n = 16;
    memcpy(msg, salt, 8);
    memcpy(msg + 8, pin, n);
    crypto_blake2b(out, 32, msg, 8 + n);
}
static bool pin_ok(const char *pin) {
    char salt_hex[20], hash_hex[70];
    settings_get_str("pin_salt", salt_hex, sizeof salt_hex);
    settings_get_str("pin_hash", hash_hex, sizeof hash_hex);
    if (!salt_hex[0] || !hash_hex[0]) return strcmp(pin, "333") == 0;
    uint8_t salt[8], h[32];
    for (int i = 0; i < 8; i++) { unsigned v; sscanf(salt_hex + 2 * i, "%2x", &v); salt[i] = (uint8_t)v; }
    pin_hash(pin, salt, h);
    char hex[70];
    to_hex(h, 32, hex);
    return strcmp(hex, hash_hex) == 0;
}
static bool pair_notice;

// The PIN opens the keystore, which also checks it. A device from before encryption is moved over the first time its old PIN is accepted.
static bool unlock_with_pin(const char *pin) {
    if (keystore_exists()) {
        if (!keystore_unlock_pin(pin)) return false;
    } else {
        if (!pin_ok(pin)) return false;
        if (!keystore_create(pin)) return false;
        settings_set_str("pin_hash", "");   // the old unsalted-for-this-purpose check is no longer used
        settings_set_str("pin_salt", "");
    }
    id_unlock();
    if (id_key_was_replaced()) pair_notice = true;
    net_unlock();
    return true;
}

static int held_mask(void) { return (shift ? MOD_SHIFT : 0) | (ctrl ? MOD_CTRL : 0) | (alt ? MOD_ALT : 0) | (gui ? MOD_GUI : 0); }

static void mask_text(int m, char *out, size_t cap) {
    out[0] = 0;
    if (m & MOD_CTRL) strncat(out, "Ctrl+", cap - strlen(out) - 1);
    if (m & MOD_SHIFT) strncat(out, "Shift+", cap - strlen(out) - 1);
    if (m & MOD_ALT) strncat(out, "Alt+", cap - strlen(out) - 1);
    if (m & MOD_GUI) strncat(out, "GUI+", cap - strlen(out) - 1);
    strncat(out, "On", cap - strlen(out) - 1);
}

// True when the device looks to be on USB power. A computer on the USB port is certain, a wall charger is only a guess from the battery voltage rising.
static bool on_usb_power(void) {
    int mv, pct;
    return usb_serial_jtag_is_connected() || status_led_charging() || !status_led_battery(&mv, &pct);
}

static void draw_batt(void) {
    ov_clear();
    const char *hint = " Up Down move   Left Right change   Enter ticks   Esc back";
    int end = orows - foot_h(hint);
    int r = header("Battery and power") + 1;
    battery_info_t bi;
    char b[140];
    int64_t t = esp_timer_get_time();
    if (!status_led_battery_info(&bi)) {
        r = ov_text(r, 1, "No battery seen on the sense pin, running on USB power.", end);
    } else {
        snprintf(b, sizeof b, " %d.%02d V    about %d%%", bi.mv / 1000, (bi.mv % 1000) / 10, bi.pct);
        r = ov_text(r, 0, b, end);
        int w = ocols - 12;
        if (w > 40) w = 40;
        if (w < 10) w = 10;
        int fill = bi.pct * w / 100;
        char bar[64] = " [";
        int n = 2;
        for (int i = 0; i < w; i++) {
            char c = i < fill ? '#' : '-';
            if (bi.charging && i == fill && (t / 500000) % 2) c = '>';   // the head of the bar moves while charging
            bar[n++] = c;
        }
        bar[n++] = ']';
        bar[n] = 0;
        r = ov_text(r, 0, bar, end);
        r++;
        if (bi.charging) {
            snprintf(b, sizeof b, " Charging for  %d:%02d:%02d", bi.charge_secs / 3600, bi.charge_secs / 60 % 60, bi.charge_secs % 60);
            r = ov_text(r, 0, b, end);
            if (bi.rate_known) {
                snprintf(b, sizeof b, " Speed         about %d mA  (%d.%d%% an hour of %d mAh)", bi.ma, bi.rate_pct_x10_per_h / 10, bi.rate_pct_x10_per_h % 10, BOARD_BATTERY_MAH);
                r = ov_text(r, 0, b, end);
                if (bi.eta_min == 0) snprintf(b, sizeof b, " Full in        nearly full");
                else if (bi.eta_min > 0) snprintf(b, sizeof b, " Full in        about %d h %02d min", bi.eta_min / 60, bi.eta_min % 60);
                else snprintf(b, sizeof b, " Full in        too slow to tell");
                r = ov_text(r, 0, b, end);
            } else {
                r = ov_text(r, 0, " Speed         measuring, needs about 3 minutes", end);
            }
        } else if (usb_serial_jtag_is_connected()) {
            r = ov_text(r, 0, " On USB power from a computer, not charging", end);
        } else {
            r = ov_text(r, 0, " On battery", end);
        }
    }
    r += 1;
    char lab[40];
    if (AUTOSLEEP_MIN[autosleep_idx]) snprintf(lab, sizeof lab, "%d min", AUTOSLEEP_MIN[autosleep_idx]); else snprintf(lab, sizeof lab, "never");
    snprintf(b, sizeof b, " Sleep after idle on battery   < %s >", lab);
    { int r2 = ov_text(r, 0, b, end); if (batt_sel == 0) for (int i = r; i < r2; i++) ov_row_attr(i, ATTR_REVERSE); r = r2; }
    snprintf(b, sizeof b, " [%c] Do not sleep on USB power", nosleep_usb ? 'x' : ' ');
    { int r2 = ov_text(r, 0, b, end); if (batt_sel == 1) for (int i = r; i < r2; i++) ov_row_attr(i, ATTR_REVERSE); r = r2 + 1; }
    ov_text(r, 1, "Idle means no key and no touch. A wall charger is detected by the battery voltage rising, so it can take a couple of minutes.", end);
    footer(hint);
}

static void key_batt(uint16_t code) {
    if (code == HID_UP || code == HID_DOWN) batt_sel = batt_sel ? 0 : 1;
    else if (batt_sel == 0 && (code == HID_LEFT || code == HID_RIGHT || code == HID_ENTER)) {
        autosleep_idx = step(autosleep_idx, code == HID_LEFT ? -1 : 1, 0, N_AUTOSLEEP - 1);
        settings_set("autosleep", autosleep_idx);
    } else if (batt_sel == 1 && (code == HID_LEFT || code == HID_RIGHT || code == HID_ENTER || code == 44)) {
        nosleep_usb = !nosleep_usb;
        settings_set("nosleepusb", nosleep_usb);
    } else if (code == HID_ESC) { state = UI_MENU; }
    redraw();
}

static void draw_sec(void) {
    ov_clear();
    char b[100], chord[40];
    const char *hint = " Up Down move   Left Right change   Enter select   Esc back";
    int end = orows - foot_h(hint);
    int r = header("Security");
    mask_text(wake_mask, chord, sizeof chord);
    snprintf(b, sizeof b, " Secret unlock press   %s", chord);
    const char *rows[2] = {" Change PIN", b};
    for (int i = 0; i < 2; i++) {
        int r2 = ov_text(r, 0, rows[i], end);
        if (i == sec_sel) for (int k = r; k < r2; k++) ov_row_attr(k, ATTR_REVERSE);
        r = r2;
    }
    r = ov_text(r + 1, 1, "The saved SSH key and Wi-Fi password are encrypted with your PIN. A short PIN can still be guessed by someone who copies the flash, 8 or more digits protect much better.", end);
    ov_text(r + 1, 1, "Waking from Off with the secret press skips the PIN.", end);
    footer(hint);
}

static void draw_sec_capture(void) {
    ov_clear();
    const char *hint = " Hold keys then press On";
    int end = orows - foot_h(hint);
    ov_text(header("Secret unlock press") + 1, 1, "Hold the keys you want, Ctrl Shift Alt or GUI, and press On. Esc cancels.", end);
    footer(hint);
}

static void draw_lock(void) {
    ov_clear();
    const char *hint = " Type the PIN, then Enter";
    int end = orows - foot_h(hint);
    int r = header("Locked") + 2;
    r = ov_text(r, 1, "Enter your PIN", end) + 1;
    char dots[20] = "";
    for (int i = 0; i < lock_len && i < 16; i++) dots[i] = '*';
    ov_put(r, 3, dots, 0);
    int shown = lock_len < 16 ? lock_len : 16;
    if (3 + shown >= ocols) shown = ocols - 4;
    display_set_cursor(3 + shown, r, CURSOR_BLOCK);
    if (lock_msg[0]) ov_text(r + 2, 1, lock_msg, end);
    footer(hint);
}

static void lock_screen(void) {
    lock_len = 0;
    lock_buf[0] = 0;
    lock_msg[0] = 0;
    open_ui(UI_LOCK);
    redraw();
}

static void key_lock(uint16_t code) {
    int64_t now = esp_timer_get_time();
    if (code == A11_KEY_ON && keystore_unlocked() && held_mask() == wake_mask) { hold_guard = true; hold_guard_t = esp_timer_get_time(); unlock_now(); return; }   // the secret press
    if (now < lockout_until) { snprintf(lock_msg, sizeof lock_msg, "Too many tries, wait a little."); redraw(); return; }
    char c = a11_hid_to_char(code, false, false);
    if (code == HID_BACKSPACE) { if (lock_len) lock_buf[--lock_len] = 0; lock_msg[0] = 0; }
    else if (code == HID_ENTER) {
        snprintf(lock_msg, sizeof lock_msg, "Checking the PIN...");
        redraw();
        if (unlock_with_pin(lock_buf)) {
            lock_fail = 0;
            unlock_now();
            if (pair_notice) { pair_notice = false; msg_show(UI_OFF, "The device key was renewed and is now encrypted. Open the GUI menu, Computer, and press A to pair again"); redraw(); }
            return;
        }
        lock_len = 0; lock_buf[0] = 0;
        if (++lock_fail >= 5) { lock_fail = 0; lockout_until = now + 30000000LL; snprintf(lock_msg, sizeof lock_msg, "Too many tries, wait 30 seconds."); }
        else snprintf(lock_msg, sizeof lock_msg, "Wrong PIN.");
    } else if (c >= '0' && c <= '9' && lock_len < 15) { lock_buf[lock_len++] = c; lock_buf[lock_len] = 0; lock_msg[0] = 0; }
    redraw();
}

static void unlock_now(void) {
    lock_len = 0;
    lock_buf[0] = 0;
    display_set_cursor(0, 0, CURSOR_NONE);
    close_ui();
    restore_done = false;   // go back into the session that was open
    boot_at = esp_timer_get_time();
    reconnects = 0;
}

// Off: drop the connection, radio and screen. The session keeps running on the computer, On brings it back.
// A full screen app takes over keys and touch until it returns. Afterwards the terminal comes back, and whatever the app changed (theme, text size,
// brightness) is picked up from the saved settings.
static void launch_app(int which) {
    if (!cb.run_app) return;
    shift = ctrl = alt = gui = gui_tap = false;
    state = UI_OFF;
    int r = cb.run_app(which);
    shift = ctrl = alt = gui = gui_tap = false;
    int f = settings_get("font", font_idx);
    theme_idx = settings_get("theme", theme_idx);
    bright_idx = settings_get("bl", bright_idx);
    if (bright_idx < 0 || bright_idx >= N_BRIGHT) bright_idx = N_BRIGHT - 1;
    apply_theme();
    apply_brightness();
    if (f != font_idx) apply_font(f);
    close_ui();
    last_input_us = esp_timer_get_time();
    if (r == 2) go_sleep();
}

static void go_sleep(void) {
    status_led_off(true);
    sel_clear();
    sess_end();
    retry_at = 0;
    lock_screen();
    net_sleep();
    a11_keys_set_slow(true);
    display_sleep(true);
    asleep = true;
    deep_sleep_now();
}

// Off for real: the nRF is told to wake the ESP when On is pressed, then the ESP powers down. Stays in the light sleep above if that cannot be arranged.
static void deep_sleep_now(void) {
    if (!a11_keys_prepare_sleep()) { ESP_LOGW(TAG, "deep sleep not possible, staying in light sleep"); return; }
    keystore_stash();   // the secret press can still unlock after this sleep
    touch_sleep();
    display_power_off();
    a11_keys_deep_sleep();
}

static void wake(void) {
    status_led_off(false);
    asleep = false;
    a11_keys_set_slow(false);
    display_sleep(false);
    net_wake();
    if (held_mask() == wake_mask && keystore_unlocked()) { hold_guard = true; hold_guard_t = esp_timer_get_time(); unlock_now(); }   // the secret press
    else { lock_screen(); }
}

static void key_sec(uint16_t code) {
    if (code == HID_UP) sec_sel = step(sec_sel, -1, 0, 1);
    else if (code == HID_DOWN) sec_sel = step(sec_sel, 1, 0, 1);
    else if (code == HID_ENTER) {
        if (sec_sel == 0) input_start(IN_PIN_NEW, "New PIN, digits only", "", true, UI_SEC);
        else if (sec_sel == 1) { state = UI_SEC_CAPTURE; }
    }
    else if (code == HID_ESC) { state = UI_MENU; }
    redraw();
}

static void key_sec_capture(uint16_t code) {
    if (code == HID_ESC) { state = UI_SEC; }
    else if (code == A11_KEY_ON) {
        int m = held_mask();
        if (m == 0) { msg_show(UI_SEC, "Hold at least one key, then press On"); }
        else { wake_mask = m; settings_set("wakemask", m); char t[40], b[80]; mask_text(m, t, sizeof t); snprintf(b, sizeof b, "Saved. The secret press is now %s.", t); msg_show(UI_SEC, "%s", b); }
    }
    redraw();
}


// Copy mode. A cursor you move around the screen text, Shift with the arrows selects, Enter copies, Esc goes back to
// typing into the terminal.
static void cm_show(void) { display_hold_cursor(true, cm_x, cm_y, CURSOR_BLOCK); }

static void cm_exit(void) {
    cm_active = false;
    display_hold_cursor(false, 0, 0, CURSOR_NONE);
    vterm_cursor_refresh();
}

static void cm_enter(void) {
    int x, y;
    display_get_cursor(&x, &y);
    cm_x = x; cm_y = y;
    cm_active = true;
    sel_clear();
    cm_show();
}

static void cm_word(int dir) {   // jump to the start of the next or previous word
    const terminal_cell_t *t = cells_of(cm_y);
    int c = cm_x;
    if (dir > 0) {
        while (c < term_cols - 1 && is_word_cp(t[c].cp)) c++;
        while (c < term_cols - 1 && !is_word_cp(t[c].cp)) c++;
    } else {
        if (c > 0) c--;
        while (c > 0 && !is_word_cp(t[c].cp)) c--;
        while (c > 0 && is_word_cp(t[c - 1].cp)) c--;
    }
    cm_x = c;
}

static bool cm_key(uint16_t code) {
    bool arrow = code >= HID_RIGHT && code <= HID_UP;
    char c = a11_hid_to_char(code, false, false);
    if (code == HID_ESC) { sel_clear(); cm_exit(); return true; }
    if (code == HID_ENTER || (ctrl && code == 6) || c == 'y') {
        if (!sel_active) select_unit(cm_y, cm_x, 3);
        sel_copy();
        cm_exit();
        return true;
    }
    if (ctrl && (code == HID_UP || code == HID_DOWN)) { scroll_lines(code == HID_UP ? 1 : -1); return true; }
    if (alt && (code == HID_UP || code == HID_DOWN)) { scroll_page(code == HID_UP ? 1 : -1); return true; }
    if (!sel_active && shift && arrow) { sel_ar = cm_y; sel_ac = cm_x; sel_active = true; }
    if (arrow || (alt && (code == HID_LEFT || code == HID_RIGHT))) {
        if (alt && code == HID_LEFT) cm_x = 0;
        else if (alt && code == HID_RIGHT) { int e = last_text_col(cm_y); cm_x = e < 0 ? 0 : e; }
        else if (ctrl && code == HID_LEFT) cm_word(-1);
        else if (ctrl && code == HID_RIGHT) cm_word(1);
        else if (code == HID_LEFT) { if (cm_x > 0) cm_x--; else if (cm_y > 0) { cm_y--; cm_x = term_cols - 1; } }
        else if (code == HID_RIGHT) { if (cm_x < term_cols - 1) cm_x++; else if (cm_y < term_rows - 1) { cm_y++; cm_x = 0; } }
        else if (code == HID_UP) { if (cm_y > 0) cm_y--; else scroll_lines(1); }
        else if (code == HID_DOWN) { if (cm_y < term_rows - 1) cm_y++; else scroll_lines(-1); }
        if (shift && sel_active) { sel_hr = cm_y; sel_hc = cm_x; sel_apply(); }
        else if (!shift) sel_clear();
        cm_show();
        return true;
    }
    if (c == 'w') select_unit(cm_y, cm_x, 2);
    else if (c == 'l') select_unit(cm_y, cm_x, 3);
    else if (c == 'p') select_unit(cm_y, cm_x, 4);
    else if (c == 'a' || (ctrl && code == 4)) select_unit(cm_y, cm_x, 5);
    return true;
}

// Keys when no screen is open: scrolling, selecting, the clipboard keys and the session switch keys.
static bool hotkeys(uint16_t code) {
    bool arrow = code >= HID_RIGHT && code <= HID_UP;
    if (code == A11_KEY_BACKLIGHT) { bright_idx = bright_idx == 0 ? N_BRIGHT - 1 : bright_idx - 1; apply_brightness(); return true; }
    if (ctrl && (code == HID_MINUS || code == HID_EQUALS)) { apply_font(font_idx + (code == HID_EQUALS ? 1 : -1)); return true; }
    if (alt && !ctrl && code == 17) { hop_next(); redraw(); return true; }   // Alt N, the next computer
    if (alt && !ctrl && code == 11) {   // Alt H, read the history of this session
        if (sess_kind() == SESS_ATTACH && sess_state() == SESS_ACTIVE) { char n[41]; snprintf(n, sizeof n, "%s", sess_attached_name()); hist_start(n, true); redraw(); }
        return true;
    }
    if (shift && !ctrl && !alt && arrow) { sel_move(code); return true; }
    if (sel_active) {
        if (ctrl && code == 6) { sel_copy(); return true; }   // Ctrl C copies the selection
        if (ctrl && code == 4) { sel_all(); return true; }
        sel_clear();
        if (code == HID_ESC) return true;
    }
    if (ctrl && code == 4) { sel_all(); return true; }                                   // Ctrl A selects everything on screen
    if (ctrl && code == 25) { if (clipboard_count()) paste(clipboard_get(0)); return true; }   // Ctrl V pastes the newest entry
    if (ctrl && (code == HID_UP || code == HID_DOWN)) { scroll_lines(code == HID_UP ? 1 : -1); return true; }
    if (alt && (code == HID_UP || code == HID_DOWN)) { scroll_page(code == HID_UP ? 1 : -1); return true; }
    if (alt && code == 6) { cm_enter(); return true; }                                  // Alt C, copy mode
    // Alt Left and Right are Home and End. While you are scrolled back in a conversation they jump to the top and
    // back to the bottom, where typing goes on as normal.
    if (alt && code == HID_LEFT) { if (rscroll) cb.send("\x1b<", 2); else send_keys(VTKEY_HOME, 0); return true; }
    if (alt && code == HID_RIGHT) { if (rscroll) exit_remote_scroll(); else send_keys(VTKEY_END, 0); return true; }
    if (alt && code == 43) { if (prev_session[0] && pc.fp[0]) attach_to(prev_session); return true; }   // Alt Tab, the other session
    if (alt && code >= 30 && code <= 38) { int i = code - 30; if (i < nsl) attach_to(sl[i].name); return true; }   // Alt 1 to 9
    if (vterm_scroll_offset() > 0 && !ctrl && !alt) vterm_scroll_reset();   // typing brings the view back to the bottom
    if (rscroll) {   // typing leaves the scrolled back view first, Esc does it by itself
        if (code == HID_ESC) rscroll = false;
        else if (!arrow) exit_remote_scroll();
    }
    return false;
}

bool ui_handle(a11_event_t *evp) {
    int64_t now = esp_timer_get_time();
    if (evp->down) last_input_us = now;
    // The Caps key acts as whatever it was set to, the rest of the system sees that key instead.
    if (evp->code == A11_HID_CAPS_LOCK) evp->code = CAPS_ACTIONS[capskey].code;
    a11_event_t ev = *evp;
    uint16_t code = ev.code;

    // Modifiers, tracked always. Two quick taps of Shift on its own switch Caps Lock.
    if (code == A11_HID_LEFT_SHIFT) {
        shift = ev.down;
        if (ev.down) { shift_down_t = now; shift_other_key = false; }
        else if (!shift_other_key && now - shift_down_t < 300000) {
            if (last_tap_t && now - last_tap_t < 450000) { set_caps(!caps); last_tap_t = 0; }
            else last_tap_t = now;
        } else last_tap_t = 0;
    } else if (code == A11_HID_LEFT_CTRL) ctrl = ev.down;
    else if (code == A11_HID_LEFT_ALT) alt = ev.down;
    else if (code == A11_HID_LEFT_GUI) gui = ev.down;
    else if (ev.down) shift_other_key = true;
    if (hold_guard && ((!ev.down && (code == A11_HID_LEFT_CTRL || code == A11_HID_LEFT_GUI) && !ctrl && !gui) || now - hold_guard_t > 4000000)) hold_guard = false;   // both are up, or four seconds went by, shortcuts work again

    if (asleep) {   // only On does anything
        if (code == A11_KEY_ON && ev.down) wake();
        return true;
    }
    if (ev.down && code == A11_KEY_OFF) { go_sleep(); return true; }

    if (code == A11_HID_LEFT_SHIFT || code == A11_HID_LEFT_CTRL || code == A11_HID_LEFT_ALT) return state != UI_OFF;
    if (code == A11_HID_LEFT_GUI) {
        if (state == UI_LOCK || state == UI_SEC_CAPTURE) return true;
        if (ev.down) {
            gui_tap = true;
            if (hold_guard) { gui_tap = false; }   // still the keys from the secret press, not a shortcut
            else if (ctrl) {
                gui_tap = false;
                if (state == UI_CLIP) close_ui();
                else { clip_sel = 0; open_ui(UI_CLIP); redraw(); }
            }
        } else if (gui_tap) {
            gui_tap = false;
            if (state != UI_OFF) close_ui();
            else { menu_sel = 0; open_ui(UI_MENU); redraw(); }
        }
        return true;
    }
    if (ev.down) gui_tap = false;
    if (ev.down && code == A11_HID_CAPS_LOCK) { set_caps(!caps); return state != UI_OFF; }
    if (!ev.down) return state != UI_OFF;
    if (state == UI_LOCK) { key_lock(code); return true; }
    if (code == A11_KEY_ON) { if (state == UI_SEC_CAPTURE) key_sec_capture(code); return true; }
    if (gui && code == A11_HID_ESCAPE) to_loader();
    if (alt && code == 25 && state != UI_SEC_CAPTURE) {   // Alt V, the clipboard manager
        if (state == UI_CLIP) close_ui();
        else { clip_sel = 0; open_ui(UI_CLIP); redraw(); }
        return true;
    }
    // Ctrl minus and Ctrl equals change the text size in the menus too, not only in the terminal.
    if (state != UI_OFF && state != UI_BUSY && ev.down && ctrl && (code == HID_MINUS || code == HID_EQUALS)) {
        static int64_t last_font_t;
        if (now - last_font_t > 400000) { last_font_t = now; apply_font(font_idx + (code == HID_EQUALS ? 1 : -1)); if (state == UI_HIST) hist_build(); redraw(); }
        return true;
    }
    // The brightness button works in every menu too, not only in the terminal. The menu row shows the new level.
    if (state != UI_OFF && state != UI_SEC_CAPTURE && code == A11_KEY_BACKLIGHT) {
        bright_idx = bright_idx == 0 ? N_BRIGHT - 1 : bright_idx - 1;
        apply_brightness();
        redraw();
        return true;
    }
    if (state == UI_OFF && cm_active) return cm_key(code);
    if (state == UI_OFF) return hotkeys(code);
    switch (state) {
        case UI_MENU: key_menu(code); break;
        case UI_CLIP: key_clip(code); break;
        case UI_CLIP_NEW: key_new(code); break;
        case UI_GRAB: key_grab(code); break;
        case UI_STATUS: state = UI_MENU; redraw(); break;
        case UI_WIFI: key_wifi(code); break;
        case UI_INPUT: key_input(code); break;
        case UI_PC: key_pc(code); break;
        case UI_FIND: key_find(code); break;
        case UI_SESS: key_sess(code); break;
        case UI_BUSY: key_busy(code); break;
        case UI_MSG: key_msg(code); break;
        case UI_CONFIRM: key_confirm(code); break;
        case UI_BATT: key_batt(code); break;
        case UI_DROP: key_drop(code); break;
        case UI_THEMED: key_themed(code); break;
        case UI_TS: key_ts(code); break;
        case UI_WEB: key_web(code); break;
        case UI_THEME_LOAD: key_theme_load(code); break;
        case UI_HIST: key_hist(code); break;
        case UI_SEC: key_sec(code); break;
        case UI_SEC_CAPTURE: key_sec_capture(code); break;
        case UI_CLIP_EDIT: key_edit(code); break;
        default: break;
    }
    return true;
}

// Touch. In the terminal a vertical drag scrolls back through the conversation, like a mouse wheel. One step per
// three text rows of finger travel. Dragging down shows older text. In menus a drag moves the highlight.
void ui_touch(touch_event_t ev) {
    static int start_y, acc;
    last_input_us = esp_timer_get_time();
    int cw, ch;
    display_cell_size(&cw, &ch);
    int step_px = ch * 3;
    int64_t now = esp_timer_get_time();
    if (ev.type == TOUCH_DOWN) {
        start_y = ev.y; acc = 0; down_x = ev.x; down_y = ev.y; down_t = now;
        if (shift && state == UI_OFF && !asleep) {   // Shift with a drag selects text instead of scrolling
            if (cm_active) cm_exit();
            int r = ev.y / ch, c = ev.x / cw;
            if (r >= term_rows) r = term_rows - 1;
            if (c >= term_cols) c = term_cols - 1;
            sel_drag = true;
            last_tap_text[0] = 0;
            select_range(r, c, r, c);
        }
        return;
    }
    if (sel_drag) {
        if (ev.type == TOUCH_MOVE) {
            int r = ev.y / ch, c = ev.x / cw;
            sel_hr = r < 0 ? 0 : r >= term_rows ? term_rows - 1 : r;
            sel_hc = c < 0 ? 0 : c >= term_cols ? term_cols - 1 : c;
            sel_apply();
        } else if (ev.type == TOUCH_UP) { sel_drag = false; copy_selection_tap(); last_tap_text[0] = 0; }
        return;
    }
    if (ev.type == TOUCH_UP) {
        // A quick touch that barely moved is a tap. Taps in a row copy a word, a line, a paragraph, then everything.
        bool tap = abs(ev.x - down_x) < 14 && abs(ev.y - down_y) < 14 && now - down_t < 450000;
        if (!tap || (state != UI_OFF) || asleep) return;
        bool again = last_tap_time && now - last_tap_time < 450000 && abs(ev.x - last_tap_x) < 40 && abs(ev.y - last_tap_y) < 40;
        tap_count = again ? (tap_count < 5 ? tap_count + 1 : 5) : 1;
        last_tap_time = now; last_tap_x = ev.x; last_tap_y = ev.y;
        if (tap_count == 1) { if (cm_active) cm_exit(); sel_clear(); last_tap_text[0] = 0; return; }
        if (cm_active) cm_exit();
        if (tap_count == 2) last_tap_text[0] = 0;
        if (select_unit(down_y / ch, down_x / cw, tap_count)) copy_selection_tap();
        return;
    }
    if (ev.type != TOUCH_MOVE) return;
    acc += ev.y - start_y;
    start_y = ev.y;
    while (acc >= step_px || acc <= -step_px) {
        int dir = acc > 0 ? 1 : -1;   // finger down, older text
        acc -= dir * step_px;
        if (state == UI_OFF) scroll_lines(dir > 0 ? 3 : -3);
        else if (state == UI_MENU) { menu_sel = step(menu_sel, -dir, 0, M_COUNT - 1); redraw(); }
        else if (state == UI_HIST) { hist_scroll(-dir * 3); redraw(); }
        else if (state == UI_CLIP) { clip_sel = step(clip_sel, -dir, 0, clipboard_count() ? clipboard_count() - 1 : 0); redraw(); }
        else if (state == UI_SESS) { sess_sel = step(sess_sel, -dir, 0, nsl); redraw(); }
        else if (state == UI_WIFI) { wifi_sel = step(wifi_sel, -dir, 0, nap ? nap - 1 : 0); redraw(); }
    }
}

void ui_ready(void) {
    bool opened = false;
    if (esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_EXT1)) {   // woken from Off by the On key
        uint16_t codes[16];
        int n = a11_keys_wake_held(codes, 16), m = 0;
        for (int i = 0; i < n; i++) m |= codes[i] == A11_HID_LEFT_SHIFT ? MOD_SHIFT : codes[i] == A11_HID_LEFT_CTRL ? MOD_CTRL : codes[i] == A11_HID_LEFT_ALT ? MOD_ALT : codes[i] == A11_HID_LEFT_GUI ? MOD_GUI : 0;
        if (n >= 0 && m == wake_mask && keystore_take_stash()) { id_unlock(); net_unlock(); opened = true; hold_guard = true; hold_guard_t = esp_timer_get_time(); }   // the secret press, no PIN
    }
    keystore_wipe_stash();   // anything but the secret press leaves nothing behind
    if (!opened) lock_screen();
}
