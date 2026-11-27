#include "syncd.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef SYNCD_HOST   // built on a computer for testing, with the card in a folder and the sending captured
#include "syncd_host.h"
#else
#include "clipboard.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdcard.h"
#include "ssh_client.h"
#define SYNC_ROOT SDCARD_MOUNT
static int ssh_send_part(const uint8_t *d, size_t n) { return ssh_client_send(d, n); }   // bytes taken, or -1 when there is no room
static bool ssh_up(void) { return ssh_client_is_connected(); }
static void sync_delay_ms(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
#endif

#define LINE_MAX_LEN 2300

typedef enum { M_LINE, M_PAYLOAD, M_DISCARD } mode_t_;
static mode_t_ mode;
static char line[LINE_MAX_LEN];
static size_t line_len;
static FILE *put_f;
static char put_part[300], put_final[256], put_card[200];
static long put_total, put_done, put_left;
static char last_msg[120] = "Waiting for the computer";
static int ops;
static char xfer_name[100];
static long xfer_done, xfer_total;
static volatile bool xfer_on;

void syncd_end(void) { if (put_f) { fclose(put_f); put_f = NULL; } xfer_on = false; mode = M_LINE; line_len = 0; }
const char *syncd_last(void) { return last_msg; }
int syncd_ops(void) { return ops; }
bool syncd_transfer(char *name, size_t cap, long *done, long *total) {
    if (!xfer_on) return false;
    if (name) snprintf(name, cap, "%s", xfer_name);
    if (done) *done = xfer_done;
    if (total) *total = xfer_total;
    return true;
}
static void note(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_msg, sizeof last_msg, fmt, ap);
    va_end(ap);
}

// ---- sending. Replies come from the worker, never from inside the reader, because the reader holds the connection's lock.
static bool send_all(const char *d, size_t n) {
    size_t off = 0;
    int waited = 0;
    while (off < n) {
        int k = ssh_send_part((const uint8_t *)d + off, n - off);
        if (k > 0) { off += (size_t)k; waited = 0; continue; }
        if (!ssh_up() || ++waited > 3000) return false;   // about 30 seconds without room
        sync_delay_ms(10);
    }
    return true;
}
static void sendf(const char *fmt, ...) {
    char b[400];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) send_all(b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}

// ---- paths. Only modes, library and media on the card, plain characters, no way out.
static bool path_ok(const char *p) {
    size_t n = strlen(p);
    if (n < 7 || n > 160) return false;
    if (strncmp(p, "modes/", 6) && strncmp(p, "library/", 8) && strncmp(p, "media/", 6) && strncmp(p, "downloads/", 10)) return false;
    if (strstr(p, "..") || p[n - 1] == '/') return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (!(isalnum(c) || c == '.' || c == '_' || c == '+' || c == '(' || c == ')' || c == '\'' || c == ' ' || c == ',' || c == '-' || c == '/')) return false;
    }
    return true;
}
static void full_path(const char *card, char *out, size_t cap) { snprintf(out, cap, "%s/%s", SYNC_ROOT, card); }
static void mkdirs_for(const char *full) {
    char p[300];
    snprintf(p, sizeof p, "%s", full);
    for (char *q = p + strlen(SYNC_ROOT) + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(p, 0777); *q = '/'; }
}

static void esc_out(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; *s && o + 3 < cap; s++) {
        if (*s == '\\') { out[o++] = '\\'; out[o++] = '\\'; }
        else if (*s == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (*s == '\t') { out[o++] = '\\'; out[o++] = 't'; }
        else if (*s != '\r') out[o++] = *s;
    }
    out[o] = 0;
}
static void unesc_in(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; *s && o + 1 < cap; s++) {
        if (*s == '\\' && s[1]) { s++; out[o++] = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s; }
        else out[o++] = *s;
    }
    out[o] = 0;
}

