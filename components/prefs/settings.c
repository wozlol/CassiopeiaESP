#include "settings.h"
#include "nvs.h"
#include "nvs_flash.h"

static nvs_handle_t h;
static bool ok;

void settings_init(void) {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); e = nvs_flash_init(); }
    ok = e == ESP_OK && nvs_open("a11", NVS_READWRITE, &h) == ESP_OK;
}

int32_t settings_get(const char *key, int32_t def) {
    int32_t v;
    return ok && nvs_get_i32(h, key, &v) == ESP_OK ? v : def;
}

void settings_set(const char *key, int32_t value) {
    if (!ok) return;
    nvs_set_i32(h, key, value);
    nvs_commit(h);
}

void settings_get_str(const char *key, char *out, size_t cap) {
    out[0] = 0;
    if (!ok) return;
    size_t n = cap;
    if (nvs_get_str(h, key, out, &n) != ESP_OK) out[0] = 0;
}

void settings_set_str(const char *key, const char *value) {
    if (!ok) return;
    nvs_set_str(h, key, value);
    nvs_commit(h);
}
