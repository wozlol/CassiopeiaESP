#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "app_exit.h"
// Runs the reference reader on the screen until the person leaves it. Keys and touch come from the two queues.
app_exit_t reader_app(QueueHandle_t keys, QueueHandle_t touch);
