// The web page on the handheld. Open the brand name address (see brand.h) from any of your Tailscale devices to manage the SD card files and the clipboard.
// It listens on the Tailscale address only, unless you also allow the home Wi-Fi. A 6 digit code shown on the handheld is asked for only if that switch is on.
#pragma once
#include <stdbool.h>
#include <stddef.h>

void webadmin_start(void);                 // starts the small supervisor task, call once after Wi-Fi and Tailscale are set up
bool webadmin_enabled(void);
void webadmin_set_enabled(bool on);
bool webadmin_lan(void);                   // also listening on the home Wi-Fi
void webadmin_set_lan(bool on);
bool webadmin_ask_code(void);              // the page asks for the code, default off
void webadmin_set_ask_code(bool on);
const char *webadmin_code(void);           // the 6 digit code the page asks for
void webadmin_new_code(void);
// Where it can be reached now, one line each: the brand name, "http://100.x.y.z", the brand name with .local, "http://192.168.x.y". Returns the number of lines.
int webadmin_addresses(char out[4][48]);
const char *webadmin_state_text(void);     // for the screen: Off, Waiting for Tailscale, or Listening
