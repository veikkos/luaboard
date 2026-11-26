/*
 * ESP32 mock platform - "outside world" wiring API.
 *
 * The ESP-IDF-like headers are what the *firmware* sees. This header is what
 * the *simulator* (the board/test bench around the chip) uses to connect the
 * mocked peripherals to something: the host's serial link (host.h), device models
 * on the I2C and SPI buses, voltages on ADC pins, levels on GPIO inputs.
 *
 * Firmware code must never include this header.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_system.h"
#include "host.h"
#include "host/ble_hs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Reset ------------------------------------------------------------- */
/** What esp_reset_reason() reports to the firmware (default: power-on). */
void sim_set_reset_reason(esp_reset_reason_t r);
/** What esp_restart() does: the simulator starts itself over (it must not
 *  return). `reason` is what the next boot's esp_reset_reason() reports:
 *  ESP_RST_SW, or ESP_RST_PANIC for a simulated crash. */
void sim_set_restart_handler(void (*restart)(esp_reset_reason_t reason));
/** `fn` is called by the firmware's main task each time it starts a delay:
 *  between two passes of its loop, when nothing is half done (the screen
 *  isn't in the middle of a flush). */
void sim_set_delay_hook(void (*fn)(void));

/* ---- App slots (OTA) and the bootloader -------------------------------- */
/** Run the bootloader, run by this executable (an app image) at a boot:
 *  picks the app slot to boot (rollback as on the device) and gives its
 *  image's path, the executable the simulator then becomes. The slots and
 *  otadata live in "<base>.ota/" (base: the NVS file). `power_on`: this
 *  executable was started by hand (or rebuilt, --watch); if it isn't the one
 *  last flashed, it counts as freshly flashed (like `luaboard flash`): it
 *  becomes slot ota_0 and boots. False if no slot holds a usable image. */
bool sim_ota_boot(const char *base, bool power_on, int *slot, char *image, size_t cap);
/** Is `path` an app image for this board? Its version. */
bool sim_ota_image(const char *path, char *version, size_t cap);
/** Is `path` the executable last flashed into ota_0 (an unchanged build)? */
bool sim_ota_is_flashed(const char *path);
/** Which file is at `path` (device, inode, size, time): a new build gets a new id. */
void sim_ota_file_id(const char *path, char *out, size_t cap);
/** The image of `slot` runs (what sim_ota_boot picked). */
void sim_ota_chip(const char *base, int slot);
/** The running image's version as the firmware sees it
 *  (esp_app_get_description), instead of the build's: --set fw_version=. */
void sim_ota_set_version(const char *ver);

/* ---- Update check ------------------------------------------------------ */
/** Where the release manifest is (--set update_url=); "" or NULL: none. */
void sim_update_set_url(const char *url);
/** NULL: none set, the firmware doesn't check. */
const char *sim_update_url(void);
/** The time between checks in ms (--set update_every=, for tests: a day
 *  of fast-forward is slow); 0 (default): the firmware's own. */
void sim_update_set_every(uint32_t ms);
uint32_t sim_update_every(void);
/** Where the app marketplace is (--set apps_url=, its index at
 *  <url>/main/index.json); "" or NULL: none, no app checks. update_every
 *  sets its interval too. */
void sim_apps_set_url(const char *url);
const char *sim_apps_url(void);

/* ---- Serial links ------------------------------------------------------ */
/** Route a UART's TX/RX to the host's serial link (host.h). */
void sim_uart_attach_link(uart_port_t port, host_link_t *link);
/** Wire the port's TX to its RX (a jumper on its pins): what the firmware
 *  writes, it reads back. Off: nothing is attached. */
void sim_uart_loopback(uart_port_t port, bool on);
/** Route the ESP32-S3 USB-Serial/JTAG CDC port to the host's serial link. */
void sim_usb_serial_attach_link(host_link_t *link);
/** The computer stops talking to the port (mock_usb_serial_jtag.c), until
 *  the firmware makes it start anew. */
void sim_usb_serial_host_gone(bool gone);
bool sim_usb_serial_host_talking(void);

/* ---- I2C --------------------------------------------------------------- */
/** Device model: one write transaction (START addr+W data... STOP). */
typedef void (*sim_i2c_write_fn)(void *ctx, const uint8_t *data, size_t len);
/** Device model: read phase (addr+R), fill buf with len bytes. */
typedef void (*sim_i2c_read_fn)(void *ctx, uint8_t *buf, size_t len);
/** Attach a device model at a 7-bit address. Unattached addresses NACK.
 *  on_read may be NULL for write-only devices. */
void sim_i2c_attach(uint8_t addr7, sim_i2c_write_fn on_write, sim_i2c_read_fn on_read, void *ctx);
/** The same on another bus (I2C port 1: an external connector). */
void sim_i2c_attach_port(int port, uint8_t addr7, sim_i2c_write_fn on_write, sim_i2c_read_fn on_read, void *ctx);
/** Unplug the model at addr7 on that port. */
void sim_i2c_detach_port(int port, uint8_t addr7);
/** Something answers at that address. */
bool sim_i2c_present(int port, uint8_t addr7);

