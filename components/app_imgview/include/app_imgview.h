#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "touch.h"
// The shared picture viewer, for the reader, the media viewer and the file manager.
// It opens JPEG and PNG pictures. It starts showing the whole picture. Each Enter zooms in one step: first to fill the screen width or height,
// then twice as much each time until one picture pixel is four screen pixels, and the next Enter starts over with the whole picture.
typedef enum { IV_NONE = 0, IV_NEXT, IV_PREV, IV_CLOSE, IV_HEART } iv_action_t;   // IV_HEART: Space was pressed
bool imgview_open(uint8_t *data, uint32_t len);   // takes the heap buffer, it is freed on close or when this fails
bool imgview_file(const char *path);              // reads a picture file of up to 6 MB and opens it
void imgview_close(void);                         // frees the picture and brings the text screen back
bool imgview_active(void);
const char *imgview_error(void);                  // why the last open failed, in a plain sentence for the person
// What the caller must do after a key or touch. Enter, plus, minus, arrows when zoomed in and taps are handled inside.
// IV_NEXT and IV_PREV come when the whole picture is showing and an arrow is pressed. IV_CLOSE on any other key and on a long press.
iv_action_t imgview_key(uint16_t code);
iv_action_t imgview_touch(touch_event_t te);
// A small picture for a thumbnail view: tw by th pixels, the middle of the picture cropped to fill it, panel ordered RGB565 from the heap.
// Returns NULL when the file cannot be read as a picture. Do not call while a picture is open.
uint16_t *imgview_thumb(const char *path, int tw, int th);
// A white heart in the top right corner of the picture on screen, for a favorite. The caller keeps track of which pictures are favorites.
void imgview_set_heart(bool on);
// Draws the same heart into a 16 bit buffer (panel ordered RGB565), x and y are its top left, scale 2 is small, 3 is the viewer size.
void imgview_draw_heart(uint16_t *buf, int stride, int height, int x, int y, int scale);
