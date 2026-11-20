#ifndef VFPR_TELEMETRY_CACHE_H
#define VFPR_TELEMETRY_CACHE_H

#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"
#include "link_protocol.h"

/* Only authenticated, nonduplicate aircraft frames belong here. The station
 * owns radio/session replay protection; this module validates payloads. */
esp_err_t telemetry_cache_init(void);
void telemetry_cache_reset(uint64_t session);
esp_err_t telemetry_cache_accept(const lp_frame_t *frame, int64_t received_us);
/* Thread safe. Caller owns the returned object; NULL means allocation failed.
 * All ages use the station monotonic clock, with source reading ages added.
 * Shape matches Python GroundCache categories, plus last_result and cache. */
cJSON *telemetry_cache_snapshot(int64_t now_us);
/* Flight instruments and plots only. Copies selected cache fields instead of
 * cloning the complete diagnostic cache, with identical source-age semantics. */
cJSON *telemetry_cache_live_snapshot(int64_t now_us);

#endif