// ---- LIST, GET, RM, RMDIR, CLIP, INSTALL. They run on the worker.
static int list_walk(const char *base, const char *rel, int depth, int *count) {
    char dirp[400];
    snprintf(dirp, sizeof dirp, "%s/%s%s%s", SYNC_ROOT, base, rel[0] ? "/" : "", rel);
    DIR *d = opendir(dirp);
    if (!d) return 0;
    struct dirent *e;
    char buf[1200];
    size_t bl = 0;
    while ((e = readdir(d)) != NULL && *count < 6000) {
        if (e->d_name[0] == '.') continue;
        char relp[300], fp[760];
        snprintf(relp, sizeof relp, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        snprintf(fp, sizeof fp, "%s/%s", dirp, e->d_name);
        struct stat st;
        if (stat(fp, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 6) list_walk(base, relp, depth + 1, count);
        } else if (S_ISREG(st.st_mode)) {
            size_t len = strlen(relp);
            if (len > 5 && !strcmp(relp + len - 5, ".part")) continue;
            int n = snprintf(buf + bl, sizeof buf - bl, "F\t%s\t%ld\n", relp, (long)st.st_size);
            if (n > 0 && bl + (size_t)n < sizeof buf) bl += (size_t)n;
            if (bl > sizeof buf - 340) { send_all(buf, bl); bl = 0; }
            (*count)++;
        }
    }
    if (bl) send_all(buf, bl);
    closedir(d);
    return 1;
}
static void cmd_list(const char *dir) {
    if (!(path_ok(dir) || !strcmp(dir, "modes") || !strcmp(dir, "library") || !strcmp(dir, "media") || !strcmp(dir, "downloads"))) { sendf("ERR\tbad folder\n"); return; }
    int count = 0;
    list_walk(dir, "", 0, &count);
    sendf("END\n");
    note("Listed %s, %d files", dir, count);
}
static void cmd_get(const char *card) {
    char full[300];
    if (!path_ok(card)) { sendf("ERR\tbad path\n"); return; }
    full_path(card, full, sizeof full);
    FILE *f = fopen(full, "rb");
    struct stat st;
    if (!f || stat(full, &st) != 0 || !S_ISREG(st.st_mode)) { if (f) fclose(f); sendf("ERR\tno such file\n"); return; }
    sendf("DATA\t%ld\n", (long)st.st_size);
    snprintf(xfer_name, sizeof xfer_name, "%.99s", card);
    xfer_total = st.st_size; xfer_done = 0; xfer_on = true;
    static char chunk[4096];
    size_t n;
    bool ok = true;
    while (ok && (n = fread(chunk, 1, sizeof chunk, f)) > 0) { ok = send_all(chunk, n); xfer_done += (long)n; }
    fclose(f);
    xfer_on = false;
    if (ok) sendf("OK\n");
    note("%s %s", ok ? "Sent" : "Failed sending", card);
}
static void rm_tree(const char *path, int depth) {
    DIR *d = opendir(path);
    if (!d) { remove(path); return; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[460];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode) && depth < 6) rm_tree(sub, depth + 1);
        else remove(sub);
    }
    closedir(d);
    remove(path);
}
static void cmd_rm(const char *card, bool tree) {
    char full[300];
    if (!path_ok(card)) { sendf("ERR\tbad path\n"); return; }
    full_path(card, full, sizeof full);
    struct stat st;
    if (stat(full, &st) != 0) { sendf("ERR\tno such file or folder\n"); return; }
    if (tree) rm_tree(full, 0); else remove(full);
    bool gone = stat(full, &st) != 0;
    sendf(gone ? "OK\n" : "ERR\tcould not remove it\n");
    note("%s %s", gone ? "Removed" : "Could not remove", card);
}
static void cmd_clip(char *f[], int nf) {
    if (nf < 2) { sendf("ERR\tbad clip command\n"); return; }
    if (!strcmp(f[1], "LIST")) {
        char t[1100], e[2300];
        for (int i = 0; i < clipboard_count(); i++) {
            snprintf(t, sizeof t, "%s", clipboard_get(i));
            esc_out(t, e, sizeof e);
            sendf("C\t%d\t%.380s\n", i + 1, e);
        }
        sendf("END\n");
        note("Clipboard listed, %d clips", clipboard_count());
    } else if (!strcmp(f[1], "ADD") && nf >= 3) {
        char t[1100];
        unesc_in(f[2], t, sizeof t);
        bool ok = clipboard_add(t);
        sendf(ok ? "OK\n" : "ERR\tcould not add it, it is empty or too long\n");
        note("Clip added");
    } else if (!strcmp(f[1], "DEL") && nf >= 3) {
        int n = atoi(f[2]);
        if (n < 1 || n > clipboard_count()) { sendf("ERR\tno clip %d\n", n); return; }
        clipboard_remove(n - 1);
        sendf("OK\n");
        note("Clip %d deleted", n);
    } else if (!strcmp(f[1], "SET") && nf >= 4) {
        int n = atoi(f[2]);
        char t[1100];
        unesc_in(f[3], t, sizeof t);
        if (n < 1 || n > clipboard_count() || !clipboard_set(n - 1, t)) { sendf("ERR\tcould not change clip %d\n", n); return; }
        sendf("OK\n");
        note("Clip %d changed", n);
    } else sendf("ERR\tbad clip command\n");
}
static void cmd_install(const char *card, int slot) {
    char full[300];
    if (!path_ok(card) || slot < 1 || slot > 4) { sendf("ERR\tbad file or slot\n"); return; }
    full_path(card, full, sizeof full);
#ifdef SYNCD_HOST
    sendf("OK\tpretend installed %s in slot %d\n", card, slot);
#else
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *dst = esp_partition_find_first(ESP_PARTITION_TYPE_APP, (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_0 + slot - 1), NULL);
    if (!dst) { sendf("ERR\tno such slot\n"); return; }
    if (dst == run) { sendf("ERR\tslot %d is the one running. Open the loader on the handheld and install it from there\n", slot); return; }
    FILE *f = fopen(full, "rb");
    struct stat st;
    if (!f || stat(full, &st) != 0) { if (f) fclose(f); sendf("ERR\tno such file\n"); return; }
    if ((size_t)st.st_size > dst->size) { fclose(f); sendf("ERR\tthe file is bigger than the slot\n"); return; }
    esp_ota_handle_t h;
    note("Installing %s in slot %d", card, slot);
    if (esp_ota_begin(dst, (size_t)st.st_size, &h) != ESP_OK) { fclose(f); sendf("ERR\tcould not start the install\n"); return; }
    static uint8_t buf[4096];
    size_t n;
    bool ok = true;
    while (ok && (n = fread(buf, 1, sizeof buf, f)) > 0) ok = esp_ota_write(h, buf, n) == ESP_OK;
    fclose(f);
    if (!ok || esp_ota_end(h) != ESP_OK) { sendf("ERR\tthe file is not a valid image, nothing was kept\n"); return; }
    esp_app_desc_t d;
    if (esp_ota_get_partition_description(dst, &d) == ESP_OK) { sendf("OK\t%s %s installed in slot %d\n", d.project_name, d.version, slot); note("Installed %s in slot %d", d.project_name, slot); }
    else sendf("OK\tinstalled in slot %d\n", slot);
#endif
}

