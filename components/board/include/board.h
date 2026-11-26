// Board profile: Freenove FNK0104S, ESP32-S3, 4.0 inch 320x480 ST7796 touch, in the Casio A-11 shell.
// A different screen or board gets its own profile with the same names, the apps only include this file.
#pragma once

#define BOARD_NAME "FNK0104S"

// Display, SPI panel ST7796 driven in landscape
#define BOARD_LCD_HOST SPI2_HOST
#define BOARD_LCD_MOSI 11
#define BOARD_LCD_SCLK 12
#define BOARD_LCD_CS 10
#define BOARD_LCD_DC 46
#define BOARD_LCD_BL 45
#define BOARD_LCD_RST (-1)
#define BOARD_LCD_WIDTH 480       // after rotation
#define BOARD_LCD_HEIGHT 320
#define BOARD_LCD_PCLK_HZ (40 * 1000 * 1000)
#define BOARD_LCD_INVERT 1
#define BOARD_LCD_SWAP_XY 1
#define BOARD_LCD_MIRROR_X 0
#define BOARD_LCD_MIRROR_Y 0
#define BOARD_LCD_BGR 1

// Touch FT6336U and audio codec ES8311 share this I2C bus
#define BOARD_I2C_SDA 16
#define BOARD_I2C_SCL 15
#define BOARD_TOUCH_RST 18
#define BOARD_TOUCH_INT 17

// How touch coordinates map onto the landscape screen. Raw touch is portrait, x 0..319 and y 0..479.
// Landscape X is the raw y, landscape Y is 319 minus the raw x (same as the Freenove examples).
#define BOARD_TOUCH_SWAP_XY 1
#define BOARD_TOUCH_FLIP_X 0
#define BOARD_TOUCH_FLIP_Y 1

// SD card, SD_MMC 4 bit
#define BOARD_SD_CLK 38
#define BOARD_SD_CMD 40
#define BOARD_SD_D0 39
#define BOARD_SD_D1 41
#define BOARD_SD_D2 48
#define BOARD_SD_D3 47

// Link to the nRF52832 keyboard controller
#define BOARD_NRF_SWCLK 43
#define BOARD_NRF_SWDIO 44
#define BOARD_NRF_IRQ 3

// WS2812 status LED on the back of the board, and the battery sense input (divided by 2 on the board).
// Both pins come from community projects for the FNK0104, not from Freenove's own docs. Check the battery reading against a meter.
#define BOARD_RGB_LED_GPIO 42
#define BOARD_BAT_ADC_CHANNEL ADC_CHANNEL_8   // GPIO9
#define BOARD_BAT_DIVIDER 2
#define BOARD_BATTERY_MAH 3000   // the battery in the case, used for charging speed estimates
