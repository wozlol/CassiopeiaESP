#include "sessions.h"
#include "brand.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "a11_id.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "net.h"
#include "ssh_client.h"
#include "esp_heap_caps.h"
#include <sys/stat.h>
#include "syncd.h"
#include "vterm.h"
#include "status_led.h"

static const char *TAG = "sess";
#define CONNECT_TIMEOUT_US (12 * 1000000LL)
#define PAIR_WAIT_US (100 * 1000000LL)

static sess_state_t state = SESS_IDLE;
static sess_kind_t kind;
static ssh_config_t cfg;
static char host[64], user[8], cmd[160], expect_fp[65], error[240], fp_seen[65], attached[41], pair_code[5];
static char capture[2048];
static size_t cap_len;
static uint16_t port = 2222;
static bool clean_end, auth_failed;
static int64_t deadline;
static volatile bool sink_closed;

#define HIST_MAX (256 * 1024)
static char *hist;
static size_t hist_len;

const char *sess_capture_text(size_t *len) { if (len) *len = hist_len; return hist ? hist : ""; }

static void hist_add(const char *buf, size_t len) {
    if (!hist) hist = heap_caps_malloc(HIST_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!hist) return;
    if (len > HIST_MAX / 2) { buf += len - HIST_MAX / 2; len = HIST_MAX / 2; }
    if (hist_len + len > HIST_MAX) {   // keep the newest half
        size_t keep = HIST_MAX / 2;
        memmove(hist, hist + hist_len - keep, keep);
        hist_len = keep;
    }
    memcpy(hist + hist_len, buf, len);
    hist_len += len;
    hist[hist_len] = 0;
}

// ----- drop folder download -----
static char drop_path[160], drop_hdr[48];
static FILE *drop_f;
static long drop_size, drop_got;
static int drop_hdr_n;
static bool drop_ok, drop_open_failed;

static void mkdirs_for(const char *file) {   // makes the folders above /sdcard/<path>
    char p[220];
    snprintf(p, sizeof p, "/sdcard/%s", file);
    for (char *q = p + 8; *q; q++) if (*q == '/') { *q = 0; mkdir(p, 0777); *q = '/'; }
}
static void drop_reset(void) {
    if (drop_f) { fclose(drop_f); drop_f = NULL; }
    drop_size = drop_got = 0; drop_hdr_n = 0; drop_ok = false; drop_open_failed = false;
}
static void drop_data(const char *buf, size_t len) {
    size_t i = 0;
    while (drop_f == NULL && !drop_open_failed && i < len && drop_hdr_n < (int)sizeof drop_hdr - 1) {   // the first line says how big the file is
        char c = buf[i++];
        if (c == '\n') {
            drop_hdr[drop_hdr_n] = 0;
            long n = -1;
            if (sscanf(drop_hdr, "A11DROP %ld", &n) == 1 && n >= 0) {
                drop_size = n;
                mkdirs_for(drop_path);
                char tmp[240];
                snprintf(tmp, sizeof tmp, "/sdcard/%s.part", drop_path);
                drop_f = fopen(tmp, "wb");
                if (!drop_f) drop_open_failed = true;
            } else drop_open_failed = true;
            break;
        }
        drop_hdr[drop_hdr_n++] = c;
    }
    if (!drop_f) return;
    if (i < len) {
        size_t n = len - i;
        if (drop_got + (long)n > drop_size) n = (size_t)(drop_size - drop_got);
        if (n && fwrite(buf + i, 1, n, drop_f) != n) { drop_open_failed = true; fclose(drop_f); drop_f = NULL; return; }
        drop_got += (long)n;
    }
}
static void drop_finish(void) {
    if (!drop_f) return;
    fclose(drop_f);
    drop_f = NULL;
    char tmp[240], fin[240];
    snprintf(tmp, sizeof tmp, "/sdcard/%s.part", drop_path);
    snprintf(fin, sizeof fin, "/sdcard/%s", drop_path);
    if (drop_got == drop_size) { remove(fin); drop_ok = rename(tmp, fin) == 0; }
    else remove(tmp);
}
void sess_drop_progress(long *got, long *total) { if (got) *got = drop_got; if (total) *total = drop_size; }
bool sess_drop_ok(void) { return drop_ok; }

