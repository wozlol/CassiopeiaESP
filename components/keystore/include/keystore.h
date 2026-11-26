// Encrypted storage for the SSH key and the Wi-Fi password.
// A random data key is wrapped under a key made from the PIN (Argon2id), the wrapped copy lives in flash. The PIN opens it, the data key then seals the secrets.
// A short PIN can be guessed by someone who copies the flash, the lock only protects well with a longer PIN.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool keystore_exists(void);                  // a PIN has been set up for the store
bool keystore_unlocked(void);                // the data key is in RAM
bool keystore_create(const char *pin);       // first time, makes the data key, unlocked afterwards
bool keystore_unlock_pin(const char *pin);   // takes about a second, false for a wrong PIN
bool keystore_change_pin(const char *new_pin);   // needs the store unlocked
// Sealed form is a 24 byte nonce, a 16 byte tag, then the data. Output needs n + 40 bytes.
size_t keystore_seal(const uint8_t *in, size_t n, uint8_t *out);
bool keystore_open(const uint8_t *in, size_t n, uint8_t *out, size_t *outn);   // false if locked or the data was changed
// Deep sleep keeps the data key in RAM that survives it, so the secret press can unlock without the PIN. Anything else that wakes the device wipes it.
void keystore_stash(void);
bool keystore_take_stash(void);
void keystore_wipe_stash(void);