static void run_line(char *l) {
    char *f[6];
    int nf = 0;
    for (char *p = l; nf < 6;) {
        f[nf++] = p;
        char *t = strchr(p, '\t');
        if (!t) break;
        *t = 0;
        p = t + 1;
    }
    if (!strcmp(f[0], "PING")) sendf("PONG\n");
    else if (!strcmp(f[0], "LIST") && nf >= 2) cmd_list(f[1]);
    else if (!strcmp(f[0], "GET") && nf >= 2) cmd_get(f[1]);
    else if (!strcmp(f[0], "RM") && nf >= 2) cmd_rm(f[1], false);
    else if (!strcmp(f[0], "RMDIR") && nf >= 2) cmd_rm(f[1], true);
    else if (!strcmp(f[0], "CLIP")) cmd_clip(f, nf);
    else if (!strcmp(f[0], "INSTALL") && nf >= 3) cmd_install(f[1], atoi(f[2]));
    else sendf("ERR\tunknown command\n");
    ops++;
}

// ---- the reader side: parses lines, takes file bytes straight to the card, hands every reply to the worker
static void job(const char *text);

static void finish_put(bool ok, const char *why) {
    if (put_f) { fclose(put_f); put_f = NULL; }
    xfer_on = false;
    if (ok) {
        remove(put_final);
        if (rename(put_part, put_final) != 0) { ok = false; why = "could not finish the file"; }
    }
    char r[160];
    if (ok) { snprintf(r, sizeof r, "OK"); note("Received %s", put_card); ops++; }
    else { snprintf(r, sizeof r, "ERR\t%s", why); note("Failed: %s", why); }
    job(r);
}

