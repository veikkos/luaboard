/*
 * ESP32 board simulator - generic part.
 *
 * Runs the unmodified firmware (app_main) natively in a thread on top of the
 * ESP-IDF mock, connects the board's PC link (USB-Serial/JTAG or UART) to the
 * host's serial link (host.h: a pseudo-terminal, or a named pipe on Windows),
 * and lets the selected board (platform/sim/boards/<board>/) attach its device
 * models and draw itself in an SDL window.
 *
 * Boots go through the bootloader, as on the device. The executable you
 * start opens the serial link and the control channel, runs the bootloader
 * (sim_ota_boot, which picks an app slot) and replaces itself (host_become)
 * with that slot's image. A reset (esp_restart, a crash) does the same again:
 * the image replaces itself with itself to run the bootloader, which may
 * pick the other slot. The link and the control channel are handed over
 * (--boot), so a PC tool stays connected through a reboot; Wi-Fi clients are
 * dropped and the window opens again, as the board's screen goes dark. The
 * world's clock and the board's inputs carry over (--keep).
 *
 * With --watch, a new build of the executable you started is flashed like
 * `luaboard flash` would, into ota_0, and booted. Apps and settings stay.
 */
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ble_device.h"
#include "crash.h"
#include "board.h"
#include "esp_timer.h"
#include "host.h"
#include "ota.h"
#include "sim_audio.h"
#include "sim_board.h"
#include "sim_hw.h"
#include "sim_window.h"

#ifdef _WIN32
#define LINK_HELP "pipe NAME: \\\\.\\pipe\\NAME (default: the first free\n" \
                  "                      of esp32-sim, esp32-sim-2 ... esp32-sim-16)"
#else
#define LINK_HELP "symlink PATH -> serial pty (default: the first\n" \
                  "                      free of /tmp/esp32-sim, /tmp/esp32-sim-2 ... -16)"
#endif

extern void app_main(void);

static volatile sig_atomic_t s_quit;


/* -------------------------------------------------------------- options */

#define MAX_ARGS 96

typedef struct {
    int headless, run_ms, scale, watch, loopback;
    const char *link, *nvs, *flash, *control, *ppm, *shot, *ble, *wav;
    int mute;
    const char *sets[32], *ble_devs[8];
    int nsets, nble_devs;
    /* Handed over from the last boot (internal): */
    const char *boot;          /* the descriptors and why it boots, see boot_t */
    const char *origin;        /* the executable started by hand */
    const char *keep;          /* the board's inputs (sim_board_state) */
    /* What was given on the command line, for the next boot. */
    char *user[MAX_ARGS];
    int nuser;
} opts_t;

static opts_t s_o;

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [options]                          (board: %s)\n"
        "  --model NAME        board variant: %s\n"
        "  --headless          no window (CI / tests)\n"
        "  --loopback          servers (Wi-Fi link, net.listen) take 127.0.0.1 only:\n"
        "                      not reachable from other computers, no firewall prompt\n"
        "  --link PATH         the serial link: " LINK_HELP "\n"
        "  --nvs FILE          NVS flash backing file (default: esp32-sim-nvs.bin)\n"
        "  --flash DIR         folder holding the apps partition (default: <nvs FILE>.flash)\n"
        "  --run-ms N          exit after N milliseconds (of the chip's clock)\n"
        "  --watch             flash a new build of this executable as\n"
        "                      soon as it's there, and boot it; apps and settings stay\n"
        "  --scale N           window points per LCD pixel, 1-8 (default: as last\n"
        "                      time, or 2); the window can be resized\n"
        "  --set KEY=VALUE     board input: %s\n"
        "                      time=EPOCH: the world's clock (NTP, RTC chip) starts\n"
        "                      at these UTC seconds and runs with the chip's\n"
        "                      update_url=URL: check for updates there (default:\n"
        "                      none, no checks); update_every=MS: that often\n"
        "                      apps_url=URL: the app marketplace (default: none)\n"
        "                      wifi_net=NAME: the network the board joins while it\n"
        "                      has none set up (default: none)\n"
        "                      fw_version=V: the version the firmware reports\n"
        "  --control PORT      a TCP port on 127.0.0.1 ([127.0.0.1:]PORT, 0: any free\n"
        "                      one, printed) taking KEY=VALUE lines (the --set keys)\n"
        "                      while running, advance=MS to fast-forward the clock\n"
        "                      and quit=1 to power off; each is answered on the same\n"
        "                      connection. `luaboard sim-ctl button=B` sends them\n"
        "  --ble host|sim      Bluetooth devices: host = this computer's Bluetooth\n"
        "                      too (default with a window), sim = simulated only\n"
        "  --ble-device FILE   a simulated Bluetooth device described in JSON\n"
        "                      (repeatable; --set %s)\n"
        "  --mute              no sound from this computer's speakers (window mode)\n"
        "  --wav FILE          record what the speaker plays as a WAV file\n"
        "  --ppm FILE          write the LCD image as PPM on exit\n"
        "  --screenshot FILE   save the window as BMP on exit (window mode)\n",
        argv0, BOARD_NAME, sim_board_models(), sim_board_set_help(), ble_device_set_help());
}

