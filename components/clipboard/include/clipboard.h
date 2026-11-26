// Clipboard manager storage. Entries live in RAM and are saved to /store/clip.dat on the internal storage partition.
#pragma once
#include <stdbool.h>
#define CLIP_MAX_ENTRIES 40
#define CLIP_MAX_LEN 1000
void clipboard_init(void);
int clipboard_count(void);
const char *clipboard_get(int index);          // 0 is the newest
bool clipboard_add(const char *text);          // adds at the front, moves an identical entry to the front
void clipboard_remove(int index);
bool clipboard_set(int index, const char *text);   // replace the text of one entry
int clipboard_find(const char *text);                // index of an identical entry, or -1
