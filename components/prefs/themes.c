#include "themes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"

static const display_theme_t THEMES[] = {
    {"Neon Green", RGB565(0x39, 0xFF, 0x14), RGB565(0, 0, 0), RGB565(0x39, 0xFF, 0x14)},
    {"Amber", RGB565(0xFF, 0xB0, 0x00), RGB565(0, 0, 0), RGB565(0xFF, 0xB0, 0x00)},
    {"Soft Green", RGB565(0x00, 0xB1, 0x40), RGB565(0, 0, 0), RGB565(0x00, 0xB1, 0x40)},
    {"Ice Blue", RGB565(0x7F, 0xDB, 0xFF), RGB565(0x00, 0x08, 0x14), RGB565(0x7F, 0xDB, 0xFF)},
    {"White", RGB565(0xE6, 0xE6, 0xE6), RGB565(0, 0, 0), RGB565(0xE6, 0xE6, 0xE6)},
    {"Solarized Dark", RGB565(0x93, 0xA1, 0xA1), RGB565(0x00, 0x2B, 0x36), RGB565(0x93, 0xA1, 0xA1)},
    {"Gruvbox", RGB565(0xEB, 0xDB, 0xB2), RGB565(0x28, 0x28, 0x28), RGB565(0xEB, 0xDB, 0xB2)},
    {"Indiglo Mint", RGB565(0x0B, 0x2A, 0x24), RGB565(0xA4, 0xE9, 0xD0), RGB565(0x0B, 0x2A, 0x24)},
    {"Paper", RGB565(0x1A, 0x1A, 0x1A), RGB565(0xF2, 0xEF, 0xE6), RGB565(0x1A, 0x1A, 0x1A)},
};
#define N_BUILTIN ((int)(sizeof THEMES / sizeof THEMES[0]))

// Saved themes are kept in the settings as "Name|fg|bg|cursor", the colors as four hex digits each.
static display_theme_t custom[THEME_CUSTOM_MAX];
static char custom_name[THEME_CUSTOM_MAX][THEME_NAME_MAX + 1];
static int ncustom;
static bool loaded;

static void load(void) {
    loaded = true;
    ncustom = 0;
    for (int slot = 0; slot < THEME_CUSTOM_MAX; slot++) {
        char key[8], v[48];
        snprintf(key, sizeof key, "cth%d", slot);
        settings_get_str(key, v, sizeof v);
        char *p1 = strchr(v, '|');
        if (!p1) continue;
        *p1++ = 0;
        unsigned fg, bg, cu;
        if (sscanf(p1, "%4x|%4x|%4x", &fg, &bg, &cu) != 3 || !v[0]) continue;
        snprintf(custom_name[ncustom], sizeof custom_name[0], "%.19s", v);
        custom[ncustom] = (display_theme_t){custom_name[ncustom], (color_t)fg, (color_t)bg, (color_t)cu};
        ncustom++;
    }
}

static void store_all(void) {
    for (int slot = 0; slot < THEME_CUSTOM_MAX; slot++) {
        char key[8], v[48] = "";
        snprintf(key, sizeof key, "cth%d", slot);
        if (slot < ncustom) snprintf(v, sizeof v, "%s|%04X|%04X|%04X", custom_name[slot], custom[slot].fg, custom[slot].bg, custom[slot].cursor);
        settings_set_str(key, v);
    }
}

int themes_builtin_count(void) { return N_BUILTIN; }
int themes_count(void) { if (!loaded) load(); return N_BUILTIN + ncustom; }
bool themes_is_custom(int i) { return i >= N_BUILTIN && i < themes_count(); }
const display_theme_t *themes_get(int i) {
    int n = themes_count();
    if (i < 0 || i >= n) i = 0;
    return i < N_BUILTIN ? &THEMES[i] : &custom[i - N_BUILTIN];
}

int themes_custom_save(const char *name, color_t fg, color_t bg, color_t cursor) {
    if (!loaded) load();
    char nm[THEME_NAME_MAX + 1];
    snprintf(nm, sizeof nm, "%.19s", name && name[0] ? name : "My Theme");
    for (char *c = nm; *c; c++) if (*c == '|') *c = '-';
    for (int i = 0; i < N_BUILTIN; i++) if (!strcmp(nm, THEMES[i].name)) { size_t l = strlen(nm); if (l > THEME_NAME_MAX - 2) nm[THEME_NAME_MAX - 2] = 0; strcat(nm, " 2"); break; }
    int slot = -1;
    for (int i = 0; i < ncustom; i++) if (!strcmp(custom_name[i], nm)) slot = i;
    if (slot < 0) { if (ncustom >= THEME_CUSTOM_MAX) return -1; slot = ncustom++; }
    snprintf(custom_name[slot], sizeof custom_name[0], "%s", nm);
    custom[slot] = (display_theme_t){custom_name[slot], fg, bg, cursor};
    store_all();
    return N_BUILTIN + slot;
}

bool themes_custom_delete(int index) {
    if (!themes_is_custom(index)) return false;
    int s = index - N_BUILTIN;
    for (int i = s; i < ncustom - 1; i++) {
        memcpy(custom_name[i], custom_name[i + 1], sizeof custom_name[0]);
        custom[i] = (display_theme_t){custom_name[i], custom[i + 1].fg, custom[i + 1].bg, custom[i + 1].cursor};
    }
    ncustom--;
    store_all();
    return true;
}