/* 0 if fine, -1 for --help, else the exit code. */
static int parse_args(int argc, char **argv, opts_t *o)
{
    memset(o, 0, sizeof *o);
    o->nvs = "esp32-sim-nvs.bin";
    /* A reboot may run an older or newer image than the one started: an
       option it doesn't know is skipped, not a reason to stop. */
    bool reboot = false;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--boot")) reboot = true;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
        int first = i;
        if (!strcmp(a, "--boot") && next) { o->boot = next; i++; continue; }
        if (!strcmp(a, "--origin") && next) { o->origin = next; i++; continue; }
        if (!strcmp(a, "--keep") && next) { o->keep = next; i++; continue; }
        if (!strcmp(a, "--headless")) o->headless = 1;
        else if (!strcmp(a, "--watch")) o->watch = 1;
        else if (!strcmp(a, "--loopback")) o->loopback = 1;
        else if (!strcmp(a, "--model") && next) {
            if (!sim_board_set_model(next)) { fprintf(stderr, "unknown --model %s (%s)\n", next, sim_board_models()); return 2; }
            i++;
        }
        else if (!strcmp(a, "--link") && next) { o->link = next; i++; }
        else if (!strcmp(a, "--nvs") && next) { o->nvs = next; i++; }
        else if (!strcmp(a, "--flash") && next) { o->flash = next; i++; }
        else if (!strcmp(a, "--control") && next) { o->control = next; i++; }
        else if (!strcmp(a, "--run-ms") && next) { o->run_ms = atoi(next); i++; }
        else if (!strcmp(a, "--scale") && next) { o->scale = atoi(next); i++; }
        else if (!strcmp(a, "--set") && next && o->nsets < 32) { o->sets[o->nsets++] = next; i++; }
        else if (!strcmp(a, "--ppm") && next) { o->ppm = next; i++; }
        else if (!strcmp(a, "--wav") && next) { o->wav = next; i++; }
        else if (!strcmp(a, "--mute")) o->mute = 1;
        else if (!strcmp(a, "--ble-device") && next && o->nble_devs < 8) { o->ble_devs[o->nble_devs++] = next; i++; }
        else if (!strcmp(a, "--ble") && next && (!strcmp(next, "host") || !strcmp(next, "sim"))) { o->ble = next; i++; }
        else if (!strcmp(a, "--screenshot") && next) { o->shot = next; i++; }
        else if (reboot) fprintf(stderr, "warning: option %s not known to this firmware, skipped\n", a);
        else { usage(argv[0]); return (!strcmp(a, "-h") || !strcmp(a, "--help")) ? -1 : 2; }
        for (int k = first; k <= i && o->nuser < MAX_ARGS - 8; k++) o->user[o->nuser++] = argv[k];
    }
    if (o->scale < 0 || o->scale > 8) o->scale = 0;
    return 0;
}

/* ------------------------------------------------------------- handover */

/* What one boot hands the next (--boot): the serial link and the control
   channel (host_handover), the slot it runs (-1: run the bootloader first),
   whether that's a power-on (a new build is flashed), why it booted, the
   world's time if it went on, and how long the boots before ran (chip ms,
   for --run-ms). */
typedef struct {
    host_link_t *link;
    host_ctl_t *ctl;
    int slot, power;
    long long ran;
    esp_reset_reason_t reset;
    long long world;
    bool have_world;
} boot_t;

#define HANDOVER_MAX 1024

static boot_t s_boot = { .slot = -1 };
static char s_self[PATH_MAX];       /* this executable */
static char s_origin[PATH_MAX];     /* the one started by hand, for --watch */

static bool boot_parse(const char *s, boot_t *b)
{
    *b = (boot_t){ .slot = -1, .reset = ESP_RST_POWERON };
    char buf[HANDOVER_MAX];
    snprintf(buf, sizeof buf, "%s", s);
    for (char *save, *kv = strtok_r(buf, ",", &save); kv; kv = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(kv, '=');
        if (!eq) return false;
        *eq = 0;
        const char *k = kv, *v = eq + 1;
        if (!strcmp(k, "slot")) b->slot = atoi(v);
        else if (!strcmp(k, "power")) b->power = atoi(v);
        else if (!strcmp(k, "reset")) b->reset = !strcmp(v, "sw") ? ESP_RST_SW : !strcmp(v, "panic") ? ESP_RST_PANIC
                                               : !strcmp(v, "deepsleep") ? ESP_RST_DEEPSLEEP : ESP_RST_POWERON;
        else if (!strcmp(k, "world")) { b->world = strtoll(v, NULL, 10); b->have_world = true; }
        else if (!strcmp(k, "ran")) b->ran = strtoll(v, NULL, 10);
    }
    b->link = host_link_adopt(s);
    b->ctl = host_ctl_adopt(s);
    return b->link != NULL;
}

/* Become `image`, booting as `b` says, with the board's inputs `keep`.
   Only the serial link and the control channel (and stdio) go along: Wi-Fi
   clients and the window go, as on a board that resets. */
