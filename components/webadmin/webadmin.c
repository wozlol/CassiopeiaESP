#include "brand.h"
#include "mdns.h"
#include "webadmin.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "clipboard.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "net.h"
#include "sdcard.h"
#include "settings.h"
#include "tunnel.h"

static const char *TAG = "web";
extern const char page_start[] asm("_binary_page_html_start");
extern const char page_end[] asm("_binary_page_html_end");

static bool enabled = true, lan = false;
static char code[8], token[33];
static int listen_fd = -1;
static uint32_t listen_addr;   // network order, 0 means every address
static volatile int conns;
static int fails;
static int64_t lock_until;
static const char *state_text = "Off";

// ---- settings
static void make_code(void) {
    snprintf(code, sizeof code, "%06u", (unsigned)(esp_random() % 1000000u));
    settings_set_str("wcode", code);
}
bool webadmin_enabled(void) { return enabled; }
void webadmin_set_enabled(bool on) { enabled = on; settings_set("web", on); }
bool webadmin_lan(void) { return lan; }
void webadmin_set_lan(bool on) { lan = on; settings_set("weblan", on); }
static bool ask;   // the page asks for the 6 digit code, off unless the person turns it on
bool webadmin_ask_code(void) { return ask; }
void webadmin_set_ask_code(bool on) { ask = on; settings_set("wask", on); }
const char *webadmin_code(void) { return code; }
void webadmin_new_code(void) { make_code(); fails = 0; lock_until = 0; }
const char *webadmin_state_text(void) { return state_text; }
int webadmin_addresses(char out[4][48]) {
    int n = 0;
    char ip[24];
    if (tunnel_ip(ip, sizeof ip)) { snprintf(out[n++], 48, "http://" BRAND_HOST); snprintf(out[n++], 48, "http://%s", ip); }
    if (lan && net_state() == NET_CONNECTED) { snprintf(out[n++], 48, "http://" BRAND_HOST ".local"); snprintf(out[n++], 48, "http://%s", net_ip()); }
    return n;
}

// ---- small http helpers
typedef struct { char method[8], path[300], query[400], cookie[100], host[80]; long clen; bool a11; } req_t;

static bool send_all(int fd, const void *d, size_t n) {
    const char *p = d;
    while (n) {
        int k = send(fd, p, n, 0);
        if (k <= 0) return false;
        p += k; n -= (size_t)k;
    }
    return true;
}
static void head(int fd, int status, const char *ctype, long clen, const char *extra) {
    char h[400];
    const char *st = status == 200 ? "OK" : status == 400 ? "Bad Request" : status == 401 ? "Unauthorized" : status == 403 ? "Forbidden" : status == 404 ? "Not Found" : status == 429 ? "Too Many Requests" : "Error";
    int n = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n", status, st, ctype);
    if (clen >= 0) n += snprintf(h + n, sizeof h - (size_t)n, "Content-Length: %ld\r\n", clen);
    if (extra) n += snprintf(h + n, sizeof h - (size_t)n, "%s", extra);
    n += snprintf(h + n, sizeof h - (size_t)n, "\r\n");
    send_all(fd, h, (size_t)n);
}
static void say(int fd, int status, const char *text) {
    head(fd, status, "text/plain; charset=utf-8", (long)strlen(text), NULL);
    send_all(fd, text, strlen(text));
}
static void url_decode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) { char h[3] = {s[1], s[2], 0}; *o++ = (char)strtol(h, NULL, 16); s += 2; }
        else if (*s == '+') *o++ = ' ';
        else *o++ = *s;
    }
    *o = 0;
}
static bool qget(const req_t *r, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    for (const char *p = r->query; *p;) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t n = strcspn(v, "&");
            if (n >= cap) n = cap - 1;
            memcpy(out, v, n);
            out[n] = 0;
            url_decode(out);
            return true;
        }
        p += strcspn(p, "&");
        if (*p == '&') p++;
    }
    return false;
}
static void json_str(int fd, const char *s) {
    char b[260];
    size_t o = 0;
    b[o++] = '"';
    for (; *s; s++) {
        if (o + 8 > sizeof b) { send_all(fd, b, o); o = 0; }
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { b[o++] = '\\'; b[o++] = (char)c; }
        else if (c == '\n') { b[o++] = '\\'; b[o++] = 'n'; }
        else if (c == '\r' || c == '\t') { b[o++] = ' '; }
        else if (c < 32) { b[o++] = ' '; }
        else b[o++] = (char)c;
    }
    b[o++] = '"';
    send_all(fd, b, o);
}

