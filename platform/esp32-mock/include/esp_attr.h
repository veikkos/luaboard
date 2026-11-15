/* ESP32 mock platform - memory placement attributes (esp_attr.h).
 *
 * The host has one kind of memory. RTC_NOINIT_ATTR data, which on the chip
 * survives a reset, is ordinary memory here: the simulator carries what the
 * firmware keeps there (the crash record, crash.h) through its restart. */
#pragma once

#define RTC_NOINIT_ATTR
#define RTC_DATA_ATTR
#define IRAM_ATTR
#define DRAM_ATTR
