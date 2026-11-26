// Color themes. The terminal's default text and background follow the theme, ANSI colors stay as they are.
// Nine themes are built in. Up to eight more can be saved by the theme editor, they live in the settings and every mode sees them.
#pragma once
#include <stdbool.h>
#include "display.h"
#define THEME_NAME_MAX 19
#define THEME_CUSTOM_MAX 8
int themes_count(void);
const display_theme_t *themes_get(int index);
int themes_builtin_count(void);
bool themes_is_custom(int index);
// Saves a theme under a name, replacing a saved one with the same name. Returns its index, or -1 when all eight places are used.
int themes_custom_save(const char *name, color_t fg, color_t bg, color_t cursor);
bool themes_custom_delete(int index);