static HOST_NORETURN void become(const char *image, const boot_t *b, const char *keep)
{
    char ho[HANDOVER_MAX], world[40] = "";
    if (b->have_world) snprintf(world, sizeof world, ",world=%lld", b->world);
    host_handover(b->link, b->ctl, ho, sizeof ho);
    size_t len = strlen(ho);
    snprintf(ho + len, sizeof ho - len, ",slot=%d,power=%d,reset=%s,ran=%lld%s", b->slot, b->power,
             b->reset == ESP_RST_SW ? "sw" : b->reset == ESP_RST_PANIC ? "panic"
             : b->reset == ESP_RST_DEEPSLEEP ? "deepsleep" : "poweron", b->ran, world);
    char *argv[MAX_ARGS];
    int n = 0;
    argv[n++] = (char *)image;
    for (int i = 0; i < s_o.nuser; i++) argv[n++] = s_o.user[i];
    argv[n++] = "--origin";
    argv[n++] = s_origin;
    argv[n++] = "--boot";
    argv[n++] = ho;
    if (keep && *keep) { argv[n++] = "--keep"; argv[n++] = (char *)keep; }
    argv[n] = NULL;
    host_become(image, argv, b->link, b->ctl);
}

/* ------------------------------------------------------------- control */

/* --control PORT: "key=value" lines, applied like --set while running, and
 * advance=MS to fast-forward the chip's clock (sim_time_advance), skip=MS to
 * make it jump (sim_time_skip). Each line
 * is answered on the same connection, in order, with "ok LINE" or "error
 * LINE" once it has taken effect: an advance once the firmware has run
 * through the time. Lines after an advance wait for it, so "button=B
 * advance=1000 button=-" holds B for exactly a simulated second. Tests and
 * `luaboard sim-ctl` read the answers to know when to go on. */
static char s_ctl_buf[256];
static size_t s_ctl_len;
static char s_ctl_advance[64];          /* the advance line being run through, "" if none */
/* Held while lines are read and handled, so a reboot (from the firmware's
   thread) sees whole lines: none read but not answered. It keeps it. The
   owner is noted, so a crash knows whether its own thread holds it. */
static pthread_mutex_t s_ctl_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t s_ctl_owner;
static volatile sig_atomic_t s_ctl_owned;

static void ctl_lock(void)
{
    pthread_mutex_lock(&s_ctl_lock);
    s_ctl_owner = pthread_self();
    s_ctl_owned = 1;
}

static void ctl_unlock(void)
{
    s_ctl_owned = 0;
    pthread_mutex_unlock(&s_ctl_lock);
}

static bool ctl_mine(void) { return s_ctl_owned && pthread_equal(s_ctl_owner, pthread_self()); }

static void control_ack(const char *status, const char *line)
{
    char out[300];
    int n = snprintf(out, sizeof out, "%s %s\n", status, line);
    if (s_boot.ctl && n > 0) host_ctl_answer(s_boot.ctl, out, (size_t)n);
}

/* An input: the simulated Bluetooth devices' (ble*), else the board's. */
static bool sim_set(const char *key, const char *val)
{
    if (!strncmp(key, "ble", 3)) return ble_device_set(key, val);
    if (!strcmp(key, "time")) {                       /* the world's time: UTC seconds since 1970 */
        char *end;
        long long t = strtoll(val, &end, 10);
        if (!*val || *end || t < 0) return false;
        sim_world_set(t * 1000000LL);
        return true;
    }
    if (!strcmp(key, "update_url")) {                 /* the release manifest (none: no update checks) */
        sim_update_set_url(val);
        return true;
    }
    if (!strcmp(key, "wifi_net")) {                   /* the network a board with none set up joins */
        if (strlen(val) > 32) return false;
        sim_wifi_set_net(val);
        return true;
    }
    if (!strcmp(key, "apps_url")) {                   /* the app marketplace (none: no app checks) */
        sim_apps_set_url(val);
        return true;
    }
    if (!strcmp(key, "update_every")) {               /* ms between update checks */
        char *end;
        long ms = strtol(val, &end, 10);
        if (!*val || *end || ms < 1000) return false;
        sim_update_set_every((uint32_t)ms);
        return true;
    }
    if (!strcmp(key, "fw_version")) {                 /* the version the firmware reports */
        sim_ota_set_version(val);
        return true;
    }
    return sim_board_set(key, val);
}

/* ppm=FILE on the control channel: the LCD, written by the firmware's task
   at its next delay, between two passes of its loop: no flush is half
   through then. A board that is off, or a firmware that doesn't get to a
   delay within 200 ms (a busy script), is written as it stands. */
static int write_ppm(const char *path);
static pthread_mutex_t s_ppm_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_ppm_done = PTHREAD_COND_INITIALIZER;
static char s_ppm_path[PATH_MAX];                        /* "": none asked for (under s_ppm_lock) */
static int s_ppm_rc;
static atomic_bool s_ppm_asked;

