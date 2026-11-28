#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "touch.h"
// The shared picture viewer. Open a JPEG, it shows fitted to the screen. Keys and touch zoom and move it.
typedef enum { IV_NONE = 0, IV_NEXT, IV_PREV, IV_CLOSE } iv_action_t;
bool imgview_open(uint8_t *jpg, uint32_t len);   // takes the heap buffer, it is freed on close or when this fails
bool imgview_file(const char *path);             // reads a JPEG file of up to 6 MB and opens it
void imgview_close(void);                        // frees the picture and brings the text screen back
bool imgview_active(void);
// What the caller must do after a key or touch. Plus, minus, Enter, arrows when zoomed and taps are handled inside.
// IV_NEXT and IV_PREV come when the picture is fitted and an arrow is pressed. IV_CLOSE on any other key and on a long press.
iv_action_t imgview_key(uint16_t code);
iv_action_t imgview_touch(touch_event_t te);
