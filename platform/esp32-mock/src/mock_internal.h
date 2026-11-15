/* Helpers shared between mock implementation files (not part of the API). */
#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "host.h"

/* Byte I/O on the host's serial link, shared by the UART and USB-Serial/JTAG
   mocks. With no link attached the line is idle and writes vanish. */
int  mock_link_read(host_link_t *link, void *buf, uint32_t length, TickType_t ticks_to_wait);
int  mock_link_write(host_link_t *link, const void *src, size_t size, int stall_timeout_ms);
/* For the mock's own host threads (e.g. the simulated Wi-Fi association):
   wait until `ms` of simulated time have passed. Unlike vTaskDelay, this
   never uses up a fast-forward (sim_time_advance), it only follows it. */
void mock_sleep_ms(uint32_t ms);
/* The same until the chip's clock (esp_timer_get_time) reads `deadline_us`. */
void mock_sleep_until_us(int64_t deadline_us);
/* A fast-forward waits for a thread that woke from one of these until it
   waits again, or says here that it has nothing more to do. */
void sim_time_idle(void);
/* For a thread of the mock that waits for work: pthread_cond_wait, _signal
   and _broadcast that a fast-forward keeps in step with (it waits for a
   thread given work until that thread waits again), and a wait that also
   ends when the chip's clock reads `at_us` (INT64_MAX: for work only);
   with a time it can return early. */
int sim_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int sim_cond_wait_until(pthread_cond_t *cond, pthread_mutex_t *mutex, int64_t at_us);
int sim_cond_signal(pthread_cond_t *cond);
int sim_cond_broadcast(pthread_cond_t *cond);
/* The firmware's connections to servers: `s` is one it makes (connect), it
   sent something on it, something came in. A fast-forward waits for the
   answer to what was sent to have arrived, as on the chip, where a server
   answers in a tick or two and not after seconds of the chip's time. */
void sim_net_client(int s);
void sim_net_sent(int s);
/* `s` is a socket the firmware accepted or bound: no connection it made. */
void sim_net_forget(int s);
/** Whether a read of the PC link that doesn't wait should look at it now:
 *  not in every pass of a fast-forward (mock_core.c). */
bool sim_link_look(void);
void sim_net_received(int s);
/** The firmware read from `s` in this pass of its loop (or found nobody to
 *  accept on it): input on it ends the tick's wait (mock_core.c). */
void sim_net_waits(int s);
/* This image is made to crash 1 s into every boot (fail=boot in its tag,
   mock_ota.c), for rollback tests. */
bool mock_ota_fail_boot(void);
/* Reset the chip with this reason for the next boot (esp_restart: ESP_RST_SW). */
HOST_NORETURN void mock_restart(esp_reset_reason_t reason);