static void sink_data(const char *buf, size_t len, void *u) {
    if (kind == SESS_SYNC) { syncd_feed(buf, len); return; }
    if (kind == SESS_DROPGET) { drop_data(buf, len); return; }
    if (kind == SESS_CAPTURE || kind == SESS_DROPLIST) { hist_add(buf, len); return; }
    if (kind == SESS_ATTACH) { vterm_feed(buf, len); status_led_output(); }
    else { size_t n = len < sizeof capture - 1 - cap_len ? len : sizeof capture - 1 - cap_len; memcpy(capture + cap_len, buf, n); cap_len += n; capture[cap_len] = 0; }
}
static void sink_flush(void *u) { if (kind == SESS_ATTACH) vterm_flush(); }
static void sink_closed_cb(bool clean, void *u) { if (kind == SESS_DROPGET) drop_finish(); clean_end = clean; sink_closed = true; }
static const ssh_sink_t sink = {sink_data, sink_flush, sink_closed_cb, NULL};

void sess_init(void) { ssh_client_init(); }

// The second address for the same computer, asked for each time a connection starts. NULL or empty means none.
static const char *(*away_provider)(void);
void sess_set_away_provider(const char *(*fn)(void)) { away_provider = fn; }
bool sess_via_away(void) { return ssh_client_used_alt(); }

void sess_set_target(const char *h, uint16_t p, const char *fp) {
    snprintf(host, sizeof host, "%s", h);
    port = p ? p : 2222;
    snprintf(expect_fp, sizeof expect_fp, "%s", fp ? fp : "");
}

static void launch(bool with_expected) {
    int cols = 80, rows = 24;
    display_get_grid(&cols, &rows);
    memset(&cfg, 0, sizeof cfg);
    cfg.host = host;
    cfg.port = port;
    cfg.username = user;
    cfg.sink = &sink;
    cfg.term_cols = cols;
    cfg.term_rows = rows;
    cfg.command = cmd;
    if (kind != SESS_PAIR) cfg.private_key_pem = id_private_pem();   // pairing knocks without a key
    cfg.expected_fp = (with_expected && expect_fp[0]) ? expect_fp : NULL;
    static char alt[64];
    const char *a = (kind != SESS_PAIR && away_provider) ? away_provider() : NULL;
    snprintf(alt, sizeof alt, "%s", a ? a : "");
    cfg.alt_host = alt[0] && strcmp(alt, host) != 0 ? alt : NULL;   // when the home address does not answer in 3 seconds
    cfg.first_timeout_ms = 3000;
    sink_closed = false;
    cap_len = 0;
    capture[0] = 0;
    hist_len = 0;
    clean_end = false;
    error[0] = 0;
    auth_failed = false;
    state = SESS_CONNECTING;
    deadline = esp_timer_get_time() + CONNECT_TIMEOUT_US + (cfg.alt_host ? 10 * 1000000LL : 0);   // room for the second address
    if (ssh_client_connect_start(&cfg) != ESP_OK) { snprintf(error, sizeof error, "Still finishing the last try. Wait a few seconds and try again"); state = SESS_ERROR; }
}

void sess_begin(sess_kind_t k, const char *arg) {
    kind = k;
    pair_code[0] = 0;
    error[0] = 0;
    auth_failed = false;
    if (!host[0]) { snprintf(error, sizeof error, "No computer set up yet. Open the GUI menu, Computer, and add one"); state = SESS_ERROR; return; }
    if (net_state() != NET_CONNECTED) { snprintf(error, sizeof error, "Wi-Fi is not connected. Open the GUI menu, Wi-Fi, and join a network"); state = SESS_ERROR; return; }
    if (k == SESS_PAIR) {
        snprintf(user, sizeof user, "pair");
        char c[140];
        id_pairing_command("A-11", c, sizeof c);
        snprintf(cmd, sizeof cmd, "%s", c);
        expect_fp[0] = 0;   // first contact, the fingerprint comes back and goes into the code
        launch(false);
    } else {
        if (!expect_fp[0]) { snprintf(error, sizeof error, "Not paired with this computer yet. Open the GUI menu, Computer, and press A to pair"); state = SESS_ERROR; return; }
        snprintf(user, sizeof user, "a11");
        if (k == SESS_ATTACH) { snprintf(cmd, sizeof cmd, "attach %s", arg); snprintf(attached, sizeof attached, "%s", arg); }
        else if (k == SESS_KILL) snprintf(cmd, sizeof cmd, "kill %s", arg);
        else if (k == SESS_CAPTURE) snprintf(cmd, sizeof cmd, "capture %s 2000", arg);
        else if (k == SESS_SYNC) snprintf(cmd, sizeof cmd, "sync");
        else if (k == SESS_DROPLIST) snprintf(cmd, sizeof cmd, "drop list");
        else if (k == SESS_DROPGET) { drop_reset(); snprintf(drop_path, sizeof drop_path, "%.159s", arg); snprintf(cmd, sizeof cmd, "drop get %s", arg); }
        else snprintf(cmd, sizeof cmd, "list");
        launch(true);
    }
}

