// The handheld's own SSH identity (an ed25519 key made on the device the first time) and the computer it pairs with.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Nothing to do before the PIN, the key is sealed with the keystore.
bool id_init(void);
// After the keystore is unlocked: opens the key, or makes a new one if there is none. An old unencrypted key is replaced, so the computer must be paired again.
bool id_unlock(void);
void id_lock(void);
// True once after id_unlock replaced an old unencrypted key.
bool id_key_was_replaced(void);
// The private key as an OpenSSH PEM string, for libssh2. Valid after id_init().
const char *id_private_pem(void);
// The request sent to a11helper to pair: "pair NAME base64 of the public key".
void id_pairing_command(const char *device_name, char *out, size_t cap);
// 4 digit code both screens show, made from the computer's host key fingerprint (64 hex characters) and this device's key.
void id_pair_code(const char *fp_hex, char out[5]);

// The paired computer, one for now.
typedef struct {
    char name[40];      // what the computer calls itself
    char host[64];
    uint16_t port;
    char fp[65];        // pinned SHA256 host key fingerprint, 64 hex characters, empty before pairing
    char code[10];      // short host key code both screens show, like 29F1-EE21
    char away[64];      // a second address for when away from home, like a Tailscale name or 100.x address, empty if none
} id_computer_t;
#define ID_MAX_COMPUTERS 4
// Up to four computers are kept. The load, save and forget calls below work on the active one, slot 0 is where the first pairing lives.
void id_computer_set_slot(int slot);
int id_computer_slot(void);
bool id_computer_slot_load(int slot, id_computer_t *c);
void id_computer_slot_save(int slot, const id_computer_t *c);
void id_computer_slot_forget(int slot);
bool id_computer_load(id_computer_t *c);
void id_computer_save(const id_computer_t *c);
void id_computer_forget(void);
// Short code from a 64 character hex fingerprint.
void id_fp_to_code(const char *fp_hex, char *code, size_t cap);