static void ppm_at_delay(void)
{
    if (!atomic_load(&s_ppm_asked)) return;
    pthread_mutex_lock(&s_ppm_lock);
    if (*s_ppm_path) {
        s_ppm_rc = write_ppm(s_ppm_path);
        *s_ppm_path = 0;
        atomic_store(&s_ppm_asked, false);
        pthread_cond_broadcast(&s_ppm_done);
    }
    pthread_mutex_unlock(&s_ppm_lock);
}

static bool screen_to_file(const char *path)
{
    if (sim_powered_off()) return write_ppm(path) == 0;
    pthread_mutex_lock(&s_ppm_lock);
    snprintf(s_ppm_path, sizeof s_ppm_path, "%s", path);
    atomic_store(&s_ppm_asked, true);
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += 200 * 1000000L;
    if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    while (*s_ppm_path && pthread_cond_timedwait(&s_ppm_done, &s_ppm_lock, &until) == 0) {}
    if (*s_ppm_path) {                                   /* not at a delay in time: as it stands */
        s_ppm_rc = write_ppm(s_ppm_path);
        *s_ppm_path = 0;
        atomic_store(&s_ppm_asked, false);
    }
    int rc = s_ppm_rc;
    pthread_mutex_unlock(&s_ppm_lock);
    return rc == 0;
}

/* One control line; false if it's not a valid one (or, for the question
   is=STATE, if the answer is no: *quiet, as that's no mistake to log). */
static bool control_apply(char *line, bool *quiet)
{
    char *eq = strchr(line, '=');
    *quiet = false;
    if (!eq) return false;
    *eq = 0;
    const char *key = line, *val = eq + 1;
    bool ok;
    if (!strcmp(key, "advance")) {
        char *end;
        long ms = strtol(val, &end, 10);
        ok = *val && !*end && ms >= 0 && ms <= 24L * 3600 * 1000;
        if (ok) sim_time_advance((uint32_t)ms);
    } else if (!strcmp(key, "skip")) {
        /* The clock jumps ahead at once, also under a call that never waits. */
        char *end;
        long ms = strtol(val, &end, 10);
        ok = *val && !*end && ms >= 0 && ms <= 0x7fffffff;
        if (ok) sim_time_skip((uint32_t)ms);
    } else if (!strcmp(key, "is")) {
        /* A question, answered ok or error: is the board on, off (powered
           down), or asleep with light sleep allowed? */
        ok = !strcmp(val, "on") ? !sim_powered_off()
           : !strcmp(val, "off") ? sim_powered_off()
           : !strcmp(val, "light") ? !sim_powered_off() && sim_light_sleep_allowed() : false;
        *quiet = true;
    } else if (!strcmp(key, "pwm")) {
        /* A question too: pwm=PIN:off, or pwm=PIN:HZ:PERMILLE (the share
           of the time the pin is high, in thousandths). */
        int pin = -1, mille = -1, n = 0;
        unsigned want_hz = 0;
        uint32_t hz = 0;
        double duty = 0;
        *quiet = true;
        if (sscanf(val, "%d:off%n", &pin, &n) == 1 && n && !val[n]) ok = !sim_pwm_get(pin, &hz, &duty);
        else if (sscanf(val, "%d:%u:%d%n", &pin, &want_hz, &mille, &n) == 3 && !val[n])
            ok = sim_pwm_get(pin, &hz, &duty) && hz == want_hz && (int)(duty * 1000 + 0.5) == mille;
        else { ok = false; *quiet = false; }
    } else if (!strcmp(key, "pin")) {
        /* A question: pin=N:out|pullup|pulldown|floating: what the chip does
           to it; or pin=N:idle: is it exactly in ESP-IDF's reset state (not
           driven, the input off, the pull-up alone), as a pin no app uses? */
        int pin = -1, n = 0;
        *quiet = true;
        if (sscanf(val, "%d:%n", &pin, &n) == 1 && n) {
            const char *want = val + n;
            char k = sim_gpio_drive_kind((gpio_num_t)pin);
            if (!strcmp(want, "idle")) ok = sim_gpio_idle((gpio_num_t)pin);
            else
                ok = (k == 'o' && !strcmp(want, "out")) || (k == 'u' && !strcmp(want, "pullup")) ||
                     (k == 'd' && !strcmp(want, "pulldown")) || (k == 'f' && !strcmp(want, "floating"));
        } else { ok = false; *quiet = false; }
    } else if (!strcmp(key, "sdcard")) {
        /* A question: sdcard=mounted|free: does the firmware hold the card? */
        *quiet = true;
        if (!strcmp(val, "mounted")) ok = sim_sd_mounted();
        else if (!strcmp(val, "free")) ok = !sim_sd_mounted();
        else { ok = false; *quiet = false; }
    } else if (!strcmp(key, "sensor")) {
        /* A question: sensor=NAME:on|off (imu, light): is it switched on? */
        char name[16];
        int n = 0;
        *quiet = true;
        if (sscanf(val, "%15[a-z]:%n", name, &n) == 1 && n && !strcmp(val + n, "on")) ok = sim_board_sensor_on(name);
        else if (n && !strcmp(val + n, "off")) ok = !sim_board_sensor_on(name);
        else { ok = false; *quiet = false; }
    } else if (!strcmp(key, "ppm")) {
        /* The LCD as it is now, to a file (as --ppm writes it at the end). */
        ok = *val && screen_to_file(val);
    } else if (!strcmp(key, "quit")) {
        /* Power off, as on SIGTERM (Windows has no signal for it): a clean
           exit that writes --ppm. */
        ok = !strcmp(val, "1");
        if (ok) s_quit = 1;
    } else {
        ok = sim_set(key, val);
    }
    *eq = '=';
    return ok;
}

