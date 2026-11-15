# Architecture

```
 ┌──────────────────────── PC ─────────────────────────┐
 │  pc-tool/  (Rust)                                    │
 │   luaboard-studio  egui GUI: edit, upload, run, REPL │
 │   luaboard         CLI: put/run/eval/monitor ...     │
 │   device.rs        protocol client (Device)          │
 │   link.rs          serial port, sim pty/pipe; Wi-Fi  │
 │                    link in wifi.rs                   │
 │   project.rs       project folder, secrets file      │
 └──────────────┬───────────────────────────────────────┘
                │ text protocol (docs/PROTOCOL.md), over either link:
     USB:   real HW: USB-C ─ ESP32-S3 USB-Serial/JTAG   sim: pseudo-terminal (/tmp/esp32-sim)
     Wi-Fi: TCP :3333, mDNS luaboard.local / _luaboard._tcp  (sim: this Mac + Bonjour)
                │
 ┌──────────────▼──────────── firmware (app/) ──────────┐
 │ main.c         super-loop: polls every module, then  │
 │                script_tick and menu_tick;            │
 │                boot: menu or autostart               │
 │ menu.c         home menu, app options, Settings      │
 │                App updates, Get apps (with QR codes) │
 │ appmeta.c      app.json: title, description, ...     │
 │ buttons.c      touch strip → A/B/C, long-B; tap,     │
 │                hold, drag and swipe on the screen    │
 │ settings.c     the device's settings (NVS)           │
 │ sleep.c        screen off when idle or asked; wake;  │
 │                light sleep on battery                │
 │ power.c        turning off: sys.off, the setting,    │
 │ wake_time.c    the wake-up time, the boot reason     │
 │ clock.c        NTP / RTC chip at boot; POSIX tz      │
 │ protocol.c     commands, PUT/GET/FB, LOG/EVT output; │
 │                one session per link (USB, Wi-Fi)     │
 │ net.c          Wi-Fi join/retry, TCP server, mDNS    │
 │ conn.c         a TCP connection out in steps: name   │
 │                lookup (dns.c), connect, TLS (tls.c), │
 │                send and receive; for lua_net.c and   │
 │                httpc.c                               │
 │ httpc.c        HTTPS GET in small steps, follows     │
 │                redirects, streams the body           │
 │ ota.c          a firmware image into the other slot, │
 │                checked and booted; back to the old   │
 │                one if the new keeps crashing         │
 │ update.c       release check (manifest over httpc),  │
 │                the Update row's data, Install into   │
 │                the other slot through ota.c          │
 │ update_manifest.c  manifest parser, version compare  │
 │ app_update.c   marketplace index check (httpc, on    │
 │                the update check's schedule); app     │
 │                installs: changed files only, size +  │
 │                SHA-256 checked, vfs_stage_commit     │
 │ apps_index.c   index.json parser: the newest version │
 │                of each app this firmware runs        │
 │ check_sched.c  when the update and app checks run    │
 │ semver.c       app version compare (dev stamps too)  │
 │ json.c         the small JSON reader of appmeta.c,   │
 │                update_manifest.c and apps_index.c    │
 │ auth.c         Wi-Fi pairing key, session keys       │
 │ wifilink.c     encrypted Wi-Fi link (AES-256-GCM)    │
 │ secrets.c      per-app secrets in NVS                │
 │ appdata.c      an app's secrets, store and bonds;    │
 │                uninstall and format                  │
 │ browser.c      browser login requests to the PC tool │
 │ script.c       Lua VM: heap cap, watchdog, require,  │
 │                timers + loop() via a Lua prelude     │
 │ lua_api.c      disp / touch / adc / gpio / sys       │
 │ lua_net.c      net: non-blocking TCP sockets, TLS    │
 │ lua_web.c      secrets / crypto / browser for Lua    │
 │ lua_store.c    store: per-app values (/apps/.data)   │
 │ lua_i2c.c      i2c: the bus on Grove Port A          │
 │ lua_uart.c     uart: a serial port (Grove Port C)    │
 │ lua_pins.c     which of gpio, i2c, uart has a pin    │
 │ lua_sensors.c  imu, light: the board's own sensors   │
 │ lua_sd.c       sd: files on the microSD card (FAT),  │
 │ sdcard.c       mounted while a script uses it        │
 │ sound.c        speaker: queue of tones and WAV clips │
 │                played by a thread of its own         │
 │ lua_sound.c    sound: the speaker for Lua            │
 │ ble.c          Bluetooth LE central (NimBLE host):   │
 │                scan, connect, discovery, pairing     │
 │ lua_ble.c      ble: devices and connections for Lua  │
 │ builtin_lua.c  app/lua/*.lua (prelude, mqtt, http,   │
 │                json, oauth) embedded in the binary   │
 │ vfs.c          apps: LittleFS /apps/<app>/<file>     │
 │                + RAM uploads shadowing them;         │
 │                app data in /apps/.data/<app>         │
 │                staged installs: .stage/<app> swapped │
 │                in whole, finished or undone at boot  │
 │ gfx.c          RGB565 drawing, dirty-rect tracking   │
 │ text.c         UTF-8 text for the 5x7 font           │
 │ jpeg.c         disp.jpeg via TJpgDec                 │
 │ png.c          disp.png via miniz's inflate (tinfl;  │
 │                on the device the ROM's)              │
 │ lcd_ili9342c.c panel driver over SPI                 │
 │ hw.c           USB link, PMIC, IO expander, touch,   │
 │                RTC, ADC, backlight, speaker (I2S)    │
 │ screens.c      boot splash, error, notice and        │
 │                Restarted screens                     │
 │ crash.c        a crash's cause and stack trace, kept │
 │                for the next boot (RTC memory; the    │
 │                panic hook in app/idf/main fills it)  │
 └──────────────┬───────────────────────────────────────┘
                │ ESP-IDF API (driver/spi_master.h, driver/i2c_master.h,
                │ driver/usb_serial_jtag.h, driver/gpio.h, esp_adc/...,
                │ esp_heap_caps.h, nvs.h, esp_timer.h, freertos/task.h,
                │ esp_wifi.h, lwip/sockets.h, mdns.h, esp_littlefs.h,
                │ host/ble_hs.h (NimBLE), ...)
       ┌────────┴─────────┐
       ▼                  ▼
  real ESP-IDF      platform/esp32-mock/   (same headers, host implementation;
  (app/idf/)              │                 NimBLE answered from simulated devices
                          │                 and the Mac's CoreBluetooth)
                          │ sim_hw.h: wiring API for the "outside world"
                          ▼
                    platform/sim/
                     sim_main.c   PTY ⇄ USB serial, boots app_main() in a thread;
                                  at a reset runs the bootloader and becomes
                                  the slot's image (execv), PTY kept
                     sim_ui.c     SDL2 drawing helpers (LCD with pixel grid, sliders)
                     sim_window.c the window: the board scaled to fit it
                     sim_audio.c  the speaker through the computer's, --wav
                     models/      ILI9342C (SPI), FT6336U touch, AXP2101 PMIC,
                                  AW9523B IO expander, BM8563 RTC, AW88298
                                  speaker amplifier (fed by the I2S mock),
                                  BMI270 motion sensor, LTR-553 light
                                  sensor, SHT40 (the ENV unit) -
                                  register/bus-level models; ble_device.c,
                                  Bluetooth peripherals described in JSON
                     boards/<board>/sim_board.c
                                  wires the models like the real board, draws
                                  the device and its control panel
```

