/* ESP32 mock platform - NimBLE port: start and run the host. */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nimble_port_init(void);
/** The host's event loop. Runs on the host task until nimble_port_stop(). */
void nimble_port_run(void);
int nimble_port_stop(void);

#ifdef __cplusplus
}
#endif