static void handle_line(char *l) {
    if (!strncmp(l, "PUTQ\t", 5)) {
        char *a = l + 5, *t = strchr(a, '\t');
        if (!t) { job("ERR\tbad request"); return; }
        *t = 0;
        long size = atol(t + 1);
        if (!path_ok(a)) { job("ERR\tbad path"); return; }
        char part[300], r[60];
        full_path(a, part, sizeof part);
        strcat(part, ".part");
        struct stat st;
        long have = stat(part, &st) == 0 ? (long)st.st_size : 0;
        if (have > size) { remove(part); have = 0; }
        snprintf(r, sizeof r, "HAVE\t%ld", have);
        job(r);
    } else if (!strncmp(l, "PUT\t", 4)) {
        char *a = l + 4, *t1 = strchr(a, '\t');
        char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t2) { job("ERR\tbad request"); return; }
        *t1 = 0; *t2 = 0;
        long size = atol(t1 + 1), off = atol(t2 + 1);
        if (size < 0 || off < 0 || off > size) { job("ERR\tbad request"); return; }
        long left = size - off;   // the computer sends these bytes right behind the command, so a refusal must throw them away
        if (!path_ok(a)) { job("ERR\tbad path"); if (left > 0) { mode = M_DISCARD; put_left = left; } return; }
        snprintf(put_card, sizeof put_card, "%.199s", a);
        full_path(a, put_final, sizeof put_final);
        snprintf(put_part, sizeof put_part, "%s.part", put_final);
        mkdirs_for(put_final);
        struct stat st;
        if (off > 0 && !(stat(put_part, &st) == 0 && st.st_size == off)) { job("ERR\tthe part on the card is not the size the computer expected, send again"); if (left > 0) { mode = M_DISCARD; put_left = left; } return; }
        put_f = fopen(put_part, off > 0 ? "ab" : "wb");
        if (!put_f) { job("ERR\tcould not open the file on the card"); if (left > 0) { mode = M_DISCARD; put_left = left; } return; }
        put_total = size; put_done = off; put_left = size - off;
        snprintf(xfer_name, sizeof xfer_name, "%.99s", a);
        xfer_total = size; xfer_done = off; xfer_on = true;
        if (put_left == 0) finish_put(true, NULL); else mode = M_PAYLOAD;
    } else {
        job(l);   // the rest is done on the worker
    }
}

void syncd_feed(const char *buf, size_t len) {
    size_t i = 0;
    while (i < len) {
        if (mode == M_LINE) {
            char c = buf[i++];
            if (c == '\n') { line[line_len] = 0; if (line_len) handle_line(line); line_len = 0; }
            else if (line_len + 1 < sizeof line) line[line_len++] = c;
            else line_len = 0;   // a line that long is not ours
        } else {
            size_t n = len - i;
            if ((long)n > put_left) n = (size_t)put_left;
            if (mode == M_PAYLOAD) {
                if (put_f && fwrite(buf + i, 1, n, put_f) != n) { fclose(put_f); put_f = NULL; mode = M_DISCARD; put_left -= (long)n; i += n; job("ERR\tthe card is full or not writable"); xfer_on = false; continue; }
                put_done += (long)n; xfer_done = put_done;
            }
            put_left -= (long)n;
            i += n;
            if (put_left <= 0) {
                if (mode == M_PAYLOAD) { mode = M_LINE; finish_put(true, NULL); }
                else mode = M_LINE;
            }
        }
    }
    if (put_f) fflush(put_f);   // what arrived is safe on the card, so a dropped link can carry on from here
}

// ---- the worker. It runs the commands and sends every reply, on its own stack.
#ifdef SYNCD_HOST
static void job(const char *text) { char b[LINE_MAX_LEN]; snprintf(b, sizeof b, "%s", text); if (!strncmp(b, "OK", 2) || !strncmp(b, "ERR", 3) || !strncmp(b, "HAVE", 4) || !strncmp(b, "PONG", 4)) sendf("%s\n", b); else run_line(b); }
void syncd_begin(const char *name) { if (put_f) { fclose(put_f); put_f = NULL; } mode = M_LINE; line_len = 0; ops = 0; note("Waiting for the computer"); sendf("HELLO\t%s\n", name); }
#else
static QueueHandle_t q;
static void worker(void *arg) {
    for (;;) {
        char *t = NULL;
        if (xQueueReceive(q, &t, portMAX_DELAY) != pdTRUE || !t) continue;
        if (!strncmp(t, "OK", 2) || !strncmp(t, "ERR", 3) || !strncmp(t, "HAVE", 4) || !strncmp(t, "PONG", 4)) { sendf("%s\n", t); }
        else run_line(t);
        free(t);
    }
}
static void job(const char *text) {
    char *c = strdup(text);
    if (c && xQueueSend(q, &c, pdMS_TO_TICKS(2000)) != pdTRUE) free(c);
}
void syncd_begin(const char *name) {
    if (!q) { q = xQueueCreate(8, sizeof(char *)); xTaskCreate(worker, "syncd", 7168, NULL, 4, NULL); }
    if (put_f) { fclose(put_f); put_f = NULL; }
    mode = M_LINE; line_len = 0; ops = 0; xfer_on = false;
    note("Waiting for the computer");
    char h[80];
    int n = snprintf(h, sizeof h, "HELLO\t%.40s\n", name);
    for (int tries = 0; tries < 200 && ssh_send_part((const uint8_t *)h, (size_t)n) <= 0; tries++) vTaskDelay(pdMS_TO_TICKS(20));
}
#endif
