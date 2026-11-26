// Wi-Fi for the handheld. Settings are entered on the device and saved in NVS, nothing is compiled in.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool open;   // no password needed
} net_ap_t;

typedef enum { NET_IDLE, NET_CONNECTING, NET_CONNECTED, NET_FAILED } net_state_t;

// Starts the Wi-Fi stack. If a network is saved it connects to it.
void net_init(void);
void net_unlock(void);   // after the PIN: joins the saved network, its password is encrypted
// Scans and returns up to max networks, strongest first, one entry per name. Takes a few seconds.
int net_scan(net_ap_t *out, int max);
// Connects to a network. With save set, the name and password are kept for the next boot.
void net_connect(const char *ssid, const char *password, bool save);
void net_disconnect(void);   // stops trying and drops the link
void net_forget(void);       // deletes the saved network
net_state_t net_state(void);
const char *net_ip(void);    // "" when not connected
const char *net_ssid(void);  // current or saved network name
bool net_has_saved(void);
const char *net_state_text(void);

// Finding a11helper on the local network.
typedef struct {
    char host[16];     // address like 192.168.1.60
    uint16_t port;     // SSH port the helper listens on
    char name[40];     // the computer's name
} net_found_t;
// Asks the network and collects answers for wait_ms. Returns how many helpers answered.
int net_discover(net_found_t *out, int max, int wait_ms);

// Radio off for sleep, and back on again with the saved network.
void net_sleep(void);
void net_wake(void);
