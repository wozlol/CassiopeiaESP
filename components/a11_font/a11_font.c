#include "a11_font.h"
#include <stddef.h>

static const uint8_t *lookup(const a11_font_t *f, uint16_t cp) {
    int lo = 0, hi = f->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->cps[mid] == cp) return f->bits + (size_t)mid * f->h * f->row_bytes;
        if (f->cps[mid] < cp) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

const uint8_t *a11_font_glyph(const a11_font_t *f, uint16_t cp) {
    const uint8_t *g = lookup(f, cp);
    return g ? g : lookup(f, '?');
}

const a11_font_t *a11_font_get(int size_index, bool bold) {
    if (size_index < 0 || size_index >= A11_FONT_SIZES) size_index = 0;
    return &A11_FONTS[size_index * 2 + (bold ? 1 : 0)];
}
