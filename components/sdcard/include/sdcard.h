// The SD card on the board, 4 bit SD_MMC, FAT32, mounted at /sdcard.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SDCARD_MOUNT "/sdcard"
bool sdcard_mount(void);   // false if there is no card or it is not FAT32
bool sdcard_ok(void);
bool sdcard_present(void);     // true while a mounted card still answers, false once it has been taken out
void sdcard_unmount(void);     // finishes any writing and lets go of the card, so it can be taken out safely
uint32_t sdcard_serial(void);  // tells one card from another
