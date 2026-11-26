// The SD card on the board, 4 bit SD_MMC, FAT32, mounted at /sdcard.
#pragma once
#include <stdbool.h>

#define SDCARD_MOUNT "/sdcard"
bool sdcard_mount(void);   // false if there is no card or it is not FAT32
bool sdcard_ok(void);
