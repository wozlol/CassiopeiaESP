#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "touch.h"
// The shared picture viewer, for the reader, the media viewer and the file manager.
// It opens JPEG and PNG pictures. It starts showing the whole picture. Each Enter zooms in one step: first to fill the screen width or height,
// then twice as much each time until one picture pixel is four screen pixels, and the next Enter starts over with the whole picture.
typedef enum { IV_NONE = 0, IV_NEXT, IV_PREV, IV_CLOSE } iv_action_t;
bool imgview_open(uint8_t *data, uint32_t len);   // takes the heap buffer, it is freed on close or when this fails
bool imgview_file(const char *path);              // reads a picture file of up to 6 MB and opens it
void imgview_close(void);                         // frees the picture and brings the text screen back
bool imgview_active(void);
const char *imgview_error(void);                  // why the last open failed, in a plain sentence for the person
// What the caller must do after a key or touch. Enter, plus, minus, arrows when zoomed in and taps are handled inside.
// IV_NEXT and IV_PREV come when the whole picture is showing and an arrow is pressed. IV_CLOSE on any other key and on a long press.
iv_action_t imgview_key(uint16_t code);
iv_action_t imgview_touch(touch_event_t te);