/* ---- Camera (esp_camera.h) ------------------------------------------------
 * The board's camera: its chip's I2C address, and what it sees: w x h pixels
 * of RGB565, the high byte first. Pictures come while a model is attached
 * at that address. */
typedef void (*sim_camera_fn)(void *ctx, uint8_t *be565, int w, int h);
void sim_camera_attach(uint8_t addr7, sim_camera_fn picture, void *ctx);
/** The firmware has the camera taking pictures. */
bool sim_camera_running(void);

/* ---- SPI --------------------------------------------------------------- */
/** Device model: bytes clocked out while the device's CS is asserted. */
typedef void (*sim_spi_write_fn)(void *ctx, const uint8_t *data, size_t len);
/** Attach a device model selected by chip-select pin cs. */
void sim_spi_attach(int cs_pin, sim_spi_write_fn on_write, void *ctx);

/* ---- I2S --------------------------------------------------------------- */
/** What an I2S port's data line is wired to (a speaker amplifier): called
 *  with the frames as they play, 16-bit with `channels` interleaved, at
 *  `rate` frames a second. Called on the firmware thread that wrote them. */
typedef void (*sim_i2s_sink_fn)(void *ctx, const int16_t *frames, size_t n, int channels, uint32_t rate);
void sim_i2s_set_sink(int port, sim_i2s_sink_fn fn, void *ctx);

/* ---- ADC --------------------------------------------------------------- */
/** Set the raw 12-bit value (0..4095) the ADC will read on a channel. */
void sim_adc_set_raw(adc_unit_t unit, adc_channel_t ch, int raw);
int  sim_adc_get_raw(adc_unit_t unit, adc_channel_t ch);

/* ---- GPIO -------------------------------------------------------------- */
/** Externally drive an input pin: 0/1, or -1 to release (floating -> pulls). */
void sim_gpio_drive(gpio_num_t pin, int level);
/** Output latch level as seen from outside (e.g. LED). -1 if not an output. */
int  sim_gpio_output_level(gpio_num_t pin);
/** What the chip does to the pin: 'o' drives it, 'u' pulls it up, 'd' pulls
 *  it down, 'f' leaves it floating (what's wired to it decides). */
char sim_gpio_drive_kind(gpio_num_t pin);
/** Is the pin exactly as gpio_reset_pin leaves it (no output, no input, the
 *  pull-up alone)? */
bool sim_gpio_idle(gpio_num_t pin);

/* ---- Wi-Fi / network -------------------------------------------------- */
/** Whether the simulated access point is in range. Dropping it disconnects. */
void sim_wifi_set_ap(int available);
/** The network the simulated board joins while none is set up on it
 *  (--set wifi_net=, which the run target passes; "" or "none": none, the
 *  board waits for `luaboard wifi set` as a real one does). */
void sim_wifi_set_net(const char *ssid);
const char *sim_wifi_net(void);
/** The signal it comes in with, in dBm (default -52): what
 *  esp_wifi_sta_get_ap_info reports while connected. */
void sim_wifi_set_rssi(int dbm);
int  sim_wifi_connected(void);
/** Bind requests for firmware port `fw_port` use `host_port` instead
 *  (e.g. to run several simulators side by side). Also used for mDNS.
 *  host_port 0: any free port (the first bind picks one, later ones reuse it). */
void sim_net_map_port(uint16_t fw_port, uint16_t host_port);
/** Servers bound to "any address" take this computer's loopback only (127.0.0.1):
 *  nothing outside reaches them, and no firewall asks about them (tests). */
void sim_net_loopback(bool on);
/** The host port for firmware port `fw_port`: the one bound, if mapped to 0. */
uint16_t sim_net_host_port(uint16_t fw_port);
/** The firmware's connect() to this port on this computer is refused: the
 *  simulator's control channel isn't for apps. 0: none. */
void sim_net_refuse_port(uint16_t host_port);

/* ---- Bluetooth LE ------------------------------------------------------ */
/* Simulated peripherals the firmware can scan for and connect to (NimBLE in
   the mock). */
#define SIM_BLE_ENCRYPTED 0x01   /* chr flag: reads/writes need a paired link */
typedef struct {
    ble_uuid_any_t svc;          /* service UUID */
    ble_uuid_any_t uuid;         /* characteristic UUID */
    uint8_t props;               /* BLE_GATT_CHR_PROP_* bits */
    uint8_t flags;               /* SIM_BLE_* */
} sim_ble_chr_t;
/** Read of characteristic `chr` (index into chrs): fill buf, return the
    length, or -ATT error code to refuse. */
