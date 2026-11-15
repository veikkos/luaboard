/* ESP32 mock platform - the update check's simulator options: where the
 * release manifest is (none by default: the simulator never reaches GitHub
 * on its own) and how often it is checked. */
#include <stdio.h>

#include "sim_hw.h"

static char s_url[512];
static uint32_t s_every;                /* 0: the firmware's own interval */

void sim_update_set_url(const char *url) { snprintf(s_url, sizeof s_url, "%s", url ? url : ""); }

const char *sim_update_url(void) { return s_url[0] ? s_url : NULL; }

void sim_update_set_every(uint32_t ms) { s_every = ms; }

uint32_t sim_update_every(void) { return s_every; }
