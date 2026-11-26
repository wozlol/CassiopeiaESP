// Connection to the PC helper (a11helper) and the tmux sessions on it. One connection at a time, switching sessions
// ends the connection and attaches to the other one, the sessions themselves keep running on the PC.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SESS_IDLE,
    SESS_CONNECTING,
    SESS_ACTIVE,       // attached and live, or running a short command
    SESS_DONE,         // finished, for an attach it means the session ended or the link dropped
    SESS_ERROR,
} sess_state_t;

typedef enum { SESS_ATTACH, SESS_LIST, SESS_PAIR, SESS_KILL, SESS_CAPTURE, SESS_DROPLIST, SESS_DROPGET } sess_kind_t;

typedef struct {
    char name[41];
    int windows;
    int attached;
} sess_info_t;

void sess_init(void);
// The away address of the computer, or NULL. Asked each time a connection starts. The home address gets 3 seconds, then this one is tried.
void sess_set_away_provider(const char *(*fn)(void));
bool sess_via_away(void);   // the last connection went through the away address
// Starts connecting to the saved computer. SESS_ATTACH: arg is the session name and output goes to the terminal.
// SESS_LIST: lists sessions. SESS_KILL: ends the session named in arg, on the computer. SESS_PAIR: asks the computer to allow this device, arg is not used. Host and port come from
// sess_set_target().
void sess_begin(sess_kind_t kind, const char *arg);
void sess_set_target(const char *host, uint16_t port, const char *pinned_fp);   // pinned_fp may be ""
// Call about every 100 ms while anything is connecting. Returns the current state.
sess_state_t sess_poll(void);
sess_state_t sess_state(void);
sess_kind_t sess_kind(void);
void sess_end(void);                     // detach or abort, the tmux session stays on the PC
// SESS_CAPTURE: arg is the session name, when it is done this holds the last 2000 lines of its history as text. The text stays until the next session starts.
const char *sess_capture_text(size_t *len);
// Files the computer offers in its drop folder. SESS_DROPLIST: the list ends up in sess_capture_text, one line per file, path then a tab then the size.
// SESS_DROPGET: arg is the path, like modes/mode1.bin. It is written to /sdcard/<path>, the SD card must be mounted first.
// Progress while it runs, and whether the whole file arrived once it is done.
void sess_drop_progress(long *got, long *total);
bool sess_drop_ok(void);
const char *sess_error(void);
bool sess_auth_failed(void);   // the computer refused this device's key, it needs pairing again
const char *sess_fingerprint(void);      // 64 hex characters seen from the computer
const char *sess_pair_code(void);        // 4 digits to show while pairing, empty until the computer has been reached
bool sess_pair_ok(void);                 // after SESS_DONE of a pairing, true when the computer said yes
const char *sess_attached_name(void);
int sess_list(sess_info_t *out, int max);   // parse the output of a finished SESS_LIST
bool sess_clean_end(void);
// Keys and pasted text for the attached session. Returns false when nothing is attached.
bool sess_send(const char *data, size_t len);
