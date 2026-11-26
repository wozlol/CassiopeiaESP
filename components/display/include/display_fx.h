// Esperdeck had visual effects here. This display has none, the calls vterm makes do nothing.
#pragma once
static inline void display_fx_static_brief(void) {}
static inline void display_fx_touch_row(int row) { (void)row; }
