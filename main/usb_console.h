#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

typedef void (*usb_line_fn)(const char *line, void *context);
esp_err_t usb_console_open(void);
void usb_console_poll(usb_line_fn callback, void *context);
bool usb_console_send(cJSON *message);
uint32_t usb_console_dropped(void);
