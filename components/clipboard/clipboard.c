#include "clipboard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

#define CLIP_FILE "/store/clip.dat"
static char *entries[CLIP_MAX_ENTRIES];
static int count;

static void save(void) {
    FILE *f = fopen(CLIP_FILE, "wb");
    if (!f) return;
    for (int i = 0; i < count; i++) {
        uint16_t n = (uint16_t)strlen(entries[i]);
        fwrite(&n, 2, 1, f);
        fwrite(entries[i], 1, n, f);
    }
    fclose(f);
}

void clipboard_init(void) {
    FILE *f = fopen(CLIP_FILE, "rb");
    if (!f) return;
    uint16_t n;
    while (count < CLIP_MAX_ENTRIES && fread(&n, 2, 1, f) == 1 && n <= CLIP_MAX_LEN) {
        char *s = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s || fread(s, 1, n, f) != n) { free(s); break; }
        s[n] = 0;
        entries[count++] = s;
    }
    fclose(f);
}

int clipboard_count(void) { return count; }
const char *clipboard_get(int i) { return (i >= 0 && i < count) ? entries[i] : ""; }

void clipboard_remove(int i) {
    if (i < 0 || i >= count) return;
    free(entries[i]);
    memmove(&entries[i], &entries[i + 1], (count - i - 1) * sizeof(char *));
    count--;
    save();
}

bool clipboard_add(const char *text) {
    size_t n = strlen(text);
    if (n == 0 || n > CLIP_MAX_LEN) return false;
    for (int i = 0; i < count; i++)
        if (strcmp(entries[i], text) == 0) clipboard_remove(i--);
    if (count == CLIP_MAX_ENTRIES) clipboard_remove(count - 1);
    char *s = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s) return false;
    memcpy(s, text, n + 1);
    memmove(&entries[1], &entries[0], count * sizeof(char *));
    entries[0] = s;
    count++;
    save();
    return true;
}

int clipboard_find(const char *text) {
    for (int i = 0; i < count; i++) if (strcmp(entries[i], text) == 0) return i;
    return -1;
}

bool clipboard_set(int index, const char *text) {
    size_t n = strlen(text);
    if (index < 0 || index >= count || n == 0 || n > CLIP_MAX_LEN) return false;
    char *s = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s) return false;
    memcpy(s, text, n + 1);
    free(entries[index]);
    entries[index] = s;
    save();
    return true;
}
