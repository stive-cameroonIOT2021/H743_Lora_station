#pragma once

#include <stdbool.h>

#define LOCAL_WEB_HOSTNAME "espground"
#define LOCAL_WEB_HOST "espground.local"
#define LOCAL_WEB_IP "192.168.4.1"
#define LOCAL_WEB_URL "http://espground.local"

/* HTTP default port 80 is equivalent to an omitted port. Only the configured
 * local name and fixed AP address are accepted, and the Origin must identify
 * the same authority used by Host. NULL fetch_site means header not supplied. */
bool web_origin_allowed(const char *host, const char *origin, const char *fetch_site);
