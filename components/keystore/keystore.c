#include "keystore.h"
#include <string.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "monocypher.h"
#include "nvs.h"

static const char *TAG = "keystore";
#define KS_M 2048   // Argon2id memory in 1 KB blocks
#define KS_T 3      // passes

typedef struct __attribute__((packed)) {
    uint8_t ver, salt[16];
    uint32_t m, t;
    uint8_t nonce[24], mac[16], ct[32];
} blob_t;

static uint8_t dk[32];
static bool unlocked;

typedef struct { uint32_t magic; uint8_t dk[32]; uint8_t chk[8]; } stash_t;
static RTC_NOINIT_ATTR stash_t stash;
#define STASH_MAGIC 0x4B53544DUL

static bool load(blob_t *b) {
    nvs_handle_t h;
    if (nvs_open("a11", NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = sizeof *b;
    bool ok = nvs_get_blob(h, "ks", b, &n) == ESP_OK && n == sizeof *b && b->ver == 1;
    nvs_close(h);
    return ok;
}

static bool save(const blob_t *b) {
    nvs_handle_t h;
    if (nvs_open("a11", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, "ks", b, sizeof *b) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

static bool derive(const char *pin, const uint8_t salt[16], uint32_t m, uint32_t t, uint8_t kek[32]) {
    uint8_t *work = heap_caps_aligned_alloc(8, (size_t)m * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!work) work = heap_caps_aligned_alloc(8, (size_t)m * 1024, MALLOC_CAP_8BIT);
    if (!work) { ESP_LOGE(TAG, "no memory for the key stretching"); return false; }
    int64_t t0 = esp_timer_get_time();
    crypto_argon2_config cfg = {CRYPTO_ARGON2_ID, m, t, 1};
    crypto_argon2_inputs in = {(const uint8_t *)pin, salt, (uint32_t)strlen(pin), 16};
    crypto_argon2(kek, 32, work, cfg, in, crypto_argon2_no_extras);
    free(work);
    ESP_LOGI(TAG, "key stretching took %lld ms", (long long)((esp_timer_get_time() - t0) / 1000));
    return true;
}

static bool wrap_with(const char *pin) {
    blob_t b = {.ver = 1, .m = KS_M, .t = KS_T};
    esp_fill_random(b.salt, sizeof b.salt);
    esp_fill_random(b.nonce, sizeof b.nonce);
    uint8_t kek[32];
    if (!derive(pin, b.salt, b.m, b.t, kek)) return false;
    crypto_aead_lock(b.ct, b.mac, kek, b.nonce, NULL, 0, dk, 32);
    crypto_wipe(kek, sizeof kek);
    return save(&b);
}

bool keystore_exists(void) { blob_t b; bool ok = load(&b); return ok; }
bool keystore_unlocked(void) { return unlocked; }

bool keystore_create(const char *pin) {
    esp_fill_random(dk, sizeof dk);
    if (!wrap_with(pin)) { crypto_wipe(dk, sizeof dk); return false; }
    unlocked = true;
    return true;
}

bool keystore_unlock_pin(const char *pin) {
    blob_t b;
    if (!load(&b)) return false;
    uint8_t kek[32], k[32];
    if (!derive(pin, b.salt, b.m, b.t, kek)) return false;
    int bad = crypto_aead_unlock(k, b.mac, kek, b.nonce, NULL, 0, b.ct, 32);
    crypto_wipe(kek, sizeof kek);
    if (bad) { crypto_wipe(k, sizeof k); return false; }
    memcpy(dk, k, 32);
    crypto_wipe(k, sizeof k);
    unlocked = true;
    return true;
}

bool keystore_change_pin(const char *new_pin) { return unlocked && wrap_with(new_pin); }

size_t keystore_seal(const uint8_t *in, size_t n, uint8_t *out) {
    if (!unlocked) return 0;
    esp_fill_random(out, 24);
    crypto_aead_lock(out + 40, out + 24, dk, out, NULL, 0, in, n);
    return n + 40;
}

bool keystore_open(const uint8_t *in, size_t n, uint8_t *out, size_t *outn) {
    if (!unlocked || n < 40) return false;
    if (crypto_aead_unlock(out, in + 24, dk, in, NULL, 0, in + 40, n - 40)) return false;
    if (outn) *outn = n - 40;
    return true;
}

void keystore_stash(void) {
    if (!unlocked) return;
    stash.magic = STASH_MAGIC;
    memcpy(stash.dk, dk, 32);
    crypto_blake2b(stash.chk, 8, dk, 32);
}

bool keystore_take_stash(void) {
    bool ok = false;
    if (stash.magic == STASH_MAGIC) {
        uint8_t chk[8];
        crypto_blake2b(chk, 8, stash.dk, 32);
        if (memcmp(chk, stash.chk, 8) == 0) { memcpy(dk, stash.dk, 32); unlocked = true; ok = true; }
    }
    keystore_wipe_stash();
    return ok;
}

void keystore_wipe_stash(void) {
    crypto_wipe(&stash, sizeof stash);
}
