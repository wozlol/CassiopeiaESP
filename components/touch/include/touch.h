// FT6336U capacitive touch on the shared I2C bus. Events carry landscape screen coordinates (x 0..479, y 0..319).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum { TOUCH_DOWN, TOUCH_MOVE, TOUCH_UP } touch_type_t;
typedef struct {
    touch_type_t type;
    int16_t x, y;
} touch_event_t;

// Starts the reader. Events go to the queue. Returns false if the touch chip does not answer.
bool touch_start(QueueHandle_t events);

void touch_sleep(void);