## Design decisions

The mock copies the ESP-IDF API instead of defining a custom HAL. The firmware
includes `driver/spi_master.h`, `driver/i2c_master.h` and so on, exactly as it would on the
chip. On the host, `platform/esp32-mock/include` provides those headers. On hardware,
ESP-IDF does. So `app/` has no `#ifdef SIM`, and the code that runs in the simulator
is the code that ships.

The hardware is simulated at the bus level. The firmware has real drivers:
it sends the ILI9342C's init sequence and pixel data over SPI, toggles the D/C
pin, reads touch registers over I2C, sets the backlight voltage on the PMIC and
pulses the LCD reset through the IO expander. The simulator's models interpret
those bytes the way the chips do. The LCD model even reproduces this panel's
quirks: it shows inverted colors unless the driver sends INVON, and its
sub-pixels are BGR. Driver bugs show up in the simulator too.

There are two sides to the platform. The firmware sees `esp32-mock/include/*.h`.
The test bench sees `sim_hw.h`: attach a file descriptor to the serial link, device
models to I2C addresses and SPI chip-selects, set ADC voltages, drive input pins.
Firmware never includes `sim_hw.h`.

One protocol, several links. The protocol keeps a session per link: its own
line buffer and upload state. A command's reply goes back on the link it came
from, while `LOG`/`EVT` lines are broadcast to every link. So a script's output
and errors reach a Wi-Fi listener even while someone works over USB. The Wi-Fi
link is a plain TCP server (lwIP BSD sockets) with room for one logged-in client
and two still logging in (who pushes out whom is in [PROTOCOL.md](PROTOCOL.md)).

