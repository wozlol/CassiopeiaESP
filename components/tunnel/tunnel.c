#include "tunnel.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "microlink.h"
#include "net.h"
#include "settings.h"

static const char *TAG = "tunnel";
static microlink_t *ml;
static bool enabled, want_start, started;
static char pending_key[100];
static volatile tun_state_t st = TUN_OFF;

static void on_state(microlink_t *m, microlink_state_t s, void *u) {
    switch (s) {
        case ML_STATE_CONNECTED: st = TUN_CONNECTED; break;
        case ML_STATE_NEEDS_LOGIN: st = TUN_NEEDS_LOGIN; break;
        case ML_STATE_AUTH_FAILED: st = TUN_AUTH_FAILED; break;
        case ML_STATE_ERROR: st = TUN_ERROR; break;
        case ML_STATE_WIFI_WAIT: st = TUN_WAIT_WIFI; break;
        default: st = TUN_STARTING; break;
    }
}

static void begin(void) {
    microlink_config_t cfg = {
        .auth_key = pending_key[0] ? pending_key : NULL,
        .device_name = "casio",
        .enable_derp = true, .enable_disco = true, .enable_stun = true,
        .max_peers = 8,
    };
    ml = microlink_init(&cfg);
    memset(pending_key, 0, sizeof pending_key);   // never kept after init
    if (!ml) { st = TUN_ERROR; ESP_LOGE(TAG, "microlink_init failed"); return; }
    microlink_set_state_callback(ml, on_state, NULL);
    st = TUN_STARTING;
    if (microlink_start(ml) != ESP_OK) st = TUN_ERROR;
    started = true;
}

static void task(void *arg) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (enabled && want_start && !started) {
            if (net_state() == NET_CONNECTED) { want_start = false; begin(); }
            else st = TUN_WAIT_WIFI;
        } else if (!enabled && started) {
            microlink_stop(ml);
            started = false;
            st = TUN_OFF;
        }
    }
}

void tunnel_init(void) {
    enabled = settings_get("tson", 0) != 0;
    want_start = enabled;
    st = enabled ? TUN_WAIT_WIFI : TUN_OFF;
    xTaskCreate(task, "tunnel", 6144, NULL, 3, NULL);
}

tun_state_t tunnel_state(void) { return st; }
bool tunnel_enabled(void) { return enabled; }

const char *tunnel_state_text(void) {
    switch (st) {
        case TUN_OFF: return "Off";
        case TUN_WAIT_WIFI: return "Waiting for Wi-Fi";
        case TUN_STARTING: return "Joining the tailnet ...";
        case TUN_CONNECTED: return "Connected";
        case TUN_NEEDS_LOGIN: return "Open the link to approve this device";
        case TUN_AUTH_FAILED: return "The key was refused or expired, make a new one";
        default: return "Error, check the Wi-Fi and try again";
    }
}

void tunnel_set_enabled(bool on) {
    enabled = on;
    settings_set("tson", on);
    if (on) { want_start = !started; if (!started) st = TUN_WAIT_WIFI; }
}

void tunnel_join_with_key(const char *auth_key) {
    snprintf(pending_key, sizeof pending_key, "%s", auth_key);
    if (started) { microlink_stop(ml); microlink_destroy(ml); ml = NULL; started = false; }
    tunnel_set_enabled(true);
    want_start = true;
}

bool tunnel_ip(char *out, size_t cap) {
    if (!ml || st != TUN_CONNECTED) return false;
    uint32_t ip = microlink_get_vpn_ip(ml);
    if (!ip) return false;
    snprintf(out, cap, "%u.%u.%u.%u", (unsigned)((ip >> 24) & 255), (unsigned)((ip >> 16) & 255), (unsigned)((ip >> 8) & 255), (unsigned)(ip & 255));
    return true;
}

bool tunnel_login_url(char *out, size_t cap) {
    if (!ml || st != TUN_NEEDS_LOGIN) return false;
    char url[MICROLINK_AUTH_URL_MAX];
    if (microlink_get_auth_url(ml, url, sizeof url) != ESP_OK || !url[0]) return false;
    snprintf(out, cap, "%s", url);
    return true;
}

bool tunnel_resolve(const char *name, char *out, size_t cap) {
    if (!ml || st != TUN_CONNECTED || !name || !name[0]) return false;
    uint32_t ip = microlink_resolve(ml, name);
    if (!ip) return false;
    snprintf(out, cap, "%u.%u.%u.%u", (unsigned)((ip >> 24) & 255), (unsigned)((ip >> 16) & 255), (unsigned)((ip >> 8) & 255), (unsigned)(ip & 255));
    return true;
}

void tunnel_forget(void) {
    if (started && ml) { microlink_stop(ml); microlink_destroy(ml); ml = NULL; started = false; }
    microlink_factory_reset();
    tunnel_set_enabled(false);
    want_start = false;
    st = TUN_OFF;
}
