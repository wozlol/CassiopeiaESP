#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "app_exit.h"
// Runs the media viewer and player on the screen until the person leaves it. Keys and touch come from the two queues.
app_exit_t media_app(QueueHandle_t keys, QueueHandle_t touch);
// The same viewer and player, started straight on one file. Esc or the end of the sound returns to the caller.
app_exit_t media_app_file(QueueHandle_t keys, QueueHandle_t touch, const char *path);