// ---- paths, only modes, library and media on the card
static bool chars_ok(const char *p) {
    for (; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '.' || c == '_' || c == '+' || c == '(' || c == ')' || c == '\'' || c == ' ' || c == ',' || c == '-' || c == '/')) return false;
    }
    return true;
}
static bool is_root(const char *p) { return !strcmp(p, "modes") || !strcmp(p, "library") || !strcmp(p, "media") || !strcmp(p, "downloads"); }
static bool under_root(const char *p) { return !strncmp(p, "modes/", 6) || !strncmp(p, "library/", 8) || !strncmp(p, "media/", 6) || !strncmp(p, "downloads/", 10); }
static bool path_ok(const char *p) {   // a file or folder below a root
    size_t n = strlen(p);
    return n >= 7 && n <= 180 && under_root(p) && !strstr(p, "..") && p[n - 1] != '/' && chars_ok(p) && !strstr(p, "//");
}
static bool dir_ok(const char *p) { return is_root(p) || path_ok(p); }
static void full(const char *card, char *out, size_t cap) { snprintf(out, cap, "%s/%s", SDCARD_MOUNT, card); }
static void mkdirs_for(const char *fullp) {
    char p[400];
    snprintf(p, sizeof p, "%s", fullp);
    for (char *q = p + strlen(SDCARD_MOUNT) + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(p, 0777); *q = '/'; }
}
static void rm_tree(const char *path, int depth) {
    DIR *d = opendir(path);
    if (!d) { remove(path); return; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[600];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode) && depth < 6) rm_tree(sub, depth + 1);
        else remove(sub);
    }
    closedir(d);
    remove(path);
}

