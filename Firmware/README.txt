CassiopeiaESP firmware, TEST BUILD.

What this is: the loader and two programs for the ESP32-S3 board in the Casio A-11 handheld, with the bootloader and the partition table. Mode 1 is the terminal with the web admin page, the Library is the reader, media viewer and file manager.
Version 0.5.6, source commit bc0294d (tag v0.5.6). mode1.bin and library.bin were built from that source. loader.bin, bootloader.bin and partition-table.bin are unchanged from the earlier build a363d23.
Tested: NOT fully tested on hardware. Seen working: Mode 1 starts, the web page answers over the home Wi-Fi by its number and takes uploads to the SD card with no code. Built but not yet seen working on hardware: the Library program, the picture zoom in Media and Files, the status line, the version number display, the Files screens, wozlol.local, installing a program from the web page, the Tailscale name, the Ask for a code switch, the restart between Mode 1 and the Library. Treat them as test builds.

How to flash, from this folder, with the USB cable plugged in. Find your port first, on a Mac it looks like /dev/cu.usbmodem1101.

First time, everything at once:
  python -m esptool --chip esp32s3 -p PORT -b 460800 write-flash 0x0 bootloader.bin 0x8000 partition-table.bin 0x20000 loader.bin 0x120000 mode1.bin 0x3a0000 library.bin
  python -m esptool --chip esp32s3 -p PORT erase-region 0xf000 0x2000

One program later, for example the Library:
  python -m esptool --chip esp32s3 -p PORT -b 460800 write-flash 0x3a0000 library.bin

Slots: loader 0x20000, slot 1 Mode 1 0x120000, slot 2 Library 0x3a0000, slots 3 and 4 (0x620000 and 0x8a0000) are free. Your Wi-Fi, saved computers and key are kept when you flash a program.
Full instructions are in the README at the top of this repository.
