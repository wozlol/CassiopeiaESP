#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef struct jf jf_t;
// Opens a baseline JPEG that is already open as a file, reads its headers. Returns NULL for anything else (progressive, arithmetic coding).
jf_t *jf_open(FILE *f, int *w, int *h);
// Decodes the part of the picture inside the rectangle x0, y0, rw, rh (in picture pixels, rw and rh a multiple of den) shrunk by den (1, 2, 4 or 8)
// into out, rw/den by rh/den pixels, panel ordered RGB565.
bool jf_region(jf_t *j, int den, int x0, int y0, int rw, int rh, uint16_t *out);
void jf_close(jf_t *j);
