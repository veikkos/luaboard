/* ESP32 mock platform - NimBLE port: the host task. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MOCK_TASKFUNCTION_T
#define MOCK_TASKFUNCTION_T
typedef void (*TaskFunction_t)(void *);
#endif

/** Start the host task: host_task_fn runs nimble_port_run(). */
void nimble_port_freertos_init(TaskFunction_t host_task_fn);
void nimble_port_freertos_deinit(void);

#ifdef __cplusplus
}
#endif
