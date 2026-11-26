CassiopeiaESP firmware, TEST BUILD.

What this is: the loader and three modes (Mode 1 terminal, reader, media) for the ESP32-S3 board in the Casio A-11 handheld, with the bootloader and the partition table.
Source commit: a363d23. These files were built from that source.
Tested: NOT fully tested on hardware. The terminal sessions, pairing, library, search, theme editor and installing from the SD card have been seen working. The Tailscale link, the zoom viewer, the GUI plus Esc star effect, the loader brightness key and Off, Get files from the computer, Wi-Fi sync mode and the loader suggesting the same-name slot are built but not yet seen working. Treat them as test builds.

How to flash, from this folder, with the USB cable plugged in. Find your port first, on a Mac it looks like /dev/cu.usbmodem1101.

First time, everything at once:
  python -m esptool --chip esp32s3 -p PORT -b 460800 write-flash 0x0 bootloader.bin 0x8000 partition-table.bin 0x20000 loader.bin 0x120000 mode1.bin 0x3a0000 reader.bin 0x620000 media.bin
  python -m esptool --chip esp32s3 -p PORT erase-region 0xf000 0x2000

One mode later, for example the reader:
  python -m esptool --chip esp32s3 -p PORT -b 460800 write-flash 0x3a0000 reader.bin

Slots: loader 0x20000, slot 1 mode1 0x120000, slot 2 reader 0x3a0000, slot 3 media 0x620000, slot 4 0x8a0000 is free. Your Wi-Fi, saved computers and key are kept when you flash a mode.
Full instructions are in the README at the top of this repository.
