#include "a11_id.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "monocypher-ed25519.h"
#include "nvs.h"
#include "keystore.h"

static const char *TAG = "id";
static uint8_t seed[32], secret[64], pub[32];
static char pem[600];
static bool ready;

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64(const uint8_t *in, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = B64[v >> 18 & 63];
        out[o++] = B64[v >> 12 & 63];
        out[o++] = i + 1 < n ? B64[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

static size_t put_u32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; return 4; }
static size_t put_str(uint8_t *p, const void *s, size_t n) { size_t o = put_u32(p, n); memcpy(p + o, s, n); return o + n; }

static size_t pub_blob(uint8_t *out) {
    size_t o = put_str(out, "ssh-ed25519", 11);
    return o + put_str(out + o, pub, 32);
}

static void build_pem(void) {
    uint8_t blob[64], priv[256], all[512];
    size_t bl = pub_blob(blob), p = 0;
    p += put_u32(priv + p, 0x1a2b3c4d);
    p += put_u32(priv + p, 0x1a2b3c4d);
    p += put_str(priv + p, "ssh-ed25519", 11);
    p += put_str(priv + p, pub, 32);
    p += put_str(priv + p, secret, 64);   // seed then public key, the 64 bytes monocypher calls the secret key
    p += put_str(priv + p, "a11", 3);
    for (uint8_t pad = 1; p % 8; pad++) priv[p++] = pad;
    size_t o = 0;
    memcpy(all, "openssh-key-v1", 15); o = 15;       // with the trailing zero
    o += put_str(all + o, "none", 4);
    o += put_str(all + o, "none", 4);
    o += put_str(all + o, "", 0);
    o += put_u32(all + o, 1);
    o += put_str(all + o, blob, bl);
    o += put_str(all + o, priv, p);
    char body[800];
    b64(all, o, body);
    size_t n = snprintf(pem, sizeof pem, "-----BEGIN OPENSSH PRIVATE KEY-----\n");
    for (size_t i = 0; body[i]; i += 70) n += snprintf(pem + n, sizeof pem - n, "%.70s\n", body + i);
    snprintf(pem + n, sizeof pem - n, "-----END OPENSSH PRIVATE KEY-----\n");
}

#define KEY_PLAIN "/store/id_ed25519"
#define KEY_ENC "/store/id_ed25519.enc"
static bool replaced;

bool id_init(void) { return true; }   // nothing to load before the PIN, the key is sealed

bool id_key_was_replaced(void) { bool r = replaced; replaced = false; return r; }

static void shred(const char *path) {
    FILE *f = fopen(path, "r+b");
    if (f) {
        uint8_t z[32] = {0};
        fwrite(z, 1, sizeof z, f);
        fclose(f);
    }
    remove(path);
}

// Opens the key, or makes a new one when there is none. An old unencrypted key is thrown away and replaced, its copy may linger in flash.
bool id_unlock(void) {
    if (ready) return true;
    if (!keystore_unlocked()) return false;
    uint8_t sealed[72];
    bool have = false;
    FILE *f = fopen(KEY_ENC, "rb");
    if (f) {
        have = fread(sealed, 1, sizeof sealed, f) == sizeof sealed;
        fclose(f);
        size_t n = 0;
        have = have && keystore_open(sealed, sizeof sealed, seed, &n) && n == 32;
    }
    if (!have) {
        FILE *old = fopen(KEY_PLAIN, "rb");
        if (old) { fclose(old); shred(KEY_PLAIN); replaced = true; }
        esp_fill_random(seed, sizeof seed);
        keystore_seal(seed, 32, sealed);
        f = fopen(KEY_ENC, "wb");
        if (!f || fwrite(sealed, 1, sizeof sealed, f) != sizeof sealed) { if (f) fclose(f); ESP_LOGE(TAG, "cannot save the device key"); return false; }
        fclose(f);
        ESP_LOGI(TAG, "made a new encrypted device key");
    }
    uint8_t s2[32];
    memcpy(s2, seed, 32);   // key_pair wipes its seed argument
    crypto_ed25519_key_pair(secret, pub, s2);
    build_pem();
    ready = true;
    return true;
}

void id_lock(void) {
    crypto_wipe(seed, sizeof seed);
    crypto_wipe(secret, sizeof secret);
    memset(pem, 0, sizeof pem);
    ready = false;
}

const char *id_private_pem(void) { return pem; }

void id_pairing_command(const char *name, char *out, size_t cap) {
    uint8_t blob[64];
    char enc[100];
    b64(blob, pub_blob(blob), enc);
    snprintf(out, cap, "pair %s %s", name, enc);
}

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : 0; }

