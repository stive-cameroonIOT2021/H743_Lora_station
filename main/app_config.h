#pragma once

#include <stdbool.h>
#include <stdatomic.h>

/* Human-readable ESP_LOG output only. Native USB NDJSON is a separate path. */
extern atomic_bool g_terminal_print_enabled;

/* Call once, at the beginning of app_main, before creating other tasks. */
void app_config_init(void);
bool app_config_terminal_print_enabled(void);
void app_config_set_terminal_print(bool enabled);