typedef int (*sim_ble_read_fn)(void *ctx, int chr, uint8_t *buf, size_t cap);
/** Write to `chr`: return 0, or an ATT error code to refuse. */
typedef int (*sim_ble_write_fn)(void *ctx, int chr, const uint8_t *data, size_t len);
typedef struct {
    uint8_t addr[6];             /* as written: addr[0] is the first byte of "c7:18:..." */
    bool random;                 /* random (static) address, else public */
    const uint8_t *adv;          /* advertising data (AD structures, at most 31 bytes) */
    size_t adv_len;
    const sim_ble_chr_t *chrs;   /* grouped by service */
    int nchrs;
    sim_ble_read_fn on_read;     /* NULL: reads fail */
    sim_ble_write_fn on_write;   /* NULL: writes fail */
    /** A central turned notifications of `chr` on or off (may be NULL). */
    void (*on_subscribe)(void *ctx, int chr, bool on);
    void *ctx;
} sim_ble_device_t;
/** Add a peripheral (not yet in range). Returns its id, or -1 if full. The
    struct and what it points to must stay valid. */
int  sim_ble_add(const sim_ble_device_t *dev);
/** In range (advertising, connectable) or not. Going away drops its links
    after the supervision timeout. */
void sim_ble_set_present(int id, bool present);
/** Signal strength its advertisements arrive with (default -60 dBm). */
void sim_ble_set_rssi(int id, int rssi);
/** The value of `chr` changed: notify (or indicate) to subscribed centrals. */
void sim_ble_notify(int id, int chr, const uint8_t *data, size_t len);
/** Also use the computer's own Bluetooth (macOS CoreBluetooth): real devices
    nearby show up next to the simulated ones. Takes effect when the
    firmware starts Bluetooth (macOS asks for permission then, once). */
void sim_ble_use_host(bool on);

/* ---- microSD card ---------------------------------------------------------- */
/** What's in the card slot: a host folder (a FAT card with those files; made
 *  if it isn't there), "unformatted" (a card without a FAT file system) or
 *  "none". Changing it while the firmware has a card mounted is pulling
 *  that card out. */
void sim_sd_set(const char *what);
void sim_sd_get(char *out, size_t cap);
/** The firmware has the card mounted. */
bool sim_sd_mounted(void);

/* ---- PWM ------------------------------------------------------------------- */
/** What the PWM unit puts out on `pin`: its frequency and the share of the
 *  time it's high (0..1). False if the pin isn't pulsing. */
bool sim_pwm_get(int pin, uint32_t *hz, double *duty);

/* ---- Power management ---------------------------------------------------- */
/** The firmware has light sleep configured and holds no lock against it
 *  (esp_pm.h): on a real chip it would sleep whenever every task waits. */
bool sim_light_sleep_allowed(void);

/** The board lost its power (the board model calls this when the PMIC is
 *  told to power down; deep sleep ends the same way): the firmware's main
 *  task stops at its next delay, and sim_time_advance() moves the clock at
 *  once. The simulator boots the board again when something turns it on. */
void sim_power_off(void);
bool sim_powered_off(void);
/** The chip went into deep sleep (esp_deep_sleep_start): powered down as
 *  above, until its timer ends it (`timer_us` from now; negative: no timer),
 *  which sim_deep_sleep_wake_due() says. The boot that follows is a
 *  deep-sleep reset. */
void sim_deep_sleep(int64_t timer_us);
bool sim_deep_sleep_wake_due(void);

/* ---- NVS --------------------------------------------------------------- */
/** Host file used as NVS backing store. Must be set before nvs_flash_init(). */
void sim_nvs_set_path(const char *path);
/** Host folder holding the LittleFS "apps" partition. Set before app_main(). */
void sim_flash_set_dir(const char *dir);

/* ---- Time -------------------------------------------------------------- */
/** Reset the boot time reference (called by the simulator at startup). */
void sim_time_init(void);
/** Fast-forward the chip's clock by `ms`: the firmware's delays return at
 *  once until it has run through that much simulated time, tick by tick.
 *  Calls add up. The host's wall clock (gettimeofday) is not affected. */
void sim_time_advance(uint32_t ms);
/** The chip's clock jumps `ms` ahead at once, whatever the firmware is
 *  doing: also in the middle of a call that never waits (a script's busy
 *  loop, whose watchdog then finds its second gone). A fast-forward is run
 *  through by the firmware's delays, and such a call has none. */
void sim_time_skip(uint32_t ms);
/** The world's time (what NTP and the RTC chip see), µs since 1970 UTC.
 *  It runs with the chip's clock, so `advance` moves it too. It starts at
 *  this computer's time unless set. */
int64_t sim_world_us(void);
/** Set the world's time now (--set time=EPOCH): tests run on a fixed date. */
void sim_world_set(int64_t utc_us);
/** The firmware's system clock (gettimeofday, see sim_wallclock.h) is the
 *  world's plus what settimeofday changed; an NTP sync puts it back. */
void sim_system_clock_sync(void);
/** The clock stops when the fast-forward under way ends (at once if none
 *  is), until sim_time_release() or the next sim_time_advance(): control
 *  lines sent together take effect at the device time they were meant for,
 *  not however much later the host gets to each. */
void sim_time_hold_after(void);
void sim_time_release(void);
/** True while a fast-forward is still being run through. */
bool sim_time_advancing(void);
/** Input on `link` (the PC's) ends the firmware's one-tick delay at once,
 *  the clock skipping the rest of the tick, instead of waiting it out. */
void sim_time_wake_on_input(host_link_t *link);

#ifdef __cplusplus
}
#endif
