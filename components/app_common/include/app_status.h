#pragma once
#include <stddef.h>
// The battery text for the top right of a screen, like "Batt 85%" or "Batt 85%+" while charging. Empty when no battery is seen.
void app_battery_text(char *out, size_t cap);