// ---- the API
static void api_info(int fd) {
    uint64_t total = 0, freeb = 0;
    esp_vfs_fat_info(SDCARD_MOUNT, &total, &freeb);
    const esp_partition_t *run = esp_ota_get_running_partition();
    char b[200];
    const esp_app_desc_t *ad = esp_app_get_description();
    int n = snprintf(b, sizeof b, "{\"total\":%llu,\"free\":%llu,\"running\":\"%s\",\"version\":\"%s\"}", (unsigned long long)total, (unsigned long long)freeb, run ? run->label : "", ad ? ad->version : "");
    head(fd, 200, "application/json", n, NULL);
    send_all(fd, b, (size_t)n);
}
static void api_ls(int fd, const req_t *r) {
    char dir[200] = "";
    qget(r, "dir", dir, sizeof dir);
    head(fd, 200, "application/json", -1, NULL);
    send_all(fd, "{\"items\":[", 10);
    bool first = true;
    if (!dir[0]) {
        static const char *roots[] = {"downloads", "modes", "library", "media"};
        for (int i = 0; i < 4; i++) { char b[60]; snprintf(b, sizeof b, "%s{\"n\":\"%s\",\"d\":1}", first ? "" : ",", roots[i]); send_all(fd, b, strlen(b)); first = false; }
    } else if (dir_ok(dir)) {
        char fp[300];
        full(dir, fp, sizeof fp);
        DIR *d = opendir(fp);
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char child[700];
            snprintf(child, sizeof child, "%s/%s", fp, e->d_name);
            struct stat st;
            if (stat(child, &st) != 0) continue;
            size_t nl = strlen(e->d_name);
            if (nl > 5 && !strcmp(e->d_name + nl - 5, ".part")) continue;
            if (!first) send_all(fd, ",", 1);
            first = false;
            send_all(fd, "{\"n\":", 5);
            json_str(fd, e->d_name);
            char t[60];
            if (S_ISDIR(st.st_mode)) snprintf(t, sizeof t, ",\"d\":1}"); else snprintf(t, sizeof t, ",\"s\":%ld}", (long)st.st_size);
            send_all(fd, t, strlen(t));
        }
        if (d) closedir(d);
    }
    send_all(fd, "]}", 2);
}
static void api_get_file(int fd, const req_t *r) {
    char p[200], fp[300];
    if (!qget(r, "path", p, sizeof p) || !path_ok(p)) { say(fd, 400, "bad path"); return; }
    full(p, fp, sizeof fp);
    struct stat st;
    FILE *f = fopen(fp, "rb");
    if (!f || stat(fp, &st) != 0 || !S_ISREG(st.st_mode)) { if (f) fclose(f); say(fd, 404, "no such file"); return; }
    const char *name = strrchr(p, '/');
    char extra[300];
    snprintf(extra, sizeof extra, "Content-Disposition: attachment; filename=\"%s\"\r\n", name ? name + 1 : p);
    head(fd, 200, "application/octet-stream", (long)st.st_size, extra);
    char *buf = malloc(4096);
    size_t n;
    while (buf && (n = fread(buf, 1, 4096, f)) > 0) if (!send_all(fd, buf, n)) break;
    free(buf);
    fclose(f);
}
static void api_put_file(int fd, const req_t *r, const char *lead, size_t lead_n) {
    char p[200], fp[300], part[320];
    if (!qget(r, "path", p, sizeof p) || !path_ok(p)) { say(fd, 400, "bad path, use a folder under modes, library or media"); return; }
    full(p, fp, sizeof fp);
    snprintf(part, sizeof part, "%s.part", fp);
    mkdirs_for(fp);
    FILE *f = fopen(part, "wb");
    if (!f) { say(fd, 500, "could not open the file on the card, is it full or write protected"); return; }
    long left = r->clen;
    bool ok = true;
    if (lead_n) { size_t n = lead_n > (size_t)left ? (size_t)left : lead_n; ok = fwrite(lead, 1, n, f) == n; left -= (long)n; }
    char *buf = malloc(4096);
    struct timeval tv = {20, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (ok && left > 0 && buf) {
        int k = recv(fd, buf, left > 4096 ? 4096 : (int)left, 0);
        if (k <= 0) { ok = false; break; }
        ok = fwrite(buf, 1, (size_t)k, f) == (size_t)k;
        left -= k;
    }
    free(buf);
    fclose(f);
    if (!ok || left != 0) { remove(part); say(fd, 500, "the upload did not finish and nothing was kept"); return; }
    remove(fp);
    if (rename(part, fp) != 0) { say(fd, 500, "could not finish the file"); return; }
    ESP_LOGI(TAG, "received %s", p);
    say(fd, 200, "ok");
}
static void api_delete(int fd, const req_t *r, bool tree) {
    char p[200], fp[300];
    if (!qget(r, "path", p, sizeof p) || !path_ok(p)) { say(fd, 400, "bad path"); return; }
    full(p, fp, sizeof fp);
    struct stat st;
    if (stat(fp, &st) != 0) { say(fd, 404, "no such file or folder"); return; }
    if (tree) rm_tree(fp, 0); else remove(fp);
    say(fd, stat(fp, &st) != 0 ? 200 : 500, stat(fp, &st) != 0 ? "ok" : "could not remove it");
}
static void api_move(int fd, const req_t *r) {
    char a[200], b[200], fa[300], fb[300];
    if (!qget(r, "from", a, sizeof a) || !qget(r, "to", b, sizeof b) || !path_ok(a) || !path_ok(b)) { say(fd, 400, "bad path"); return; }
    full(a, fa, sizeof fa);
    full(b, fb, sizeof fb);
    struct stat st;
    if (stat(fa, &st) != 0) { say(fd, 404, "no such file or folder"); return; }
    if (stat(fb, &st) == 0) { say(fd, 400, "something with that name is already there"); return; }
    mkdirs_for(fb);
    bool moved = rename(fa, fb) == 0;
    say(fd, moved ? 200 : 500, moved ? "ok" : "could not move it");
}
static void api_mkdir(int fd, const req_t *r) {
    char p[200], fp[300];
    if (!qget(r, "path", p, sizeof p) || !path_ok(p)) { say(fd, 400, "bad folder name"); return; }
    full(p, fp, sizeof fp);
    mkdirs_for(fp);
    mkdir(fp, 0777);
    struct stat st;
    say(fd, stat(fp, &st) == 0 ? 200 : 500, stat(fp, &st) == 0 ? "ok" : "could not make the folder");
}
static void api_install(int fd, const req_t *r) {
    char p[200], fp[300], sl[8];
    if (!qget(r, "path", p, sizeof p) || !qget(r, "slot", sl, sizeof sl) || !path_ok(p) || strncmp(p, "modes/", 6)) { say(fd, 400, "bad file or slot"); return; }
    int slot = atoi(sl);
    if (slot < 1 || slot > 4) { say(fd, 400, "slot 1 to 4"); return; }
    full(p, fp, sizeof fp);
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *dst = esp_partition_find_first(ESP_PARTITION_TYPE_APP, (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_0 + slot - 1), NULL);
    if (!dst) { say(fd, 400, "no such slot"); return; }
    if (dst == run) { say(fd, 400, "That slot is the one running now. Open the loader on the handheld and install it from there."); return; }
    FILE *f = fopen(fp, "rb");
    struct stat st;
    if (!f || stat(fp, &st) != 0) { if (f) fclose(f); say(fd, 404, "no such file"); return; }
    if ((size_t)st.st_size > dst->size) { fclose(f); say(fd, 400, "the file is bigger than the slot"); return; }
    esp_ota_handle_t h;
    if (esp_ota_begin(dst, (size_t)st.st_size, &h) != ESP_OK) { fclose(f); say(fd, 500, "could not start the install"); return; }
    uint8_t *buf = malloc(4096);
    size_t n;
    bool ok = buf != NULL;
    while (ok && (n = fread(buf, 1, 4096, f)) > 0) ok = esp_ota_write(h, buf, n) == ESP_OK;
    free(buf);
    fclose(f);
    if (!ok || esp_ota_end(h) != ESP_OK) { say(fd, 500, "the file is not a valid image, nothing was kept"); return; }
    esp_app_desc_t d;
    char msg[160];
    if (esp_ota_get_partition_description(dst, &d) == ESP_OK) snprintf(msg, sizeof msg, "%s %s is installed in slot %d", d.project_name, d.version, slot);
    else snprintf(msg, sizeof msg, "installed in slot %d", slot);
    say(fd, 200, msg);
}
static void api_clips(int fd, const req_t *r, const char *lead, size_t lead_n) {
    // /api/clips and /api/clips/N
    int n = 0;
    const char *sub = r->path + strlen("/api/clips");
    if (*sub == '/') n = atoi(sub + 1);
    if (!strcmp(r->method, "GET") && n == 0) {
        head(fd, 200, "application/json", -1, NULL);
        send_all(fd, "{\"clips\":[", 10);
        for (int i = 0; i < clipboard_count(); i++) { if (i) send_all(fd, ",", 1); json_str(fd, clipboard_get(i)); }
        send_all(fd, "]}", 2);
        return;
    }
    char body[1100];
    size_t bl = 0;
    if (!strcmp(r->method, "POST") || !strcmp(r->method, "PUT")) {
        if (r->clen < 1 || r->clen > 1000) { say(fd, 400, "a clip is 1 to 1000 characters"); return; }
        size_t take = lead_n > (size_t)r->clen ? (size_t)r->clen : lead_n;
        memcpy(body, lead, take);
        bl = take;
        while (bl < (size_t)r->clen) {
            int k = recv(fd, body + bl, (size_t)r->clen - bl, 0);
            if (k <= 0) { say(fd, 400, "the text did not arrive"); return; }
            bl += (size_t)k;
        }
        body[bl] = 0;
    }
    if (!strcmp(r->method, "POST") && n == 0) { bool ok = clipboard_add(body); say(fd, ok ? 200 : 400, ok ? "ok" : "could not add it"); }
    else if (!strcmp(r->method, "PUT") && n >= 1 && n <= clipboard_count()) { bool ok = clipboard_set(n - 1, body); say(fd, ok ? 200 : 400, ok ? "ok" : "could not change it"); }
    else if (!strcmp(r->method, "DELETE") && n >= 1 && n <= clipboard_count()) { clipboard_remove(n - 1); say(fd, 200, "ok"); }
    else say(fd, 404, "no such clip");
}

// ---- requests
static bool ct_equal(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    unsigned d = (unsigned)(la ^ lb);
    for (size_t i = 0; i < la && i < lb; i++) d |= (unsigned)(a[i] ^ b[i]);
    return d == 0;
}
static bool host_ok(const char *h) {   // an address or the brand name, so a web page somewhere else cannot point its own name at this
    char n[80];
    snprintf(n, sizeof n, "%s", h);
    char *c = strchr(n, ':');
    if (c) *c = 0;
    if (!strncasecmp(n, BRAND_HOST, strlen(BRAND_HOST))) return true;
    for (char *p = n; *p; p++) if (!isdigit((unsigned char)*p) && *p != '.') return false;
    return n[0] != 0;
}
static bool parse_req(char *buf, req_t *r) {
    memset(r, 0, sizeof *r);
    char *eol = strstr(buf, "\r\n");
    if (!eol) return false;
    *eol = 0;
    char *sp1 = strchr(buf, ' '), *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (!sp1 || !sp2) return false;
    *sp1 = 0; *sp2 = 0;
    snprintf(r->method, sizeof r->method, "%s", buf);
    char *q = strchr(sp1 + 1, '?');
    if (q) { *q = 0; snprintf(r->query, sizeof r->query, "%s", q + 1); }
    snprintf(r->path, sizeof r->path, "%s", sp1 + 1);
    for (char *l = eol + 2; *l;) {
        char *e = strstr(l, "\r\n");
        if (!e) break;
        *e = 0;
        if (!strncasecmp(l, "Content-Length:", 15)) r->clen = atol(l + 15);
        else if (!strncasecmp(l, "Cookie:", 7)) snprintf(r->cookie, sizeof r->cookie, "%s", l + 7);
        else if (!strncasecmp(l, "Host:", 5)) { const char *v = l + 5; while (*v == ' ') v++; snprintf(r->host, sizeof r->host, "%s", v); }
        else if (!strncasecmp(l, "X-A11:", 6)) r->a11 = true;
        l = e + 2;
    }
    return true;
}
static void handle(int fd) {
    char *buf = malloc(3200);
    if (!buf) return;
    struct timeval tv = {10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int len = 0;
    char *hend = NULL;
    buf[0] = 0;
    for (;;) {   // read until the end of the headers, then keep whatever of the body came with them
        hend = strstr(buf, "\r\n\r\n");
        if (hend || len >= 3100) break;
        int k = recv(fd, buf + len, 3100 - len, 0);
        if (k <= 0) { free(buf); return; }
        len += k;
        buf[len] = 0;
    }
    if (!hend) { say(fd, 400, "request too large"); free(buf); return; }
    size_t lead_n = (size_t)(len - (hend + 4 - buf));
    char lead[3200];
    memcpy(lead, hend + 4, lead_n);
    req_t r;
    if (!parse_req(buf, &r) || !host_ok(r.host)) { say(fd, 400, "bad request"); free(buf); return; }
    free(buf);
    url_decode(r.path);
    if (!strcmp(r.method, "GET") && (!strcmp(r.path, "/") || !strcmp(r.path, "/index.html"))) {
        head(fd, 200, "text/html; charset=utf-8", (long)(page_end - page_start - 1), "Content-Security-Policy: default-src 'self' 'unsafe-inline'\r\n");
        send_all(fd, page_start, (size_t)(page_end - page_start - 1));
        return;
    }
    if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/login")) {
        if (!r.a11) { say(fd, 403, "forbidden"); return; }
        if (!ask) { head(fd, 200, "text/plain", 2, ""); send_all(fd, "ok", 2); return; }
        if (esp_timer_get_time() < lock_until) { say(fd, 429, "Too many wrong codes. Wait a minute and try again."); return; }
        char c[16] = "";
        size_t take = lead_n < sizeof c - 1 ? lead_n : sizeof c - 1;
        memcpy(c, lead, take);
        c[take] = 0;
        if (!ct_equal(c, code)) {
            if (++fails >= 5) { lock_until = esp_timer_get_time() + 60 * 1000000LL; fails = 0; }
            say(fd, 401, "That code is not right.");
            return;
        }
        fails = 0;
        char extra[160];
        snprintf(extra, sizeof extra, "Set-Cookie: a11s=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=28800\r\n", token);
        head(fd, 200, "text/plain", 2, extra);
        send_all(fd, "ok", 2);
        return;
    }
    // everything else needs the cookie, and a custom header on anything that is not a plain download
    char want[48];
    snprintf(want, sizeof want, "a11s=%s", token);
    if (ask && !strstr(r.cookie, want)) { say(fd, 401, "login"); return; }   // with the code switched off, being able to reach the page is enough
    bool plain_get = !strcmp(r.method, "GET") && !strcmp(r.path, "/api/file");
    if (!plain_get && !r.a11) { say(fd, 403, "forbidden"); return; }
    if (!sdcard_ok()) sdcard_mount();
    if (!strcmp(r.path, "/api/info") && !strcmp(r.method, "GET")) api_info(fd);
    else if (!strcmp(r.path, "/api/ls") && !strcmp(r.method, "GET")) api_ls(fd, &r);
    else if (!strcmp(r.path, "/api/file") && !strcmp(r.method, "GET")) api_get_file(fd, &r);
    else if (!strcmp(r.path, "/api/file") && !strcmp(r.method, "PUT")) api_put_file(fd, &r, lead, lead_n);
    else if (!strcmp(r.path, "/api/file") && !strcmp(r.method, "DELETE")) api_delete(fd, &r, false);
    else if (!strcmp(r.path, "/api/dir") && !strcmp(r.method, "DELETE")) api_delete(fd, &r, true);
    else if (!strcmp(r.path, "/api/mkdir") && !strcmp(r.method, "POST")) api_mkdir(fd, &r);
    else if (!strcmp(r.path, "/api/move") && !strcmp(r.method, "POST")) api_move(fd, &r);
    else if (!strcmp(r.path, "/api/install") && !strcmp(r.method, "POST")) api_install(fd, &r);
    else if (!strncmp(r.path, "/api/clips", 10)) api_clips(fd, &r, lead, lead_n);
    else say(fd, 404, "not found");
}
static void conn_task(void *arg) {
    int fd = (int)(intptr_t)arg;
    handle(fd);
    shutdown(fd, SHUT_RDWR);
    close(fd);
    conns--;
    vTaskDelete(NULL);
}

// ---- the listener, kept in step with Wi-Fi, Tailscale and the settings
static void close_listener(void) {
    if (listen_fd >= 0) { close(listen_fd); listen_fd = -1; }
}
static bool mdns_up;   // announcing the brand name .local on the home Wi-Fi, only while the home Wi-Fi switch is on
static void supervisor(void *arg) {
    for (;;) {
        bool want_name = enabled && lan && net_state() == NET_CONNECTED;
        if (want_name && !mdns_up) {
            if (mdns_init() == ESP_OK) {
                mdns_hostname_set(BRAND_HOST);
                mdns_instance_name_set(BRAND_NAME " web admin");
                mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
                mdns_up = true;
            } else ESP_LOGW(TAG, BRAND_HOST ".local could not start");
        } else if (!want_name && mdns_up) { mdns_free(); mdns_up = false; }
        char ip[24] = "";
        bool ts = tunnel_ip(ip, sizeof ip);
        bool wifi = net_state() == NET_CONNECTED;
        uint32_t want_addr = 0;
        bool want = false;
        if (!enabled) state_text = "Off";
        else if (lan && wifi) { want = true; want_addr = 0; }
        else if (ts) { want = true; want_addr = inet_addr(ip); }
        else state_text = lan ? "Waiting for Wi-Fi" : "Waiting for Tailscale";
        if (listen_fd >= 0 && (!want || want_addr != listen_addr)) close_listener();
        if (want && listen_fd < 0) {
            int s = socket(AF_INET, SOCK_STREAM, 0);
            int one = 1;
            setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(80), .sin_addr.s_addr = want_addr};
            if (s >= 0 && bind(s, (struct sockaddr *)&a, sizeof a) == 0 && listen(s, 4) == 0) {
                listen_fd = s; listen_addr = want_addr;
                ESP_LOGI(TAG, "web admin listening on %s", want_addr ? ip : "every address");
            } else { if (s >= 0) close(s); ESP_LOGW(TAG, "web admin could not listen, errno %d", errno); }
        }
        if (listen_fd >= 0) {
            state_text = "Listening";
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(listen_fd, &rf);
            struct timeval tv = {1, 0};
            if (select(listen_fd + 1, &rf, NULL, NULL, &tv) > 0) {
                int c = accept(listen_fd, NULL, NULL);
                if (c >= 0) {
                    if (conns >= 4) { say(c, 429, "busy"); close(c); }
                    else { conns++; if (xTaskCreate(conn_task, "webconn", 9216, (void *)(intptr_t)c, 4, NULL) != pdPASS) { conns--; close(c); } }
                }
            }
        } else vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
void webadmin_start(void) {
    enabled = settings_get("web", 1) != 0;
    lan = settings_get("weblan", 0) != 0;
    ask = settings_get("wask", 0) != 0;
    settings_get_str("wcode", code, sizeof code);
    if (strlen(code) != 6) make_code();
    for (int i = 0; i < 16; i++) snprintf(token + i * 2, 3, "%02x", (unsigned)(esp_random() & 0xff));
    xTaskCreate(supervisor, "webadmin", 4096, NULL, 3, NULL);
}
