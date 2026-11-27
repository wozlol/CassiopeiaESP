#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "app_exit.h"
// File manager for the SD card. Browse, delete, cut and paste (move), new folder. Runs until the person leaves it.
app_exit_t files_app(QueueHandle_t keys, QueueHandle_t touch);