USB is trusted, Wi-Fi needs pairing. Each link says whether it's trusted.
Pairing over USB makes a random 256-bit key that the PC tools keep. Each Wi-Fi
connection swaps fresh nonces in a clear-text HELLO, derives per-direction
keys from the pairing key and both nonces (HMAC-SHA256), and then carries the
usual byte stream in AES-256-GCM frames (`wifilink.c`, PSA crypto from mbedTLS
on the device and in the simulator, RustCrypto on the PC). A session on an
untrusted link takes no commands and gets no `LOG`/`EVT` until one of its
frames decrypts. There is nothing to guess, so there's no lockout either.

The firmware must stay reachable. Lua failures (errors, the watchdog,
out-of-memory) only stop the script; the super-loop and both links keep running.
For failures below Lua (panics, brown-outs, hardware watchdog resets), a boot
counter in NVS detects a crash loop: 3 boots in a row that end in a crash
reset (by the chip's reset reason) while the autostart app runs make the next
boot skip it. The count clears when the app stops, after it has run 10 minutes,
and at a normal power-on. Holding B on the touch strip,
or a short press of the power key, leaves any app from the device itself, so a script that ignores every input
never locks the user out of the menu. Wi-Fi event handlers only set flags;
reconnecting with back-off, the listener and mDNS all run in the main loop.

Boards are a directory each. `platform/boards/<board>/board.h` holds the pin map,
the screen size and the chip addresses. The firmware and the simulator both use it,
so they can't drift apart. `platform/sim/boards/<board>/` holds the simulated device.
`-DBOARD=<board>` (CMake) selects one; `m5stack-cores3` is the default and currently the
only board. The framebuffer, the Lua `disp.W`/`disp.H` and the preview all take their
size from the board.

The scripting model is cooperative. There's one FreeRTOS task with a super-loop.
Lua never blocks it for long:
* `loop(dt)` is called at a configurable rate. Timers live in a small Lua prelude.
* A count hook enforces a per-call watchdog (1 s) so `while true do end` can't
  lock out the serial link. Slow C calls (`sys.delay`, JPEG decoding, TLS
  steps) are credited the time they actually took, up to a hard 10 s per call.
  Below that, the main task is on the ESP-IDF task watchdog (30 s, panics),
  so a C call that never returns resets the chip and the boot counter takes over.
* A custom allocator caps the VM at 4 MiB, in PSRAM. Out-of-memory becomes a normal Lua error.
* Only `base/table/string/math/utf8/coroutine` are opened. `io/os/debug/package`
  aren't even compiled in, and `dofile/loadfile` are removed.

An app is a folder; installing is explicit. Files are `<app>/<file>`.
`PUT` writes to RAM, where an upload shadows the installed copy, and
`INSTALL <app>` writes it to flash. You can iterate quickly without wearing
flash, and a bad upload never replaces the installed version until you install
it. Installed apps live in a LittleFS partition (`apps`, ~9.9 MB, mounted at
`/apps`) and are read when they run, not kept in RAM. NVS keeps the small
things: settings, Wi-Fi, pairing key, secrets. NVS was not used for apps because it
caps an entry at ~500 KB and gets slow and memory-hungry when large. Each file
is written to a temporary name and renamed over the old one (atomic in
LittleFS), so a power cut leaves either the old or the new file. In the simulator the partition is a folder on the Mac (`--flash`), so
installed apps are plain files you can look at. Keeping a finger on the screen
during boot skips the autostart app (safe mode).

The home menu is firmware, not a script. `menu.c` draws with `gfx` and
owns the screen whenever no script does: at boot (unless an app autostarts),
after `STOP`, `sys.exit()`, a held B or the power key. It works even when every installed app
is broken. `buttons.c` is the one place touches are timed. It turns each
touch, from finger down to finger up, into events in one queue: press, long
and repeat for the buttons, tap, hold, drag and swipe on the screen. A long
press is never also a press, and a drag never a tap. The controller is read
every 10 ms from wherever the firmware spends time: the main loop, the Lua
count hook, `sys.delay` (in slices) and between a frame's Lua and its flush.
So a slow app frame doesn't merge or delay touches, without a task of its
own. Whoever has the screen reads the queue: the menu
(`menu_tick`) while no script runs, the script (`touch.on()` in the prelude)
while one does. Starting or stopping a script empties the queue and ignores a
finger that is down until it's lifted, so a touch never carries over. Holding
B and the power key don't go through the queue: `buttons_poll()` returns them
to `menu_tick`, which leaves the app or goes back. `touch.read()` and
`touch.button()` report the same reading the events come from. A finger
from the PC (`TOUCH` in `protocol.c`) is queued in `buttons.c` and taken one
reading at a time in place of the controller's, so remote touches are timed
and turned into events by the same code. The same readings drive screen
sleep (`sleep.c`): it turns the backlight off after the Sleep after setting's
time without a touch, or on a long press of the power key, and the touch or
key that wakes it is ignored until it's let go. `hw_backlight()` only
remembers the level while the screen is off, so an app or the menu setting
the brightness doesn't light it.

Asleep on battery, the chip is allowed its automatic light sleep
(`sleep_light_allowed()`: not on USB power, no sound playing, no update
running, Bluetooth idle). `hw_light_sleep_allow()` releases the lock that
holds it off, and the main loop then waits for the script's next tick, at
most 50 ms, where it otherwise waits one 10 ms tick: the chip only sleeps
while every task waits. Touch and the power key are still polled, on every
pass, so nothing depends on an interrupt line. `POWER_LIGHT_SLEEP` in
`app_config.h` turns it off altogether. A script's open `uart` holds it
off too: the chip would lose what arrives while it sleeps. So does a pin
pulsing with `gpio.pwm`, whose clock would stop.

Turning off is `power.c`'s. A request (an app's `sys.off`, or the Turn off
after setting once the device has been asleep that long on battery with no
app keeping it on) is acted on from the main loop, between Lua calls: the
script is stopped as `sys.exit()` stops it, the app's name is noted in NVS
so the next boot starts it again in place of the autostart app (once), and
`hw_power_off()` sets the PMIC's power-off bit. A wake-up time is the RTC
chip's job: `wake_time.c` rounds it down to what the chip's countdown
timer (1 to 255 seconds or minutes) or its alarm (on the minute) can do,
less a few seconds for the boot, so the device is on early and never late,
and the chip's interrupt turns the PMIC back on. If the power stays on
after all, or the RTC chip couldn't be armed (with the power cut nothing
would wake the board then), the chip goes into deep sleep with a touch and
its own timer as wake sources, so a device can't get stuck. A firmware
update in progress holds all of this off until it's over. At boot `power_init()` reads
why the device started (`sys.info().boot`) from the reset reason and the
RTC chip's flags.