static void timed_out(void) {
    ssh_client_connect_cancel();
    snprintf(error, sizeof error, "No answer from %s. Check that the computer is awake and the helper is running, then try again", host);
    state = SESS_ERROR;
}

sess_state_t sess_poll(void) {
    if (state == SESS_CONNECTING) {
        if (ssh_client_connect_ready()) {
            esp_err_t rc = ssh_client_connect_take_result();
            if (rc == ESP_OK) {
                state = SESS_ACTIVE;
                if (kind == SESS_SYNC) syncd_begin(BRAND_HOST);
                snprintf(fp_seen, sizeof fp_seen, "%s", ssh_client_get_fingerprint());
                if (kind == SESS_PAIR) deadline = esp_timer_get_time() + PAIR_WAIT_US;   // waiting for a person to say yes
            } else if (rc == SSH_ERR_HOSTKEY_UNKNOWN && kind == SESS_PAIR) {
                // First contact. The code both screens show is made from this fingerprint, so a computer in the
                // middle would show a different code. Carry on, the person compares the codes.
                snprintf(fp_seen, sizeof fp_seen, "%s", ssh_client_get_fingerprint());
                id_pair_code(fp_seen, pair_code);
                snprintf(expect_fp, sizeof expect_fp, "%s", fp_seen);
                launch(true);
            } else if (rc == SSH_ERR_HOSTKEY_UNKNOWN || rc == SSH_ERR_HOSTKEY_MISMATCH) {
                snprintf(error, sizeof error, "This does not look like your computer any more. Open the GUI menu, Computer, and press A to pair again");
                state = SESS_ERROR;
            } else if (rc == SSH_ERR_AUTH) {
                auth_failed = kind != SESS_PAIR;
                snprintf(error, sizeof error, kind == SESS_PAIR ? "The computer refused the pairing request. Try again and type y in the helper window" : "The computer does not know this device. Open the GUI menu, Computer, and press A to pair again");
                state = SESS_ERROR;
            } else {
                snprintf(error, sizeof error, "%s", ssh_client_last_error());
                if (!error[0]) snprintf(error, sizeof error, "Could not connect. Check the Wi-Fi and that the helper is running, then try again");
                state = SESS_ERROR;
            }
        } else if (esp_timer_get_time() > deadline) {
            timed_out();
        }
    } else if (state == SESS_ACTIVE) {
        if (!ssh_client_is_connected() || sink_closed) state = SESS_DONE;
        else if (kind == SESS_PAIR && esp_timer_get_time() > deadline) {
            ssh_client_disconnect();
            snprintf(error, sizeof error, "Nobody allowed it on the computer in time. Try again and type y in the helper window");
            state = SESS_ERROR;
        }
    }
    return state;
}

void sess_end(void) {
    if (kind == SESS_SYNC) syncd_end();
    if (ssh_client_connect_pending()) ssh_client_connect_cancel();
    if (ssh_client_is_connected()) ssh_client_disconnect();
    state = SESS_IDLE;
}

sess_state_t sess_state(void) { return state; }
sess_kind_t sess_kind(void) { return kind; }
const char *sess_error(void) { return error; }
bool sess_auth_failed(void) { return auth_failed; }
const char *sess_fingerprint(void) { return fp_seen; }
const char *sess_attached_name(void) { return attached; }
const char *sess_pair_code(void) { return pair_code; }
bool sess_pair_ok(void) { return strstr(capture, "ok") != NULL; }
bool sess_clean_end(void) { return clean_end; }

int sess_list(sess_info_t *out, int max) {
    int n = 0;
    char *save = NULL;
    for (char *line = strtok_r(capture, "\r\n", &save); line && n < max; line = strtok_r(NULL, "\r\n", &save)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        int w = 0, a = 0;
        sscanf(tab + 1, "%d\t%d", &w, &a);
        snprintf(out[n].name, sizeof out[n].name, "%.40s", line);
        out[n].windows = w;
        out[n].attached = a;
        n++;
    }
    return n;
}

bool sess_send(const char *data, size_t len) {
    if (state != SESS_ACTIVE || kind != SESS_ATTACH) return false;
    return ssh_client_send((const uint8_t *)data, len) >= 0;
}
