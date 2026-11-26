#include "wrap.h"
#include <stdbool.h>
#include <string.h>

static int cp_bytes(const char *s) {
    unsigned char c = (unsigned char)s[0];
    int n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 1;
    for (int i = 1; i < n; i++) if (!s[i]) return i;
    return n;
}

int wrap_cells(const char *s, int nbytes) {
    int cells = 0;
    for (int i = 0; i < nbytes && s[i]; i += cp_bytes(s + i)) cells++;
    return cells;
}

int wrap_line(const char *s, int width, int *skip) {
    int cells = 0, i = 0, last = -1;
    bool seen = false;
    *skip = 0;
    if (width < 1) width = 1;
    while (s[i]) {
        if (cells == width) {
            if (last >= 0) { i = last; while (s[i + *skip] == ' ') (*skip)++; return i; }
            return i;   // one long word, cut here
        }
        if (s[i] == ' ') {
            if (seen && (i == 0 || s[i - 1] != ' ')) last = i;
        } else seen = true;
        i += cp_bytes(s + i);
        cells++;
    }
    return i;
}

int wrap_hang(const char *s, int cols) {
    int n0 = 0;
    while (s[n0] == ' ') n0++;
    if (n0 > cols / 3) n0 = cols / 3;
    int hang = n0;
    for (int i = n0; s[i]; i++) {
        if (s[i] == ' ' && s[i + 1] == ' ') {
            int j = i;
            while (s[j] == ' ') j++;
            if (s[j]) hang = wrap_cells(s, j);
            break;
        }
    }
    if (hang > cols / 2) hang = n0;
    return hang;
}

int wrap_footer(const char *hint, int cols, char lines[][WRAP_FOOT_LEN], int max) {
    int n = 0, col = 0;
    const char *p = hint;
    while (*p == ' ') p++;
    if (cols > WRAP_FOOT_LEN - 1) cols = WRAP_FOOT_LEN - 1;
    lines[0][0] = 0;
    while (*p && n < max) {
        const char *e = p;
        while (*e && !(e[0] == ' ' && e[1] == ' ' && e[2] == ' ')) e++;
        int len = (int)(e - p);
        int cells = wrap_cells(p, len);
        if (col > 0 && col + 3 + cells <= cols) {
            strcat(lines[n], "   ");
            strncat(lines[n], p, (size_t)len);
            col += 3 + cells;
        } else {
            if (col > 0) { if (++n >= max) break; }
            // a hint longer than one line is word wrapped, later lines are indented two cells
            const char *q = p;
            int left = len, first = 1;
            while (left > 0 && n < max) {
                int skip, w = wrap_line(q, cols - (first ? 1 : 3), &skip);
                if (w > left) w = left;
                strcpy(lines[n], first ? " " : "   ");
                strncat(lines[n], q, (size_t)w);
                col = wrap_cells(lines[n], (int)strlen(lines[n]));
                q += w + skip;
                left -= w + skip;
                first = 0;
                if (left > 0 && ++n >= max) { n = max - 1; break; }
                if (left > 0) lines[n][0] = 0;
            }
        }
        p = e;
        while (*p == ' ') p++;
    }
    return n + 1 > max ? max : n + 1;
}
