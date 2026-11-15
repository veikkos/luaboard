/* ESP32 mock platform - the update checks' simulator options: where the
 * release manifest and the app marketplace are (none by default: the
 * simulator never reaches GitHub on its own) and how often they are
 * checked. */
#include <stdio.h>

#include "sim_hw.h"

static char s_url[512];
static uint32_t s_every;                /* 0: the firmware's own interval */

void sim_update_set_url(const char *url) { snprintf(s_url, sizeof s_url, "%s", url ? url : ""); }

const char *sim_update_url(void) { return s_url[0] ? s_url : NULL; }

void sim_update_set_every(uint32_t ms) { s_every = ms; }

static char s_apps[512];

void sim_apps_set_url(const char *url) { snprintf(s_apps, sizeof s_apps, "%s", url ? url : ""); }

const char *sim_apps_url(void) { return s_apps[0] ? s_apps : NULL; }

uint32_t sim_update_every(void) { return s_every; }
