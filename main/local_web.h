#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

/* The provider returns a newly allocated object, owned by the HTTP handler.
 * It runs on the HTTP task, so it must take its own short snapshot locks.
 * Never include shared keys or Wi-Fi passwords in that object. */
typedef cJSON *(*local_web_snapshot_fn)(void);

esp_err_t local_web_start(local_web_snapshot_fn snapshot_fn, local_web_snapshot_fn live_snapshot_fn);
bool local_web_is_ready(void);
uint32_t local_web_client_count(void);
const char *local_web_ip(void);
const char *local_web_hostname(void);
const char *local_web_url(void);
bool local_web_mdns_is_ready(void);
esp_err_t local_web_mdns_error(void);
