#include "app_config.h"

#include <stdarg.h>
#include <stdio.h>

#include "sdkconfig.h"
#include "esp_log.h"

#ifndef CONFIG_STATION_TERMINAL_PRINT_DEFAULT
#define CONFIG_STATION_TERMINAL_PRINT_DEFAULT 0
#endif

/* Change the menuconfig default, or use the setter to change this at runtime. */
atomic_bool g_terminal_print_enabled = ATOMIC_VAR_INIT(CONFIG_STATION_TERMINAL_PRINT_DEFAULT);

static _Atomic(vprintf_like_t) previous_log_output = ATOMIC_VAR_INIT(vprintf);
static bool initialized;

static int terminal_log_output(const char *format, va_list arguments)
{
    if (!atomic_load_explicit(&g_terminal_print_enabled, memory_order_relaxed)) {
        return 0;
    }
    vprintf_like_t output = atomic_load_explicit(&previous_log_output, memory_order_acquire);
    return output(format, arguments);
}

void app_config_init(void)
{
    if (initialized) {
        return;
    }
    /* ESP-IDF may call this handler concurrently; only the atomic flag changes. */
    vprintf_like_t original = esp_log_set_vprintf(terminal_log_output);
    if (original != NULL && original != terminal_log_output) {
        atomic_store_explicit(&previous_log_output, original, memory_order_release);
    }
    initialized = true;
}

bool app_config_terminal_print_enabled(void)
{
    return atomic_load_explicit(&g_terminal_print_enabled, memory_order_relaxed);
}

void app_config_set_terminal_print(bool enabled)
{
    /* Keep normal IDF log-level filtering; toggling must not change verbosity. */
    atomic_store_explicit(&g_terminal_print_enabled, enabled, memory_order_relaxed);
}