void id_pair_code(const char *fp_hex, char out[5]) {
    uint8_t msg[32 + 64], h[64];
    for (int i = 0; i < 32; i++) msg[i] = hexval(fp_hex[2 * i]) << 4 | hexval(fp_hex[2 * i + 1]);
    size_t n = 32 + pub_blob(msg + 32);
    crypto_sha512(h, msg, n);
    uint32_t v = (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];
    snprintf(out, 5, "%04u", (unsigned)(v % 10000));
}

static int active_slot;
void id_computer_set_slot(int slot) { active_slot = slot >= 0 && slot < ID_MAX_COMPUTERS ? slot : 0; }
int id_computer_slot(void) { return active_slot; }

static void ns_name(int slot, char *out) { if (slot == 0) strcpy(out, "pc"); else snprintf(out, 8, "pc%d", slot); }   // slot 0 keeps the original name so an earlier pairing stays

bool id_computer_slot_load(int slot, id_computer_t *c) {
    memset(c, 0, sizeof *c);
    char ns[8];
    ns_name(slot, ns);
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = sizeof c->host;
    bool ok = nvs_get_str(h, "host", c->host, &n) == ESP_OK && c->host[0];
    n = sizeof c->name;
    if (nvs_get_str(h, "name", c->name, &n) != ESP_OK) snprintf(c->name, sizeof c->name, "%.39s", c->host);
    uint16_t port = 2222;
    nvs_get_u16(h, "port", &port);
    c->port = port;
    n = sizeof c->fp;
    if (nvs_get_str(h, "fp", c->fp, &n) != ESP_OK) c->fp[0] = 0;
    n = sizeof c->away;
    if (nvs_get_str(h, "away", c->away, &n) != ESP_OK) c->away[0] = 0;
    nvs_close(h);
    if (c->fp[0]) id_fp_to_code(c->fp, c->code, sizeof c->code);
    return ok;
}

void id_computer_slot_save(int slot, const id_computer_t *c) {
    char ns[8];
    ns_name(slot, ns);
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "host", c->host);
    nvs_set_str(h, "name", c->name);
    nvs_set_u16(h, "port", c->port);
    nvs_set_str(h, "fp", c->fp);
    nvs_set_str(h, "away", c->away);
    nvs_commit(h);
    nvs_close(h);
}

void id_computer_slot_forget(int slot) {
    char ns[8];
    ns_name(slot, ns);
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) { nvs_erase_all(h); nvs_commit(h); nvs_close(h); }
}

bool id_computer_load(id_computer_t *c) { return id_computer_slot_load(active_slot, c); }
void id_computer_save(const id_computer_t *c) { id_computer_slot_save(active_slot, c); }
void id_computer_forget(void) { id_computer_slot_forget(active_slot); }

void id_fp_to_code(const char *fp, char *code, size_t cap) {
    // The first 8 hex digits of the fingerprint, like the helper prints, upper case with a dash.
    snprintf(code, cap, "%.4s-%.4s", fp, fp + 4);
    for (char *p = code; *p; p++) if (*p >= 'a' && *p <= 'f') *p -= 32;
}
