// Sync mode, the handheld's side. While the handheld holds a connection open with the helper command "sync", the computer can send
// files to the SD card, take files from it, delete files, manage the clipboard and install firmware into a slot. Only paths under
// modes, library and media on the card are reachable. The handheld starts the connection, nothing listens here.
#pragma once
#include <stdbool.h>
#include <stddef.h>

void syncd_begin(const char *device_name);   // a sync connection has just opened, says hello to the computer
void syncd_end(void);                          // the connection is over, closes a file that was arriving
void syncd_feed(const char *buf, size_t len);   // bytes from the computer, called by the connection reader
const char *syncd_last(void);                 // one line for the screen about the last thing done
int syncd_ops(void);                          // how many commands have been done
bool syncd_transfer(char *name, size_t cap, long *done, long *total);   // true while a file is arriving or leaving