static void control_poll_locked(void)
{
    for (;;) {
        if (*s_ctl_advance) {
            if (sim_time_advancing()) return;          /* later lines wait for it */
            /* A boot that the new time brings (the RTC's alarm, the deep
               sleep's timer) is part of it: tick() boots, and boot_again
               answers. Answered here, a question right behind it would
               still find the board off. */
            if (sim_board_wake_due() || sim_deep_sleep_wake_due()) { sim_time_release(); return; }
            /* Cleared before it's answered: a crash in between (boot_again)
               must not answer it a second time. */
            char done[sizeof s_ctl_advance];
            snprintf(done, sizeof done, "%s", s_ctl_advance);
            *s_ctl_advance = 0;
            control_ack("ok", done);
        }
        char *nl = memchr(s_ctl_buf, '\n', s_ctl_len);
        if (nl) {
            *nl = 0;
            char *line = s_ctl_buf;
            if (nl > line && nl[-1] == '\r') nl[-1] = 0;
            bool quiet;
            /* Lines behind it ("touch=X,Y advance=150 touch=- advance=250"):
               the clock waits for them, at once or at the end of an advance,
               so a press lasts what was asked, however long this thread takes
               to come to them on a busy machine (answering a line can take a
               while on Windows: the clock is held before that). */
            bool more = memchr(nl + 1, '\n', s_ctl_len - (size_t)(nl + 1 - s_ctl_buf)) != NULL;
            if (!*line) { /* empty line */ }
            else if (!control_apply(line, &quiet)) {
                if (!quiet) fprintf(stderr, "control: bad line %s\n", line);
                if (more) sim_time_hold_after();
                control_ack("error", line);
            } else if (!strncmp(line, "advance=", 8)) {
                snprintf(s_ctl_advance, sizeof s_ctl_advance, "%s", line);   /* answered once it's run through */
                if (more) sim_time_hold_after();
            } else {
                if (more) sim_time_hold_after();
                control_ack("ok", line);
            }
            s_ctl_len -= (size_t)(nl + 1 - s_ctl_buf);
            memmove(s_ctl_buf, nl + 1, s_ctl_len);
            continue;
        }
        if (s_ctl_len == sizeof s_ctl_buf) s_ctl_len = 0;   /* overlong line: drop */
        int n = host_ctl_read(s_boot.ctl, s_ctl_buf + s_ctl_len, sizeof s_ctl_buf - s_ctl_len);
        if (n <= 0) { sim_time_release(); return; }      /* all applied: the clock goes on */
        s_ctl_len += (size_t)n;
    }
}

static void control_poll(void)
{
    if (!s_boot.ctl) return;
    ctl_lock();
    control_poll_locked();
    ctl_unlock();
}

/* Headless: sleep until a control line arrives, at most max_ms (1 ms while
   an advance is being run through, to answer it soon after it's done). */
static void control_wait(int max_ms)
{
    if (*s_ctl_advance) max_ms = 1;
    if (!s_boot.ctl || *s_ctl_advance) { host_sleep_ms(max_ms); return; }
    host_ctl_wait(s_boot.ctl, max_ms);
}

/* --------------------------------------------------------------- reboot */

static int64_t uptime_ms(void) { return esp_timer_get_time() / 1000; }

/* Boot again from `image` (this one, or a new build to flash: power_on). An
   advance being run through is over: it's answered; lines read but waiting
   behind it are answered as errors, so no reader waits for them. Takes the
   answers' lock for good, so none is half written (unless this thread holds
   it already: a crash while answering). */
static HOST_NORETURN void boot_again(const char *image, esp_reset_reason_t why, bool power_on)
{
    if (!ctl_mine()) ctl_lock();
    if (*s_ctl_advance) control_ack("ok", s_ctl_advance);
    for (char *line = s_ctl_buf, *nl; (nl = memchr(line, '\n', (size_t)(s_ctl_buf + s_ctl_len - line))); line = nl + 1) {
        *nl = 0;
        if (nl > line && nl[-1] == '\r') nl[-1] = 0;
        if (*line) control_ack("error", line);
    }
    boot_t b = s_boot;
    b.slot = -1;
    b.power = power_on;
    b.reset = why;
    b.world = sim_world_us();
    b.have_world = true;
    b.ran = s_boot.ran + uptime_ms();
    char keep[512];
    sim_board_state(keep, sizeof keep);
    become(image, &b, keep);
}

