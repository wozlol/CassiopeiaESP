#!/bin/zsh
set -e
cd "$(dirname "$0")"
arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -Os -ffreestanding -nostdlib -Wall -Wl,-T,link.ld -Wl,--gc-sections -o kbscan.elf kbscan.c
arm-none-eabi-objcopy -O binary kbscan.elf kbscan.bin
arm-none-eabi-size kbscan.elf
python3 -I ../../tools/bin2h.py kbscan.bin ../../nrf-tools/nrf_tool/nrf_image.h 0x00000000
