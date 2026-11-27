// Device UI: GUI key menu, clipboard manager, themes, saved settings. Draws over the terminal and gives the display back.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "a11_keys.h"
#include "touch.h"

typedef struct {
    // Send bytes to the active session (or the local terminal when there is none).
    void (*send)(const char *data, size_t len);
    // The font changed. The app must rebuild the terminal for the new grid and register its buffer with the display.
    void (*terminal_reset)(int cols, int rows);
    // Runs a full screen app (0 reader, 1 media, 2 files) until the person leaves it. Returns 1 for back, 2 when they pressed Off.
    int (*run_app)(int which);
} ui_callbacks_t;

// Call after display_init(). Mounts storage, loads settings and clipboard, applies theme, font and brightness.
// Returns the grid to build the terminal for.
void ui_init(const ui_callbacks_t *cb, int *cols, int *rows);
// Feed every key event. Returns true when the UI used it, false when it should go to the terminal.
// The event may be changed, for example the Caps key arrives as whatever it was set to act as.
bool ui_handle(a11_event_t *ev);
bool ui_active(void);
// Modifier state tracked by the UI.
bool ui_shift(void);
bool ui_ctrl(void);
bool ui_caps(void);
bool ui_alt(void);
// Call about every 100 ms. Updates screens that wait for Wi-Fi or the computer, and reports a session that ended.
void ui_tick(void);
// Call once after the terminal has been started. Shows the PIN screen when a PIN is asked at power on.
void ui_ready(void);

// Feed every touch event. Dragging up and down scrolls back through the conversation.
void ui_touch(touch_event_t ev);
