// The WS2812 LED on the back of the board. A dim glow shows the battery (green, yellow, red). When a terminal response has finished
// (you pressed Enter, output came back, then it went quiet) it fades to blue and stays blue until you type again.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void status_led_start(void);
// Call for every key the user sends to the terminal. Enter starts the wait for a response, any key clears the blue.
void status_led_user_key(bool enter);
// Call whenever output arrives from the session.
void status_led_output(void);
// Dark while the device is off or asleep.
void status_led_off(bool off);
// Battery voltage in millivolts and a rough percent, false if no battery is seen on the sense pin.
bool status_led_battery(int *mv, int *pct);

// Follows the screen brightness (percent). Charging is a guess from the voltage trend, true while the voltage is rising.
void status_led_set_brightness(int percent);
bool status_led_charging(void);

typedef struct {
    int mv, pct;            // battery voltage and rough percent
    bool charging;          // a guess from the voltage trend
    int charge_secs;        // how long it has looked like charging, 0 if not charging
    bool rate_known;        // false until about 3 minutes of charging have been seen
    int rate_pct_x10_per_h; // percent per hour times 10
    int ma;                 // rough charge current from the rate and BOARD_BATTERY_MAH
    int eta_min;            // minutes to full, -1 if unknown
} battery_info_t;

// Fills in the latest numbers. False when no battery is seen on the sense pin.
bool status_led_battery_info(battery_info_t *out);
// While a screen shows the battery live, read it every second instead of every 10.
void status_led_fast_sampling(bool fast);

// A colour shown directly while on is true, the normal glow waits. Turn it off to clear the LED and hand it back.
void status_led_override(bool on, uint8_t r, uint8_t g, uint8_t b);
