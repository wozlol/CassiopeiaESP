// Word wrap for the on screen menus. Plain C with no ESP parts, so tools/test_wrap.sh can check it on the computer.
#pragma once

#define WRAP_FOOT_MAX 6      // most footer lines
#define WRAP_FOOT_LEN 100    // bytes in one footer line

// Bytes of the next line of s that fit in width cells, breaking at a space when there is one. *skip is how many
// separator spaces follow the break and are not drawn. A word longer than the width is cut at the width.
int wrap_line(const char *s, int width, int *skip);

// Cells (UTF-8 code points) in the first nbytes of s.
int wrap_cells(const char *s, int nbytes);

// Where wrapped lines continue. Under the value for a label then two spaces then a value, otherwise under the first letter.
int wrap_hang(const char *s, int cols);

// Packs footer hints, separated by three spaces, into as few lines of at most cols cells as possible.
// Returns the number of lines, each starts with one space.
int wrap_footer(const char *hint, int cols, char lines[][WRAP_FOOT_LEN], int max);
