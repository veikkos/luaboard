# To verify on the real board

The board support was written from M5Stack's documentation and their display
library's CoreS3 setup, before the hardware was available:
* the LCD reset bit on the AW9523B (P1_1 per the docs; one report mentions P1_5);
* the AXP2101 DLDO1 backlight register values;
* whether the AW9523B power-path bits (BUS_OUT_EN / USB_OTG_EN) need setting;
* Wi-Fi: joining a real network, the lwIP `poll()`/`fcntl()` usage, the mdns
  component and throughput of the 150 KB live-preview frames;
* TLS: handshake time and memory (mbedTLS buffers in PSRAM, 4 connections),
  the certificate bundle against Spotify's servers, and the 24 KB main task
  stack during a handshake;
* `disp.png` of a 1000x600 RGBA PNG at scale 4 (the BMW example's car
  picture): time on the chip, with the ROM's inflate;
* free PSRAM with the 4 MiB Lua heap full (a script filling it), Wi-Fi and
  a TLS connection up: the firmware's own allocations (an update, Bluetooth)
  must still fit;
* TLS 1.3 (on since the BMW example): a connection to a server that takes
  only 1.3, such as BMW's CarData stream or `mqtts` to a broker limited to
  1.3: handshake time, and free heap and PSRAM before and after;
