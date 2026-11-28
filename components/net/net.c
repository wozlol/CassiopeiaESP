#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "esp_timer.h"
#include "nvs.h"
#include "keystore.h"
#include "nvs_flash.h"

static const char *TAG = "net";
static volatile net_state_t state = NET_IDLE;
static char ip[16], ssid_now[33], reason[40];
static bool wanted, started;
static int retries;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ip[0] = 0;
        wifi_event_sta_disconnected_t *d = data;
        snprintf(reason, sizeof reason, "disconnected, code %d", d->reason);
        if (wanted && retries++ < 6) { state = NET_CONNECTING; esp_wifi_connect(); }
        else { state = wanted ? NET_FAILED : NET_IDLE; }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(ip, sizeof ip, IPSTR, IP2STR(&e->ip_info.ip));
        retries = 0;
        state = NET_CONNECTED;
        ESP_LOGI(TAG, "connected, address %s", ip);
    }
}

static bool load_ssid(char *s, size_t ns) {
    nvs_handle_t h;
    if (nvs_open("net", NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_str(h, "ssid", s, &ns) == ESP_OK && s[0];
    nvs_close(h);
    return ok;
}

// The password is stored sealed. False when it is sealed and the keystore is locked. A password saved before encryption is sealed on first use.
static bool load_pass(char *p, size_t np) {
    nvs_handle_t h;
    p[0] = 0;
    if (nvs_open("net", NVS_READWRITE, &h) != ESP_OK) return false;
    uint8_t sealed[24 + 16 + 64 + 1];
    size_t n = sizeof sealed;
    bool ok = false;
    if (nvs_get_blob(h, "passe", sealed, &n) == ESP_OK) {
        uint8_t plain[65];
        size_t pn = 0;
        if (keystore_open(sealed, n, plain, &pn) && pn < np && pn < sizeof plain) { memcpy(p, plain, pn); p[pn] = 0; ok = true; }
        memset(plain, 0, sizeof plain);
    } else {
        size_t pn = np;
        ok = true;
        if (nvs_get_str(h, "pass", p, &pn) != ESP_OK) p[0] = 0;
        else if (keystore_unlocked()) {   // an old plain password, seal it now
            size_t sn = keystore_seal((const uint8_t *)p, strlen(p), sealed);
            if (sn && nvs_set_blob(h, "passe", sealed, sn) == ESP_OK) { nvs_erase_key(h, "pass"); nvs_commit(h); }
        }
    }
    nvs_close(h);
    return ok;
}

static bool load_saved(char *s, size_t ns, char *p, size_t np) { return load_ssid(s, ns) && load_pass(p, np); }

static void start_stack(void) {
    if (started) return;
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);   // the saved network lives in our own NVS key, not the driver's
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    started = true;
}

// Starts the radio stack. The saved network is joined by net_unlock after the PIN, its password is sealed.
void net_init(void) { start_stack(); }

void net_unlock(void) {
    char s[33], p[65];
    if (load_saved(s, sizeof s, p, sizeof p)) net_connect(s, p, false);
    memset(p, 0, sizeof p);
}

int net_scan(net_ap_t *out, int max) {
    start_stack();
    wifi_scan_config_t sc = {.show_hidden = false};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return 0;
    uint16_t n = 24;
    wifi_ap_record_t recs[24];
    esp_wifi_scan_get_ap_records(&n, recs);
    int count = 0;
    for (int i = 0; i < n && count < max; i++) {
        if (!recs[i].ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < count; j++) if (strcmp(out[j].ssid, (char *)recs[i].ssid) == 0) dup = true;
        if (dup) continue;
        snprintf(out[count].ssid, sizeof out[count].ssid, "%s", (char *)recs[i].ssid);
        out[count].rssi = recs[i].rssi;
        out[count].open = recs[i].authmode == WIFI_AUTH_OPEN;
        count++;
    }
    return count;   // records come back strongest first
}

void net_connect(const char *s, const char *pass, bool save) {
    start_stack();
    wanted = true;
    retries = 0;
    state = NET_CONNECTING;
    reason[0] = 0;
    snprintf(ssid_now, sizeof ssid_now, "%s", s);
    wifi_config_t wc = {0};
    snprintf((char *)wc.sta.ssid, sizeof wc.sta.ssid, "%s", s);
    snprintf((char *)wc.sta.password, sizeof wc.sta.password, "%s", pass);
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();
    if (save) {
        nvs_handle_t h;
        if (nvs_open("net", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "ssid", s);
            uint8_t sealed[24 + 16 + 64 + 1];
            size_t sn = keystore_seal((const uint8_t *)pass, strlen(pass), sealed);
            if (sn) { nvs_set_blob(h, "passe", sealed, sn); nvs_erase_key(h, "pass"); }   // no sealed copy possible while locked, keep nothing rather than plain text
            nvs_commit(h);
            nvs_close(h);
        }
    }
}

void net_disconnect(void) {
    wanted = false;
    state = NET_IDLE;
    ip[0] = 0;
    if (started) esp_wifi_disconnect();
}

void net_forget(void) {
    net_disconnect();
    nvs_handle_t h;
    if (nvs_open("net", NVS_READWRITE, &h) == ESP_OK) { nvs_erase_all(h); nvs_commit(h); nvs_close(h); }
    ssid_now[0] = 0;
}

net_state_t net_state(void) { return state; }
const char *net_ip(void) { return ip; }
bool net_has_saved(void) { char s[33]; return load_ssid(s, sizeof s); }

const char *net_ssid(void) {
    if (!ssid_now[0]) load_ssid(ssid_now, sizeof ssid_now);
    return ssid_now;
}

const char *net_state_text(void) {
    switch (state) {
        case NET_IDLE: return "off";
        case NET_CONNECTING: return "connecting";
        case NET_CONNECTED: return "connected";
        default: return reason[0] ? reason : "failed";
    }
}

#define DISCOVERY_PORT 2223

int net_discover(net_found_t *out, int max, int wait_ms) {
    if (state != NET_CONNECTED) return 0;
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s < 0) return 0;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &yes, sizeof yes);
    struct timeval tv = {0, 150000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(DISCOVERY_PORT)};
    int count = 0;
    int64_t end = esp_timer_get_time() + (int64_t)wait_ms * 1000, next_probe = 0;
    esp_netif_ip_info_t info;
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    bool have_info = nif && esp_netif_get_ip_info(nif, &info) == ESP_OK;
    while (esp_timer_get_time() < end && count < max) {
        if (esp_timer_get_time() >= next_probe) {   // ask twice, a probe can be lost
            to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
            sendto(s, "A11?", 4, 0, (struct sockaddr *)&to, sizeof to);
            if (have_info) {
                to.sin_addr.s_addr = info.ip.addr | ~info.netmask.addr;
                sendto(s, "A11?", 4, 0, (struct sockaddr *)&to, sizeof to);
            }
            next_probe = esp_timer_get_time() + 600000;
        }
        char buf[100];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(s, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        buf[n] = 0;
        int ver = 0, port = 0;
        char name[40] = "";
        if (sscanf(buf, "A11HELPER %d %d %39s", &ver, &port, name) < 3 || port <= 0) continue;
        char host[16];
        inet_ntoa_r(from.sin_addr, host, sizeof host);
        bool dup = false;
        for (int i = 0; i < count; i++) if (strcmp(out[i].host, host) == 0) dup = true;
        if (dup) continue;
        snprintf(out[count].host, sizeof out[count].host, "%s", host);
        out[count].port = (uint16_t)port;
        snprintf(out[count].name, sizeof out[count].name, "%s", name);
        count++;
    }
    close(s);
    return count;
}

void net_sleep(void) {
    wanted = false;
    state = NET_IDLE;
    ip[0] = 0;
    if (started) { esp_wifi_disconnect(); esp_wifi_stop(); }
}

void net_wake(void) {
    if (!started) { net_init(); return; }
    esp_wifi_start();
    char s[33], p[65];
    if (load_saved(s, sizeof s, p, sizeof p)) net_connect(s, p, false);
}

int net_signal_percent(void) {
    wifi_ap_record_t ap;
    if (net_state() != NET_CONNECTED || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return -1;
    int p = 2 * (ap.rssi + 100);   // about -50 dBm and better is full, -100 dBm and worse is empty
    return p < 0 ? 0 : p > 100 ? 100 : p;
}

void net_boost(bool on) {
    static int users;
    static SemaphoreHandle_t lock;
    if (!lock) lock = xSemaphoreCreateMutex();
    xSemaphoreTake(lock, portMAX_DELAY);
    if (on) { if (users++ == 0) esp_wifi_set_ps(WIFI_PS_NONE); }
    else if (users > 0 && --users == 0) esp_wifi_set_ps(WIFI_PS_MIN_MODEM);   // the default power saving again
    xSemaphoreGive(lock);
}

