#include "web_origin.h"

#include <string.h>

static bool ascii_equal(const char *left, const char *right)
{
    if (!left || !right) return false;
    while (*left && *right) {
        unsigned char a = (unsigned char)*left++, b = (unsigned char)*right++;
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return false;
    }
    return *left == *right;
}

static unsigned authority(const char *value)
{
    if (ascii_equal(value, LOCAL_WEB_HOST) || ascii_equal(value, LOCAL_WEB_HOST ":80")) return 1;
    if (ascii_equal(value, LOCAL_WEB_IP) || ascii_equal(value, LOCAL_WEB_IP ":80")) return 2;
    return 0;
}

bool web_origin_allowed(const char *host, const char *origin, const char *fetch_site)
{
    unsigned host_authority = authority(host);
    if (!host_authority || !origin || strncmp(origin, "http://", 7)) return false;
    if (authority(origin + 7) != host_authority) return false;
    return !fetch_site || !strcmp(fetch_site, "same-origin");
}
