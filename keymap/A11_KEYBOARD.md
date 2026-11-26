# Casio Cassiopeia A-11 keyboard map

Electrical map of the A-11 keyboard (marking z305-KY C140360-1), measured with the scanner in nrf/kbscan and trained with tools/kbtrain.py. Plain passive matrix, no diodes. Every key joins exactly one row pin to one column pin.

## Connector pins

The keyboard connector has 26 pins. Turn the keyboard face down so the connector traces on the PCB are visible, then count from right to left. Pin 1 is not connected and was not wired. Pin 2 is the keyboard ground and is wired to an nRF GND pad.

| Connector pin | nRF pad | nRF header | Side |
|---|---|---|---|
| 1 | none | none | not connected |
| 2 | GND | GND pad | ground |
| 3 | 29 | J3 7 | row |
| 4 | 28 | J3 8 | column |
| 5 | 24 | J3 10 | column |
| 6 | 23 | J3 11 | column |
| 7 | 22 | J3 12 | column |
| 8 | 20 | J3 13 | column |
| 9 | 19 | J3 14 | column |
| 10 | 18 | J3 15 | column |
| 11 | 17 | J3 16 | column |
| 12 | 16 | J3 17 | row |
| 13 | 2 | J4 6 | row |
| 14 | 3 | J4 7 | row |
| 15 | 4 | J4 8 | row |
| 16 | 5 | J4 9 | row |
| 17 | 8 | J4 10 | row |
| 18 | 11 | J4 13 | row |
| 19 | 12 | J4 14 | row |
| 20 | 13 | J4 15 | row |
| 21 | 14 | J4 16 | column |
| 22 | 15 | J4 17 | column |
| 23 | 31 | J3 5 | column |
| 24 | 9 | J4 11 | column |
| 25 | 10 | J4 12 | column |
| 26 | 21 | J3 4 | column |

Pad numbers are the SparkFun WRL-13990 silkscreen numbers, which are nRF52832 P0.xx numbers. Pads 9, 10 and 21 are normally NFC and reset pins and need `tools/nrf.py freepads CONFIRM` once.

## Matrix

Rows are connector pins down the side, columns are connector pins across the top.

| row \ col | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 21 | 22 | 23 | 24 | 25 | 26 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 3 |  |  |  |  |  |  |  |  |  |  |  |  |  | on |
| 12 | Backspace | Equals | Minus | 0 | 9 | Tab | Escape | Grave |  |  |  |  |  |  |
| 13 | 8 | 7 | 6 | 5 | 4 | 3 | 2 | 1 |  |  |  |  |  |  |
| 14 | i | u | y | t | r | e | w | q |  |  |  |  |  |  |
| 15 | k | j | h | g | f | d | s | a |  |  |  |  |  |  |
| 16 | l | m | n | b | v | c | x | z |  |  |  |  |  |  |
| 17 | o | p | RightBracket | LeftBracket | Backslash | Apostrophe | Enter |  |  |  |  |  |  |  |
| 18 | Right | Left | Down | Up | Semicolon | Slash | Period | Comma |  |  |  |  |  |  |
| 19 |  |  |  |  |  | backlight | off | Space |  |  |  |  |  |  |
| 20 |  |  |  |  |  |  |  |  | LeftCtrl | LeftAlt | LeftShift | CapsLock | LeftGUI |  |

## Keys

Names are USB HID Keyboard/Keypad page 0x07 names where one exists. Keys with no HID usage are device buttons.

| Key | HID usage | Pins |
|---|---|---|
| on | none | 3 and 26 |
| Backspace | 0x2A | 4 and 12 |
| 8 | 0x25 | 4 and 13 |
| i | 0x0C | 4 and 14 |
| k | 0x0E | 4 and 15 |
| l | 0x0F | 4 and 16 |
| o | 0x12 | 4 and 17 |
| Right | 0x4F | 4 and 18 |
| Equals | 0x2E | 5 and 12 |
| 7 | 0x24 | 5 and 13 |
| u | 0x18 | 5 and 14 |
| j | 0x0D | 5 and 15 |
| m | 0x10 | 5 and 16 |
| p | 0x13 | 5 and 17 |
| Left | 0x50 | 5 and 18 |
| Minus | 0x2D | 6 and 12 |
| 6 | 0x23 | 6 and 13 |
| y | 0x1C | 6 and 14 |
| h | 0x0B | 6 and 15 |
| n | 0x11 | 6 and 16 |
| RightBracket | 0x30 | 6 and 17 |
| Down | 0x51 | 6 and 18 |
| 0 | 0x27 | 7 and 12 |
| 5 | 0x22 | 7 and 13 |
| t | 0x17 | 7 and 14 |
| g | 0x0A | 7 and 15 |
| b | 0x05 | 7 and 16 |
| LeftBracket | 0x2F | 7 and 17 |
| Up | 0x52 | 7 and 18 |
| 9 | 0x26 | 8 and 12 |
| 4 | 0x21 | 8 and 13 |
| r | 0x15 | 8 and 14 |
| f | 0x09 | 8 and 15 |
| v | 0x19 | 8 and 16 |
| Backslash | 0x31 | 8 and 17 |
| Semicolon | 0x33 | 8 and 18 |
| Tab | 0x2B | 9 and 12 |
| 3 | 0x20 | 9 and 13 |
| e | 0x08 | 9 and 14 |
| d | 0x07 | 9 and 15 |
| c | 0x06 | 9 and 16 |
| Apostrophe | 0x34 | 9 and 17 |
| Slash | 0x38 | 9 and 18 |
| backlight | none | 9 and 19 |
| Escape | 0x29 | 10 and 12 |
| 2 | 0x1F | 10 and 13 |
| w | 0x1A | 10 and 14 |
| s | 0x16 | 10 and 15 |
| x | 0x1B | 10 and 16 |
| Enter | 0x28 | 10 and 17 |
| Period | 0x37 | 10 and 18 |
| off | none | 10 and 19 |
| Grave | 0x35 | 11 and 12 |
| 1 | 0x1E | 11 and 13 |
| q | 0x14 | 11 and 14 |
| a | 0x04 | 11 and 15 |
| z | 0x1D | 11 and 16 |
| Comma | 0x36 | 11 and 18 |
| Space | 0x2C | 11 and 19 |
| LeftCtrl | 0xE0 | 20 and 21 |
| LeftAlt | 0xE2 | 20 and 22 |
| LeftShift | 0xE1 | 20 and 23 |
| CapsLock | 0x39 | 20 and 24 |
| LeftGUI | 0xE3 | 20 and 25 |

## Ghosting

Without diodes, holding three keys at the corners of a rectangle in the matrix reads as a fourth key. Keys that sit in no such rectangle never ghost: CapsLock, LeftAlt, LeftCtrl, LeftGUI, LeftShift, on.

