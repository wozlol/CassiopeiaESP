// Text display for the A-11. Names follow Esperdeck's display.h so vterm and tsm work unchanged.
// A text cell grid is drawn row by row to the SPI panel from a cell buffer owned by the terminal.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef uint16_t color_t;
// Cell colors with these values are drawn in the theme text and background colors.
#define DISPLAY_DEFAULT_FG 0x0002u
#define DISPLAY_DEFAULT_BG 0x0001u
#define RGB565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#define COLOR_BLACK RGB565(0, 0, 0)
#define COLOR_WHITE RGB565(255, 255, 255)

// Same layout as tsm_cell_t and Esperdeck's terminal_cell_t. Colors are RGB565, the sentinel
// values TSM_DEFAULT_FG and TSM_DEFAULT_BG mean "use the theme".
typedef struct {
    uint16_t cp;
    uint16_t fg_color;
    uint16_t bg_color;
    uint8_t attrs;
} terminal_cell_t;
_Static_assert(sizeof(terminal_cell_t) == 8, "terminal_cell_t must be 8 bytes");

#define ATTR_BOLD (1 << 0)
#define ATTR_UNDERLINE (1 << 1)
#define ATTR_REVERSE (1 << 2)
#define ATTR_BLINK (1 << 3)
#define ATTR_DIM (1 << 4)
#define ATTR_ITALIC (1 << 5)
#define ATTR_INVISIBLE (1 << 6)
#define ATTR_STRIKE (1 << 7)

typedef enum { CURSOR_NONE = 0, CURSOR_UNDERSCORE, CURSOR_BLOCK } cursor_mode_t;

typedef struct {
    const char *name;
    color_t fg;      // default text
    color_t bg;      // default background
    color_t cursor;
} display_theme_t;

esp_err_t display_init(void);
void display_set_text_buffer(const terminal_cell_t *buf, int cols, int rows);
void display_set_cursor(int x, int y, cursor_mode_t mode);
void display_bell(void);
// 0 to 100
esp_err_t display_set_backlight(uint8_t percent);

void display_set_theme(const display_theme_t *theme);
const display_theme_t *display_theme(void);
// Font sizes 0 (smallest, 6x12) up to display_font_count() - 1. Returns the new grid in cols and rows.
// The caller must resize the terminal to that grid and register a buffer of that size.
int display_font_count(void);
void display_set_font(int index, int *cols, int *rows);
int display_font_index(void);
void display_get_grid(int *cols, int *rows);
// Pixel size of a cell for the current font.
void display_cell_size(int *w, int *h);
void display_clear(void);

// Highlights text from (r0,c0) to (r1,c1) in reading order, both ends included. Pass active false to clear.
void display_set_selection(bool active, int r0, int c0, int r1, int c1);
void display_get_cursor(int *x, int *y);
// With flat set, the colored backgrounds programs ask for are drawn in the theme background, the text keeps its colors.
void display_set_flat_backgrounds(bool flat);

// Panel off and backlight off, nothing is drawn until it is woken. Waking restores the last backlight level.
void display_sleep(bool sleep);
// Overrides the cursor shape the program asked for, for example to show Caps Lock. Pass -1 to stop overriding.
void display_set_cursor_override(int mode);

// Holds the cursor at a position the terminal cannot move, for the copy mode. Release it with hold false, then ask the
// terminal to put its cursor back.
void display_hold_cursor(bool hold, int x, int y, cursor_mode_t mode);

// Number of screen regions sent so far, for diagnosing.
uint32_t display_frames(void);

// Call after display_sleep(true), right before deep sleep.
void display_power_off(void);

// Pictures and video. display_raw(true) stops the text drawing, display_draw_pixels paints a rectangle from panel ordered RGB565 (bytes swapped),
// display_raw(false) brings the text back. The screen is 480 wide and 320 high.
void display_raw(bool on);
void display_draw_pixels(int x, int y, int w, int h, const uint16_t *pixels);

// Scroll bar on the right edge for lists and text. pos is the first visible line, visible the lines on screen, total all lines.
// Nothing is drawn when everything fits. The app must keep text out of the last column, which the bar covers.
void display_set_scrollbar(int pos, int visible, int total);
// Width in pixels of the strip, for touch hit testing.
int display_scrollbar_width(void);

// The text buffer now on screen, so it can be put back after a temporary screen. False when none is set.
bool display_get_text_buffer(const terminal_cell_t **buf, int *cols, int *rows);
bool display_is_raw(void);   // a picture or video is being painted, the text grid is not drawn
