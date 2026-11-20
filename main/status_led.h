#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    STATUS_LED_ERROR = 0,
    STATUS_LED_KEY_MISSING,
    STATUS_LED_WAITING,
    STATUS_LED_CONNECTED,
    STATUS_LED_DEGRADED,
    STATUS_LED_LOST,
} status_led_state_t;

/* WS2812 on GPIO21 per the ESP32-S3-Zero board datasheet, configurable in menuconfig. */
esp_err_t status_led_init(void);

/* Single main-loop owner. Nonblocking; a busy RMT transfer defers the update. */
void status_led_update(status_led_state_t state, int64_t now_us);

/* Safe for HTTP snapshots while the station task updates the LED. */
bool status_led_ready(void);
const char *status_led_state_name(status_led_state_t state);