On USB power the board isn't powered down: nothing would be saved, and the
PC link would drop. `power.c` acts the off out instead: the script is
stopped, the screen is off, the main loop runs neither a script nor the
menu, and at the power key or the wake-up time the same script is started
again in a fresh VM. Code that was only uploaded survives that, which it
wouldn't a real power-off.

The microSD card is on the display's SPI bus, and its data-out line is the
pin the display uses as D/C (G35). `sdcard.c` hands the pin over around
every use of the card: an input while the card answers (`sdcard_begin()`),
the display's output again after (`sdcard_end()`). All card access is
inside `sd` calls on the main task, so nothing draws in between, and
`lua_sd.c` raises no Lua error between the two (results are built for Lua
after the pin is back). The card is mounted (FAT, by ESP-IDF's FATFS, never
formatted) by a script's first `sd` call and unmounted when the script
ends; with no card mounted its select line is held high, as at boot.

`menu.c` draws every list (apps, settings, app options, the autostart
picker) through one `list_t`: rows, scrolling and scroll marks, with a
function per view for a row's text. A row under a finger shows pressed:
after the events, `menu_tick` repaints just the rows that changed.

The protocol is text, with a binary upload. It's debuggable with `screen`,
and robust against ROM/boot chatter because untagged lines are ignored. Uploads
use a length prefix plus CRC-32, so no escaping is needed.

