#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "app_exit.h"
// File manager for the SD card. Browse, delete, cut and paste (move), new folder. Runs until the person leaves it.
app_exit_t files_app(QueueHandle_t keys, QueueHandle_t touch);
// A program can hand the file manager a way to open sound files. It is given the full path and returns when the person is done with it.
void files_set_sound_handler(void (*handler)(const char *path));
// A text, Markdown or HTML file the person opened. The file manager returns, and this gives the path once so the reader can show it. NULL when there is none.
const char *files_take_doc(void);
