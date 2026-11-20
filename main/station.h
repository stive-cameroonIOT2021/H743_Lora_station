#pragma once

#include "esp_err.h"
#include "cJSON.h"

esp_err_t station_start(void);
void station_poll(void);
cJSON *station_web_snapshot(void);
cJSON *station_web_live_snapshot(void);
