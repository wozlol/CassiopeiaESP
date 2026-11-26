/*
 * color.c — ANSI-256 → RGB565 conversion
 *
 * SPDX-License-Identifier: MIT
 */
#include "color.h"

/* ── Named colors (indices 0–15) ─────────────────────────────────────────── */

/* Standard xterm/VT220 16-color palette as RGB565.
 * Matches the values used by display_ansi_to_rgb565() in display.c. */
#define C565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
/* CassiopeiaESP: VS Code dark terminal colors, the xterm values are too dim for thin glyphs on black. */
static const uint16_t s_named[16] = {
    /* 0  black   */ C565(0, 0, 0),
    /* 1  red     */ C565(205, 49, 49),
    /* 2  green   */ C565(13, 188, 121),
    /* 3  yellow  */ C565(229, 229, 16),
    /* 4  blue    */ C565(36, 114, 200),
    /* 5  magenta */ C565(188, 63, 188),
    /* 6  cyan    */ C565(17, 168, 205),
    /* 7  white   */ C565(229, 229, 229),
    /* 8  br.blk  */ C565(102, 102, 102),
    /* 9  br.red  */ C565(241, 76, 76),
    /* 10 br.grn  */ C565(35, 209, 139),
    /* 11 br.yel  */ C565(245, 245, 67),
    /* 12 br.blu  */ C565(59, 142, 234),
    /* 13 br.mag  */ C565(214, 112, 214),
    /* 14 br.cyn  */ C565(41, 184, 219),
    /* 15 br.wht  */ C565(255, 255, 255),
};

/* ── 6×6×6 colour cube helper ────────────────────────────────────────────── */

static inline uint8_t cube_to_8(int v)
{
    /* xterm cube: 0→0, 1→95, 2→135, 3→175, 4→215, 5→255 */
    return v ? (uint8_t)(55 + v * 40) : 0u;
}

uint16_t color_ansi(uint8_t idx)
{
    if (idx < 16) {
        return s_named[idx];
    }
    if (idx < 232) {
        /* 6×6×6 cube: index 16–231 */
        int i = idx - 16;
        int b = i % 6;  i /= 6;
        int g = i % 6;  i /= 6;
        int r = i % 6;
        return color_rgb(cube_to_8(r), cube_to_8(g), cube_to_8(b));
    }
    /* Grayscale: indices 232–255 → 8, 18, 28 … 238 */
    uint8_t v = (uint8_t)(8 + (idx - 232) * 10);
    return color_rgb(v, v, v);
}
