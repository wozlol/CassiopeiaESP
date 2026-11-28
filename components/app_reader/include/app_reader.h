#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "app_exit.h"
// Runs the reference reader on the screen until the person leaves it. Keys and touch come from the two queues.
app_exit_t reader_app(QueueHandle_t keys, QueueHandle_t touch);
// When set, the top of the library list has a Files row under Search. The handler runs the file manager and returns when the person leaves it.
void reader_set_files_handler(const char *(*handler)(void));   // returns the path of a document to show, or NULL
