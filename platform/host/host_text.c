#include "host_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

long host_ctl_port_parse(const char *spec)
{
    /* The host is this computer's. */
    const char *colon = strrchr(spec, ':'), *start = colon ? colon + 1 : spec;
    char *end;
    long port = strtol(start, &end, 10);
    if (end == start || *end || port < 0 || port > 65535) {
        fprintf(stderr, "--control %s: the control channel is a TCP port: [127.0.0.1:]PORT\n", spec);
        return -1;
    }
    return port;
}

bool host_handover_get(const char *h, const char *key, char *out, size_t cap)
{
    size_t k = strlen(key);
    for (const char *p = h; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL) {
        if (!strncmp(p, key, k) && p[k] == '=') {
            p += k + 1;
            snprintf(out, cap, "%.*s", (int)strcspn(p, ","), p);
            return true;
        }
    }
    return false;
}