/* esp_restart(), or a simulated crash (ESP_RST_PANIC). */
static void restart(esp_reset_reason_t why)
{
    fprintf(stderr, "restart (%s)\n", why == ESP_RST_SW ? "software reset" : "crash");
    if (why == ESP_RST_PANIC && uptime_ms() < 1000) host_sleep_ms(300);   /* a crash loop doesn't spin the computer */
    boot_again(s_self, why, false);
}

/* The crash record (crash.h). The chip keeps it in RTC memory through the
   reset; here a file next to the NVS file carries it to the next boot. */
static char s_crash_file[620];

static void crash_keep(int code)
{
    crash_record_t *r = &crash_rec;
    const char *name = code == SIGSEGV ? "SIGSEGV, a bad memory access" : code == SIGILL ? "SIGILL"
                     : code == SIGFPE ? "SIGFPE" : code == SIGABRT ? "abort()" : NULL;
#ifdef SIGBUS
    if (code == SIGBUS) name = "SIGBUS, a bad memory access";
#endif
    if (name) snprintf(r->cause, sizeof r->cause, "%s", name);
    else snprintf(r->cause, sizeof r->cause, "%s %#x", (unsigned)code < 64 ? "signal" : "exception", (unsigned)code);
    snprintf(r->version, sizeof r->version, "%s", fw_version());
    r->frames = (uint32_t)host_crash_backtrace(&r->frame[0][0], CRASH_FRAMES, CRASH_FRAME_LEN);
    crash_seal(r);
    FILE *f = fopen(s_crash_file, "wb");
    if (f) {
        fwrite(r, sizeof *r, 1, f);
        fclose(f);
    }
}

/* At a boot: what the last one left (read once, then gone). */
static void crash_restore(void)
{
    FILE *f = fopen(s_crash_file, "rb");
    if (!f) return;
    crash_record_t r;
    if (fread(&r, sizeof r, 1, f) == 1) crash_rec = r;
    fclose(f);
    remove(s_crash_file);
}

/* A real crash in the firmware: a panic reset, as on the chip. */
static void on_crash(int code)
{
    fprintf(stderr, "restart (crash: %s %#x)\n", (unsigned)code < 64 ? "signal" : "exception", (unsigned)code);
    crash_keep(code);
    if (uptime_ms() < 1000) host_sleep_ms(300);       /* a crash loop doesn't spin the computer */
    boot_again(s_self, ESP_RST_PANIC, false);
}

static void on_quit(void) { s_quit = 1; }

/* ---------------------------------------------------------------- watch */

/* --watch: a new build of the executable started that has stopped changing
   is flashed and booted, as `luaboard flash` would. */
#define WATCH_POLL_MS   200
#define WATCH_STABLE_MS 300

static int64_t now_ms(void) { return host_mono_us() / 1000; }

static char s_run_mark[600];

static void run_mark_path(void) { snprintf(s_run_mark, sizeof s_run_mark, "%s.run", s_o.nvs); }

static void watch_poll(void)
{
    static char new_id[128], bad_id[128];
    static int64_t new_since, last, touched;
    int64_t now = now_ms();
    if (!s_o.watch || now - last < WATCH_POLL_MS) return;
    last = now;
    /* The run mark's time says the simulator is alive: the rebuild watcher
       (watch_build.cmake) ends when it goes stale, as after a kill -9. */
    if (now - touched >= 5000) {
        FILE *f = fopen(s_run_mark, "w");
        if (f) fclose(f);
        touched = now;
    }
    char id[128];
    sim_ota_file_id(s_origin, id, sizeof id);
    if (!*id || !strcmp(id, bad_id) || sim_ota_is_flashed(s_origin)) { *new_id = 0; return; }
    if (strcmp(id, new_id) != 0) {                    /* still being written? */
        snprintf(new_id, sizeof new_id, "%s", id);
        new_since = now;
        return;
    }
    if (now - new_since < WATCH_STABLE_MS) return;
    char ver[48];
    if (!sim_ota_image(s_origin, ver, sizeof ver)) {
        fprintf(stderr, "watch: %s changed but isn't a simulator image for this board, not flashed\n", s_origin);
        snprintf(bad_id, sizeof bad_id, "%s", id);
        return;
    }
    fprintf(stderr, "watch: new build %s, flashing it\n", ver);
    boot_again(s_origin, ESP_RST_POWERON, true);
}

/* ------------------------------------------------------------ window loop */

static int write_ppm(const char *path)
{
    static uint8_t rgb[BOARD_LCD_W * BOARD_LCD_H * 3];
    int w, h;
    if (!sim_board_screen_rgb(rgb, &w, &h)) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 3, (size_t)w * h, f);
    return fclose(f);
}

static void tick(void)
{
    control_poll();
    if (sim_board_wake_due()) {                     /* powered down, and something turns it on */
        fprintf(stderr, "power on\n");
        boot_again(s_self, ESP_RST_POWERON, false);
    }
    if (sim_deep_sleep_wake_due()) {                /* in deep sleep, and its timer is up */
        fprintf(stderr, "wake from deep sleep\n");
        boot_again(s_self, ESP_RST_DEEPSLEEP, false);
    }
    watch_poll();
}

