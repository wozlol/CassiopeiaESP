// A-11 keyboard reader. Polls the nRF scanner mailbox over SWD and turns it into key down and up events.
// Interim link for stage 6, the nRF will later send these events itself over I2C with IO3 as interrupt.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "a11_keymap.h"

typedef struct {
    uint16_t code;   // USB HID usage (page 0x07) or A11_KEY_ON, A11_KEY_OFF, A11_KEY_BACKLIGHT
    bool down;
} a11_event_t;

// Starts the reader task. Events go to the queue, connection problems are retried forever.
void a11_keys_start(QueueHandle_t events);
const char *a11_key_name(uint16_t code);
// US layout character for a HID usage, or 0 if it has none. Shift and Caps Lock are applied for letters.
char a11_hid_to_char(uint16_t code, bool shift, bool caps);
#define A11_HID_LEFT_CTRL 224
#define A11_HID_LEFT_SHIFT 225
#define A11_HID_LEFT_ALT 226
#define A11_HID_LEFT_GUI 227
#define A11_HID_CAPS_LOCK 57
#define A11_HID_ESCAPE 41

// Poll slowly while the device sleeps, a key press still wakes it.
void a11_keys_set_slow(bool slow);

// Counters for diagnosing: events sent and scans read.
uint32_t a11_keys_event_count(void);
uint32_t a11_keys_poll_count(void);

// Deep sleep. Arms the nRF to raise IO3 when On is pressed, checks it took, powers the debug link down and keeps the SWD lock held.
// False means it is not safe to deep sleep (old nRF firmware or no answer), nothing was changed.
bool a11_keys_prepare_sleep(void);
// Last step, wakes only from IO3. Does not return.
void a11_keys_deep_sleep(void);
// After a wake from deep sleep: the keys that were held when On was pressed (modifiers count), or -1 if unknown. Cleared on each start.
int a11_keys_wake_held(uint16_t *codes, int max);

// Diagnostics for the I2C link.
uint32_t a11_keys_io3_edges(void);
uint32_t a11_keys_read_fail(void);
uint32_t a11_keys_read_ok(void);
int a11_keys_io3_level(void);

// How much of the time the nRF has slept since the ESP started reading it (tenths of a percent), its total wakes and the seconds measured. False until measured or when keys are not read over I2C.
bool a11_keys_nrf_sleep(int *asleep_tenths_pct, int *wakes, int *seconds);

// True while that key is held down, read from the key state the reader task keeps. Safe to call from any task.
bool a11_keys_down(uint16_t code);
