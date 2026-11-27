// The Tailscale link. It joins this device to your tailnet through MicroLink, so computers can be reached by their 100.x addresses
// while away from home. Off until you turn it on. Only a person turns it on, nothing starts it by itself.
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    TUN_OFF,          // switched off
    TUN_WAIT_WIFI,    // on, waiting for Wi-Fi
    TUN_STARTING,     // joining or reconnecting
    TUN_CONNECTED,
    TUN_NEEDS_LOGIN,  // no key given, open the link to approve this device
    TUN_AUTH_FAILED,  // the key was refused or has expired, a new one is needed
    TUN_ERROR,
} tun_state_t;

void tunnel_init(void);                         // reads the saved on or off setting, starts the helper task
tun_state_t tunnel_state(void);
const char *tunnel_state_text(void);
bool tunnel_enabled(void);
void tunnel_set_enabled(bool on);               // on: joins with what is saved, or asks for a login link. off: stops.
void tunnel_join_with_key(const char *auth_key);   // a one-time key from the Tailscale admin page, tagged for this device. It is not kept.
bool tunnel_ip(char *out, size_t cap);          // this device's 100.x address while connected
bool tunnel_login_url(char *out, size_t cap);   // the link to open while TUN_NEEDS_LOGIN
bool tunnel_resolve(const char *name, char *out, size_t cap);   // a Tailscale name like wozlap3 to its 100.x address, false if unknown or not connected
void tunnel_forget(void);                       // stops and wipes this device's Tailscale keys. It must be removed in the admin page too.
