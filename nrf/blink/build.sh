#!/bin/zsh
set -e
cd "$(dirname "$0")"
arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -Os -ffreestanding -nostdlib -Wall -Wl,-T,link.ld -Wl,--gc-sections -o blink.elf blink.c
arm-none-eabi-objcopy -O binary blink.elf blink.bin
arm-none-eabi-size blink.elf
python3 -I ../../tools/bin2h.py blink.bin ../../nrf-tools/nrf_tool/nrf_image.h 0x00000000
