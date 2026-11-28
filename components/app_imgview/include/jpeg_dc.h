#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
// A one eighth size picture from a progressive JPEG, panel ordered RGB565. *ow and *oh are its size, *full_w and *full_h the real picture size.
uint16_t *jpegdc_decode(const uint8_t *d, size_t n, int *ow, int *oh, int *full_w, int *full_h, char *err, size_t errcap);