## Simulator vs. real hardware

| Aspect | Simulator | CoreS3 / Lite / SE |
|---|---|---|
| Serial link | PTY (`/tmp/esp32-sim` from the `run` target; Windows: named pipe `\\.\pipe\esp32-sim`) | USB-Serial/JTAG, `/dev/cu.usbmodem*` |
| Timing | Host clock, `vTaskDelay` = `nanosleep` | FreeRTOS 100 Hz tick |
| LCD | ILI9342C model, instant SPI | 40 MHz SPI, ~30 ms per full frame |
| Touch | Mouse on the screen / A-B-C keys | FT6336U |
| ADC | Port B analog slider in the settings, ±3 LSB noise | Real SAR ADC (nonlinear, calibrate with `adc_cali`) |
| Battery | Level and USB power in the settings | AXP2101 fuel gauge |
| Memory | Host malloc | Framebuffer and Lua heap in 8 MB PSRAM |
| NVS | Host file (`--nvs`) | Flash partition (`app/idf/partitions.csv`) |
| Apps partition | Host folder (`--flash`), size reported as the device's | LittleFS on flash (`joltwallet/littlefs`) |
| Firmware update | App image = a simulator executable; slots are files in `<nvs>.ota/`; at each boot the simulator runs the bootloader and replaces itself with the chosen one | ESP image in `ota_0`/`ota_1`, ESP-IDF bootloader with rollback |
| Touch buttons | Click the `‹‹ ○ ››` marks, A/B/C keys, or `luaboard sim-ctl button=B` (`--control`) | FT6336U strip under the LCD |
| Logs | stderr of the simulator | UART0 (not on USB) |
| Wi-Fi | Simulated AP (panel toggle), joins in ~0.3 s, IP 127.0.0.1; TCP on this Mac, Bonjour | 2.4 GHz station, DHCP, espressif/mdns |

The `pc-tool` opens real ports through the `serialport` crate. For the simulator's
PTY it falls back to raw termios, because macOS rejects the baud-rate ioctl on PTYs.

Bluetooth goes through the NimBLE host API (`host/ble_hs.h`), the stack
ESP-IDF uses, so `ble.c` is the same code in both places. In the simulator the
mock plays the host stack: it queues every callback to a host thread like the
real one and gives each connection an attribute table with made-up handles,
because its peers only know UUIDs. Those peers are simulated devices
(`sim_ble_add`; an app describes its own in JSON, so none of them is C) or,
through CoreBluetooth, real ones near the Mac. macOS
never shows a device's address, so the mock makes one up from the ID macOS
gives the device (random static, the same every time on that Mac). The tests
use only simulated devices; the window adds the Mac's Bluetooth.

On the chip the stack starts the first time a script uses Bluetooth and then
idles. Discovery (services, characteristics, CCCDs) runs on the host task
before a connection reports "connected". NimBLE makes one connection at a
time and doesn't connect while scanning, so `ble_poll()` in the main loop
queues the attempts and pauses the scan meanwhile. Scanning listens 30 ms of
every 100 ms, leaving the shared radio to Wi-Fi the rest. A request refused
for lack of pairing starts "just works" pairing (bonds kept in NVS) and is
sent again. Each bond is recorded as the app's that made it (`bleowners` in
NVS): other apps can't connect to that peer, uninstall and Clear data delete
it, and the store-full callback only evicts bonds of the same app or of none.
