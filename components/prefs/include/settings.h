// Small persistent settings in NVS.
#pragma once
#include <stddef.h>
#include <stdint.h>
void settings_init(void);
int32_t settings_get(const char *key, int32_t def);
void settings_set(const char *key, int32_t value);
void settings_get_str(const char *key, char *out, size_t cap);   // empty string when not set
void settings_set_str(const char *key, const char *value);
