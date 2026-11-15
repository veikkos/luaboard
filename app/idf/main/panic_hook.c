/*
 * The chip's side of the crash record (app/src/crash.h): ESP-IDF's panic
 * handler is wrapped (the linker's --wrap=esp_panic_handler, CMakeLists.txt)
 * so that before it prints and reboots, the cause and a stack trace go into
 * RTC memory for the next boot to show. The trace is taken as ESP-IDF's own
 * backtrace is (esp_backtrace_print_from_frame), so its addresses are the
 * ones the panic output prints. Xtensa (the ESP32-S3) only.
 *
 * esp_panic_handler is ESP-IDF's internal entry (esp_private): VERIFY after
 * an ESP-IDF update, and on hardware.
 */
#include <stdint.h>
#include <string.h>

#include "crash.h"
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "esp_memory_utils.h"
#include "esp_private/panic_internal.h"
#include "ota.h"
#include "xtensa_context.h"

extern bool g_twdt_isr;                          /* task_wdt.c: the panic comes from the task watchdog */

void __real_esp_panic_handler(panic_info_t *info);
void __wrap_esp_panic_handler(panic_info_t *info);

/* Copy at most cap-1 characters, always terminated; nothing that could fail. */
static void put(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    for (; src && src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = 0;
}

static void hex(char *dst, uint32_t v)           /* "0x4200a1b4": 11 bytes */
{
    static const char d[] = "0123456789abcdef";
    dst[0] = '0';
    dst[1] = 'x';
    for (int i = 0; i < 8; i++) dst[2 + i] = d[(v >> (28 - 4 * i)) & 15];
    dst[10] = 0;
}

static void fill(const panic_info_t *info)
{
    crash_record_t *r = &crash_rec;
    const XtExcFrame *xf = info->frame;
    if (g_panic_abort) {
        put(r->cause, sizeof r->cause, g_twdt_isr ? "task watchdog" : "abort()");
    } else {
        put(r->cause, sizeof r->cause, info->reason ? info->reason : "exception");
        if (info->exception == PANIC_EXCEPTION_FAULT && xf) {   /* the data address of a bad load or store */
            size_t n = strlen(r->cause);
            if (n + 18 < sizeof r->cause) {
                memcpy(r->cause + n, ", data ", 7);
                hex(r->cause + n + 7, (uint32_t)xf->excvaddr);
            }
        }
    }
    put(r->version, sizeof r->version, fw_version());
    r->frames = 0;
    if (!xf) return;
    esp_backtrace_frame_t f = { .pc = xf->pc, .sp = xf->a1, .next_pc = xf->a0, .exc_frame = xf };
    hex(r->frame[r->frames++], esp_cpu_process_stack_pc(f.pc));
    if (!esp_stack_ptr_is_sane(f.sp)) return;
    while (r->frames < CRASH_FRAMES && f.next_pc != 0) {
        if (!esp_backtrace_get_next_frame(&f)) break;   /* the stack is corrupted from here */
        hex(r->frame[r->frames++], esp_cpu_process_stack_pc(f.pc));
    }
}

void __wrap_esp_panic_handler(panic_info_t *info)
{
    static bool once;                            /* a crash in here mustn't loop */
    if (!once) {
        once = true;
        fill(info);
        crash_seal(&crash_rec);
    }
    __real_esp_panic_handler(info);
}