* `disp.jpeg` speed for a 300 px cover, and that Spotify's covers are baseline
  JPEGs (TJpgDec can't decode progressive ones);
* our TJpgDec overriding the copy in the ESP32-S3 ROM (it links; not yet run);
* the clock: SNTP (`esp_netif_sntp`, pool.ntp.org) setting the time a second
  or two after Wi-Fi connects, and `localtime_r` with the POSIX TZ string;
* the BM8563 RTC at 0x51 (listed for the SE in M5Stack's docs): that it's
  fitted and powered on the SE, keeps time over a reset, and sets VL after
  the board was unplugged;
* battery detection: AXP2101 status1 bit3 reads 0 on an SE without a battery
  (so `sys.battery()` gives `nil` and the menu shows no battery);
* model detection: the BMI270 at 0x69 answering chip ID 0x24 on CoreS3 and
  Lite, and nothing answering there on the SE;
* the power key: AXP2101 IRQ status 1 (0x49) bit 3 latching a short press
  once 0x41 bits 3:2 are enabled, and write-1-to-clear;
* the touch strip on CoreS3 and Lite (measured on the SE: y 258..279);
* a real pull update from GitHub: the manifest and `luaboard.bin` through the
  redirect to GitHub's download host, the certificate bundle accepting both
  hosts, and the 1.6 MB image over TLS (time, memory, the main task's stack)
  into the other slot, then the reboot into it;
* the longest stall of `update_poll` (so of the main loop) during a check's
  TLS handshake while an app animates: each handshake step runs in the loop,
  and a step that does the certificate's signature math may take long enough
  to show as a dropped frame;
* LittleFS: mounting and formatting the ~9.9 MB `apps` partition on first
  boot, and `rename()` replacing an existing file;
* how long `FORMAT`, `SAVE`, `INSTALL` and `UNINSTALL` take on the chip, so
  how much of the CLI's wait for each is margin (`FORMAT_TIMEOUT` 5 min 15 s,
  `FLASH_TIMEOUT` 60 s in pc-tool/src/device.rs): a `luaboard format` and an
  install of a big app finish without "timeout waiting for reply";
* the connection core (`conn.c`, shared by Lua sockets and the update check
  since it replaced their own copies): a plain and a TLS `net.connect` to a
  name and to an address, a refused port and a name that doesn't resolve
  giving their messages, `net.listen` and `accept`, an `http.get` over
  https, and a pull update check and download, all on lwIP;
* Bluetooth: NimBLE next to Wi-Fi (coexistence, memory with its buffers in
  PSRAM), scanning and connecting to a real peripheral, and pairing with one
  that asks for it;
* USB: 150 KB frames and big `GET` replies arriving whole in 2 KB writes, and
  `usb_serial_jtag_is_connected()` going false on battery and on a charger,
  so logging doesn't stall the loop;
* the pins reserved for the speaker, microphone and camera (G0, G13-16,
  G33-34, G38-42, G45-48), taken from M5Stack's pin map;
* the crash record: that the wrapped `esp_panic_handler`
  (`app/idf/main/panic_hook.c`) runs on a crash, a watchdog and `abort()`,
  that its stack trace matches the one ESP-IDF prints, and that the record
  in RTC memory survives the reset (and reads as no crash after power-on);
* the speaker, set up as M5Unified does it: the AW88298 at 0x36 out of reset
  on the IO expander's P0_2, I2S on G34 (BCLK), G33 (WS) and G13 (data) at
  44.1 kHz, its register values (0x04, 0x05, 0x06 rate code 7, 0x61, 0x0C),
  how loud 50 % is, and no pop when it turns on or off. Also that all three
  models have it;
* the IO expander's port 0 as push-pull (0x11 bit 4, set at boot): after
  reset it's open-drain, and the touch reset, the 5V out to the ports and
  the amplifier's reset are on that port;
* the task watchdog (30 s, panic) not firing during long but legitimate
  work: the boot (it starts right after NVS, paused only while the apps
  partition mounts or formats), installing big apps, the first `LS` after
  boot reading every file;
* the encrypted Wi-Fi link: PSA AES-256-GCM in ESP-IDF's mbedTLS (with the
  S3's AES hardware), and the live preview's speed in 4 KB frames;
* opening the serial port (CLI and studio, on macOS) doesn't reset the
  board: the S3's USB-Serial/JTAG resets the chip when RTS is up and DTR
  down, and `open_link` raises DTR, then RTS. Test with a `put` and then a
  separate `run`: if the run says "no such app", the open reset the board.
  It matters most after an update, when the CLI opens the port again while
  the new firmware boots: a reset then makes the bootloader roll it back;
* the backlight: changing it (menu drag, `disp.brightness`, leaving an app)
  leaves the other AXP2101 LDO rails on (register 0x90 is read, modified and
  written back; a failed read now writes nothing);
* the crash-loop guard: `esp_reset_reason()` reporting TASK_WDT, PANIC or
  BROWNOUT after those resets, and POWERON/USB/SW after unplugging, the
  reset button and `luaboard flash`, so only crashes count;
* an LCD or framebuffer init failure: the "continuing headless" path isn't
  finished (`lcd_flush` and the gfx calls use the missing buffers), so a
  board whose display fails to start boot-loops instead of staying usable
  over USB;
* light sleep (`CONFIG_PM_ENABLE`, tickless idle, the lock released while
  asleep on battery): Wi-Fi stays joined and the PC reaches the device,
  timers fire on time, a touch wakes it within a moment, a sound starts
  cleanly afterwards, and there are no brown-out resets. `POWER_LIGHT_SLEEP`
  0 rules it out. Pins keep their state while the chip sleeps (a script's
  outputs, the LCD's lines): ESP-IDF's workaround that isolates them is
  off (`sdkconfig.defaults`), so also watch for resets it would have
  prevented;
* power off: AXP2101 register 0x10 bit 0 powers the board down on battery,
  the power key turns it on again, and so does plugging in USB;
* the timed wake-up: the BM8563's timer and alarm interrupt turns the board
  back on (it shares the PMIC's IRQ line, as M5Unified's notes say; the
  PMIC may need IRQ-low enabled as a power-on source), on time (a count of
  n is taken to run n-1 to n periods, never longer), and the firmware reads
  the chip's flags after the boot (`sys.info().boot` is `"timer"`);
* the fallback when the PMIC doesn't cut the power: deep sleep that a touch
  ends (the touch interrupt through the AW9523B to GPIO21, low when
  touched) or the chip's timer;
* the microSD card (`sd`), first of the new things to try: a FAT32 card
  mounts and reads and writes, with G35 handed between the display (its
  D/C) and the card (its data out) around each call, and the display still
  draws right afterwards and during a script that logs every second; a card
  put in before power-on doesn't disturb the display (its select line is
  high, but it isn't in SPI mode yet); an exFAT card and an empty slot give
  their two messages, and how long the empty slot's attempt takes;
* the motion sensor (`imu`, BMI270): the configuration file loads (status
  1 in register 0x21) with the 256 byte bursts, the readings are sane (1 g
  at rest, near 0 degrees a second), which way the axes point on the board
  (the docs don't say yet), and that it's back in its idle state after
  the script;
* the light sensor (`light`, LTR-553ALS at 0x23): its part ID, lux in a
  room against a phone's reading, proximity rising as a hand comes near,
  and that the default measurement rates are what the code assumes;
* `gpio.pwm` (LEDC on the 80 MHz APB clock, 13 bits at most): the
  frequency and duty on a scope or a servo, at both ends of the range,
  duty 1 really fully on, and that the pin lets go when it's stopped;
* the free pins (Ports A, B and C, G5..G7, G10): in ESP-IDF's reset state
  (not driven, pulled up weakly) once the firmware has started and again
  after a script used them, and the first `adc.read` of a pin right (the
  pull-up goes off 10 ms before it);
* `uart` on Port C (UART1 on G18/G17): bytes both ways at a few baud
  rates, the pins free again after `uart.close()`, and a write just before
  `uart.close()` arriving whole (at 9600 baud a full 2 KB buffer is cut
  at 0.5 s);
* the current drawn awake, asleep and off (M5Stack gives about 110 mA
  working and 0.1 mA in standby).
