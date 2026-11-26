# Wiring and soldering

Sources: Freenove 4.0inch_ESP32-S3_Display_Schematic.pdf (External Interface page), SparkFun nRF52832 breakout schematic (github.com/sparkfun/nRF52832_Breakout, Hardware folder).

The SparkFun WRL-13990 pads are labeled on the silkscreen by nRF pin number, so pad "31" is P0.31. Trust the silkscreen number over the position numbers below. On both long headers (J3 and J4) position 1 is the GND end.

## Before soldering

1. Cut the PWR-LED jumper (JP6). It wastes 1 to 2 mA in sleep.
2. Leave VIN unconnected. Power comes in on the 3.3V pad only.
3. Leave RESET unconnected until the freepads setting is written, then it carries keyboard pin 26.
4. Do not use pads 0, 1, 6 and 7. They are crystal, button and LED pads. Pads 9, 10 and 21 carry keyboard pins 24 to 26 and are used only after the one time setting.

## ESP connectors

Pin 1 of each 4 pin connector is per the Freenove schematic. Confirm against the board silkscreen and with a meter before connecting anything. Power the ESP alone and measure 3.3 V between I2C pin 1 and pin 2.

| Connector | Pin | Signal |
|---|---|---|
| UART | 1 | 5V |
| UART | 2 | GND |
| UART | 3 | TXD0 (GPIO43) |
| UART | 4 | RXD0 (GPIO44) |
| Extended IO | 1 | IO2 |
| Extended IO | 2 | IO3 |
| Extended IO | 3 | IO14 |
| Extended IO | 4 | IO21 |
| I2C | 1 | 3.3V |
| I2C | 2 | GND |
| I2C | 3 | SCL (GPIO15) |
| I2C | 4 | SDA (GPIO16) |

Extended IO has no GND pin. Take grounds from the UART and I2C connectors.

## ESP to nRF

| ESP connector pin | nRF pad | Purpose |
|---|---|---|
| I2C 1 (3.3V) | 3.3V (J3 position 3) | power |
| I2C 2 (GND) | GND (J3 position 1) | ground |
| UART 3 (TXD0) | C test point | SWDCLK, programming |
| UART 4 (RXD0) | D test point | SWDIO, programming |
| I2C 3 (SCL) | J1 pad labeled RXI (P0.27) | runtime link clock |
| I2C 4 (SDA) | J1 pad labeled TXO (P0.26) | runtime link data |
| Extended IO 2 (IO3) | pad 25 (J3 position 9) | wake and interrupt |

J1 is the 6 pin FTDI header. Use only its RXI and TXO pads and leave the rest empty. Check with a meter that the RXI pad reaches pad 27 and the TXO pad reaches pad 26 before powering.

## ESP to PCM5102A

Powered from 3.3 V, not the UART connector 5 V. The chip wants 3.0 to 3.6 V on all its supplies (TI datasheet SLAS859C). Many modules have an onboard regulator behind VIN, check on yours whether VIN or a separate 3V3 pin is the right one to feed with 3.3 V.

| ESP connector pin | GPIO | PCM5102A pin |
|---|---|---|
| I2C 1 (3.3V), or any 3V3 pad | none | VIN (or 3V3 if the module has one) |
| I2C 2 (GND), or any GND | none | GND |
| Extended IO 1 | IO2 | BCK |
| Extended IO 3 | IO14 | LCK (LRCK, the left and right clock) |
| Extended IO 4 | IO21 | DIN |
| Extended IO 2 | IO3 | not used here, it is the nRF wake wire |
| none | none | SCK to GND (pads on the module) |

Module pad settings (bottom pads, check your silkscreen): FLT low, DEMP low, XSMT high (unmutes, silent if left low), FMT low (I2S). Output is line level on the 3.5 mm jack or the L, R and GND pins.

## Keyboard connector to nRF (as built)

The A-11 keyboard connector has 26 pins. Turn the keyboard face down so the connector traces on the PCB are visible and count from right to left. Pin 1 is not connected and has no wire. Wire order is arbitrary because the scanner discovers the matrix, this table is what was built.

| Keyboard connector pin | nRF pad | Header position |
|---|---|---|
| 1 | none | none |
| 2 | GND | any GND pad |
| 3 | 29 | J3 7 |
| 4 | 28 | J3 8 |
| 5 | 24 | J3 10 |
| 6 | 23 | J3 11 |
| 7 | 22 | J3 12 |
| 8 | 20 | J3 13 |
| 9 | 19 | J3 14 |
| 10 | 18 | J3 15 |
| 11 | 17 | J3 16 |
| 12 | 16 | J3 17 |
| 13 | 2 | J4 6 |
| 14 | 3 | J4 7 |
| 15 | 4 | J4 8 |
| 16 | 5 | J4 9 |
| 17 | 8 | J4 10 |
| 18 | 11 | J4 13 |
| 19 | 12 | J4 14 |
| 20 | 13 | J4 15 |
| 21 | 14 | J4 16 |
| 22 | 15 | J4 17 |
| 23 | 31 | J3 5 |
| 24 | 9 | J4 11 |
| 25 | 10 | J4 12 |
| 26 | 21 | J3 4 |

Pins 24, 25 and 26 land on pads 9, 10 and 21, which are normally the NFC and reset pins. They need `tools/nrf.py freepads CONFIRM` once before soldering. That setting also removes the hardware reset pin, so the board's reset button reads as a key and the ESP resets the nRF over SWD instead. The crystal pads 0 and 1 are untouched. Pin 2 is the keyboard ground, tied to the nRF GND for ESD safety, so pad 30 (J3 position 6) is a spare clean pin. Which key sits on which pins is in keymap/A11_KEYBOARD.md.

## Before first power

1. No wire touches VIN. RESET only gets keyboard pin 26 after the freepads setting.
2. Meter beep between nRF 3.3V and GND shows no short.
3. Meter between every flex wire and its neighbors shows no short.
4. Plug in the ESP over USB, measure 3.3 V on the nRF 3.3V pad, then continue.
