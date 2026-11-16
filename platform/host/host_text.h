/* The host layer's text parsing, the same on every computer (host_text.c).
 * Internal to platform/host. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/** --control's [HOST:]PORT: the port, 0..65535 (0: any free one), or -1
 *  after saying what's wrong on stderr. */
long host_ctl_port_parse(const char *spec);

/** The value of `key` in a handover string ("k=v,k=v") into out[cap];
 *  false if it isn't there. */
bool host_handover_get(const char *handover, const char *key, char *out, size_t cap);

/** How many simulators can run at once, each with its own link name. */
#define HOST_LINK_FAMILY_MAX 16
/** The n-th simulator's link name (1..HOST_LINK_FAMILY_MAX): esp32-sim, then
 *  esp32-sim-2 ... No directory or pipe prefix; the tools look for the same
 *  names (pc-tool/src/link.rs, sim_name). */
void host_link_family(int n, char *out, size_t cap);
