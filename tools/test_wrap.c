// Host test for components/ui/wrap.c. Run with tools/test_wrap.sh.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "wrap.h"

static int fails;
static void show(const char *s, int cols, int hang) {
    int pos = 0, first = 1, total = (int)strlen(s);
    while (pos < total) {
        int skip, w = wrap_line(s + pos, first ? cols : cols - hang, &skip);
        if (w == 0) { printf("ZERO WIDTH LINE\n"); fails++; return; }
        printf("|%*s%.*s", first ? 0 : hang, "", w, s + pos);
        int cells = (first ? 0 : hang) + wrap_cells(s + pos, w);
        if (cells > cols) { printf("  <-- TOO WIDE %d", cells); fails++; }
        printf("\n");
        pos += w + skip; first = 0;
    }
}
int main(void) {
    const char *msg = "The device key was renewed and is now encrypted. Pair the computer again: GUI, then Computer.";
    int widths[] = {80, 60, 40, 30, 22, 15};
    for (int i = 0; i < 6; i++) { printf("== message at %d cols\n", widths[i]); show(msg, widths[i] - 2, 0); }
    const char *kv = " Internal RAM  123 KB free and then a long tail that has to wrap";
    printf("== kv hang %d at 40\n", wrap_hang(kv, 40)); show(kv, 40, wrap_hang(kv, 40));
    printf("== kv hang %d at 80\n", wrap_hang(kv, 80)); show(kv, 80, wrap_hang(kv, 80));
    const char *sess = " *  my-very-long-session-name-goes-here 3 windows in use";
    printf("== sess hang %d at 30\n", wrap_hang(sess, 30)); show(sess, 30, wrap_hang(sess, 30));
    printf("== long word\n"); show("abcdefghijklmnopqrstuvwxyz0123456789", 10, 0);
    printf("== utf8\n"); show("caf\xc3\xa9 caf\xc3\xa9 caf\xc3\xa9 caf\xc3\xa9", 9, 0);
    const char *hints[] = {" Enter paste   e edit   n new   c grab line   Backspace delete   Esc close", " Any key goes back", " Up Down move   Left Right change   Enter select   Esc close"};
    int fcols[] = {80, 60, 30, 20};
    for (int h = 0; h < 3; h++) for (int c = 0; c < 4; c++) {
        char lines[WRAP_FOOT_MAX][WRAP_FOOT_LEN];
        int n = wrap_footer(hints[h], fcols[c], lines, WRAP_FOOT_MAX);
        printf("== footer %d at %d cols, %d lines\n", h, fcols[c], n);
        for (int i = 0; i < n; i++) {
            int cells = wrap_cells(lines[i], (int)strlen(lines[i]));
            printf("|%s%s\n", lines[i], cells > fcols[c] ? "  <-- TOO WIDE" : "");
            if (cells > fcols[c]) fails++;
        }
    }
    printf(fails ? "FAILED %d\n" : "all fit\n", fails);
    return fails != 0;
}