/* --run-ms counts all the boots, so a crash loop ends too. */
static int window_should_quit(void) { return s_quit || (s_o.run_ms > 0 && s_boot.ran + uptime_ms() >= s_o.run_ms); }

static void *firmware_thread(void *arg)
{
    (void)arg;
    /* Room to handle a crash even when the firmware's stack overflowed: it
       still gets to reset the chip (on_crash). */
    host_thread_crash_guard();
    app_main(); /* never returns */
    return NULL;
}

/* ------------------------------------------------------------- power-on */

/* What only the executable started by hand does: the serial port, the
   control channel, the run mark; they last until the simulator is closed. */
static bool power_on(void)
{
    s_boot = (boot_t){ .slot = -1, .power = 1 };
    char where[700];
    s_boot.link = host_link_create(s_o.link, where, sizeof where);
    if (!s_boot.link) return false;
    if (s_o.control && !(s_boot.ctl = host_ctl_create(s_o.control))) return false;
    /* "<nvs>.run" exists while the simulator runs: found at start, the last
       run was killed (kill -9 stands in for a panic or watchdog reset). */
    FILE *mark = fopen(s_run_mark, "r");
    s_boot.reset = mark ? ESP_RST_PANIC : ESP_RST_POWERON;
    if (mark) fclose(mark);
    mark = fopen(s_run_mark, "w");
    if (mark) fclose(mark);
    char flash_dir[600];
    if (s_o.flash) snprintf(flash_dir, sizeof flash_dir, "%s", s_o.flash);
    else snprintf(flash_dir, sizeof flash_dir, "%s.flash", s_o.nvs);
    printf("Simulator    board=%s\n", BOARD_NAME);
    printf("Serial port: %s\n", where);
    printf("NVS file:    %s\n", s_o.nvs);
    printf("Apps flash:  %s/\n", flash_dir);
    return true;
}

/* Apply "KEY=VALUE KEY=VALUE ..." (sim_board_state). */
static void apply_keep(const char *keep)
{
    char buf[512];
    snprintf(buf, sizeof buf, "%s", keep);
    for (char *save, *kv = strtok_r(buf, " ", &save); kv; kv = strtok_r(NULL, " ", &save)) {
        char *eq = strchr(kv, '=');
        if (!eq) continue;
        *eq = 0;
        if (!sim_board_set(kv, eq + 1)) fprintf(stderr, "warning: board input %s not known to this firmware\n", kv);
    }
}

/* ------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    int bad = parse_args(argc, argv, &s_o);
    if (bad) return bad < 0 ? 0 : bad;
    host_init();
    host_self_path(s_self, sizeof s_self);
    snprintf(s_origin, sizeof s_origin, "%s", s_o.origin ? s_o.origin : s_self);
    run_mark_path();

    if (!s_o.boot) {
        if (s_o.watch) host_unlock_self();   /* a rebuild may replace it while it runs */
        if (!power_on()) return 1;
        if (s_o.wav) {                       /* a new recording; the boots add to it */
            FILE *f = fopen(s_o.wav, "wb");
            if (f) fclose(f);
        }
    } else if (!boot_parse(s_o.boot, &s_boot)) {
        fprintf(stderr, "--boot: bad handover %s\n", s_o.boot);
        return 2;
    }

    /* --- the bootloader: become the slot's image -------------------------- */
    if (s_boot.slot < 0) {
        char image[PATH_MAX];
        int slot = 0;
        if (sim_ota_boot(s_o.nvs, s_boot.power, &slot, image, sizeof image)) {
            boot_t b = s_boot;
            b.slot = slot;
            b.power = 0;
            become(image, &b, s_o.keep);
        }
        fprintf(stderr, "bootloader: no app slot holds an image, running %s\n", s_self);
        s_boot.slot = 0;
    }

    /* --- the chip ---------------------------------------------------------- */
    sim_time_init();
    /* SIM_BOOT_ADVANCE=MS in the environment: every boot begins with that
       much fast-forward, so the firmware's hardware delays (260 ms) take no
       real time. The tests set it: they boot a few hundred times, and a
       reboot inside a test (a crash, a wake-up, an update) is a boot too. */
    const char *boot_ff = getenv("SIM_BOOT_ADVANCE");
    if (boot_ff && atoi(boot_ff) > 0) sim_time_advance((uint32_t)atoi(boot_ff));
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    sim_ota_chip(s_o.nvs, s_boot.slot);
#if BOARD_LINK_USB_SERIAL_JTAG
    sim_usb_serial_attach_link(s_boot.link);
#else
    sim_uart_attach_link(BOARD_UART_PORT, s_boot.link);
