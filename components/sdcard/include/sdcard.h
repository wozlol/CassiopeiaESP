// The SD card on the board, 4 bit SD_MMC, FAT32 and exFAT, mounted at /sdcard.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SDCARD_MOUNT "/sdcard"
bool sdcard_mount(void);   // false if there is no card or it is not FAT32
bool sdcard_ok(void);
bool sdcard_present(void);     // true while a mounted card still answers, false once it has been taken out
void sdcard_unmount(void);     // finishes any writing and lets go of the card, so it can be taken out safely
// Looks for a card in the background, so the screen and keys never wait for it. State: 0 idle, 1 looking, 2 found and mounted, 3 nothing usable.
void sdcard_mount_async(void);
int sdcard_async_state(void);
void sdcard_async_ack(void);   // after reading state 2 or 3
int sdcard_problem(void);         // why the last mount failed: 0 nothing, 1 no card, 2 a card that answers but whose file system cannot be read
uint32_t sdcard_serial(void);  // tells one card from another