#endif
    sim_time_wake_on_input(s_boot.link);  /* a command from the PC doesn't wait for the next tick */
    sim_nvs_set_path(s_o.nvs);
    snprintf(s_crash_file, sizeof s_crash_file, "%s.crash", s_o.nvs);
    if (s_boot.reset == ESP_RST_PANIC) crash_restore();
    else remove(s_crash_file);                    /* a boot that wasn't a crash's: nothing to tell */
    sim_set_reset_reason(s_boot.reset);
    char flash_dir[600];
    if (s_o.flash) snprintf(flash_dir, sizeof flash_dir, "%s", s_o.flash);
    else snprintf(flash_dir, sizeof flash_dir, "%s.flash", s_o.nvs);
    sim_flash_set_dir(flash_dir);
    sim_net_loopback(s_o.loopback);
    sim_net_refuse_port(host_ctl_port(s_boot.ctl));
    sim_board_set_state_path(s_o.nvs);
    /* The speaker: in the window through this computer's; each boot adds
       to the recording that power-on started. */
    sim_audio_open(!s_o.headless && !s_o.mute, s_o.wav, true);
    sim_board_init();
    /* Real Bluetooth devices nearby, through the computer's own Bluetooth
       (macOS only so far): in the window by default; tests (headless) stick
       to the simulated ones. */
    sim_ble_use_host(s_o.ble ? !strcmp(s_o.ble, "host") : !s_o.headless);
    for (int i = 0; i < s_o.nble_devs; i++) if (!ble_device_load(s_o.ble_devs[i])) return 2;
    /* The --set inputs at power-on. After a reset the board's inputs are as
       they were (--keep): no finger at boot, no RTC that lost power again.
       What the simulator was told about the world outside (where releases
       and the marketplace are, how often to look) holds on every boot, as
       the Bluetooth devices do; the clock goes on through a reset, and the
       version is the one the reset booted. */
    for (int i = 0; i < s_o.nsets; i++) {
        char key[64];
        const char *eq = strchr(s_o.sets[i], '=');
        if (!eq || (size_t)(eq - s_o.sets[i]) >= sizeof key) { usage(argv[0]); return 2; }
        snprintf(key, sizeof key, "%.*s", (int)(eq - s_o.sets[i]), s_o.sets[i]);
        bool every_boot = !strncmp(key, "ble", 3) || !strcmp(key, "update_url") || !strcmp(key, "apps_url") ||
                          !strcmp(key, "update_every") || !strcmp(key, "wifi_net");
        if (s_o.keep && !every_boot) continue;
        if (!sim_set(key, eq + 1)) {
            fprintf(stderr, "unknown or invalid --set %s (board keys: %s %s)\n", s_o.sets[i], sim_board_set_help(),
                    ble_device_set_help());
            return 2;
        }
    }
    if (s_o.keep) apply_keep(s_o.keep);
    if (s_boot.have_world) sim_world_set(s_boot.world);   /* the world went on through the reboot */
    sim_set_restart_handler(restart);
    sim_set_delay_hook(ppm_at_delay);
    host_on_crash(on_crash);
    fflush(stdout);

    pthread_t fw;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    /* The device's main task stack (CONFIG_ESP_MAIN_TASK_STACK_SIZE, from
       its sdkconfig.defaults by the build), rounded up to whole 16 KiB pages
       as macOS on Apple silicon requires. */
    size_t page = 16384;
    size_t stack = (CONFIG_ESP_MAIN_TASK_STACK_SIZE + page - 1) / page * page;
#ifdef PTHREAD_STACK_MIN
    /* (the smallest stack a thread may have is bigger than that on some
       systems: 128 KiB on Linux on arm64) */
    if (stack < (size_t)PTHREAD_STACK_MIN) stack = ((size_t)PTHREAD_STACK_MIN + page - 1) / page * page;
#endif
    int err = pthread_attr_setstacksize(&attr, stack);
    if (!err) err = pthread_create(&fw, &attr, firmware_thread, NULL);
    if (err) {
        fprintf(stderr, "cannot start the firmware thread (%zu byte stack): %s\n", stack, strerror(err));
        return 1;
    }

    host_on_quit(on_quit);

    int rc = 0;
    if (s_o.headless) {
        while (!window_should_quit()) {
            control_wait(10);
            tick();
        }
    } else {
        /* "CoreS3 SE simulator". With several open, the others say which
           they are in the title (what the Dock and the window switcher
           show): "CoreS3 SE simulator - esp32-sim-2". */
        char title[128], view[620];
        const char *link = host_link_name(s_boot.link), *base = link ? link + strlen(link) : NULL;
        while (base && base > link && base[-1] != '/' && base[-1] != '\\') base--;
        if (base && strcmp(base, "esp32-sim"))
            snprintf(title, sizeof title, "%s simulator - %s", sim_board_model_name(), base);
        else
            snprintf(title, sizeof title, "%s simulator", sim_board_model_name());
        snprintf(view, sizeof view, "%s.view", s_o.nvs);   /* window size and scaling, kept */
        rc = sim_window_run(title, host_link_port(s_boot.link), s_o.scale, view, window_should_quit, tick, s_o.shot);
    }

    ctl_lock();                      /* no reboot from here on */
    if (s_o.ppm && write_ppm(s_o.ppm) != 0) fprintf(stderr, "could not write %s\n", s_o.ppm);
    host_link_remove(s_boot.link);
    remove(s_run_mark);              /* a clean power-off */
    if (s_o.control) host_ctl_remove(s_boot.ctl, s_o.control);
    fflush(stdout);
    fflush(stderr);
    _exit(rc); /* firmware thread runs forever; don't wait for it */
}
