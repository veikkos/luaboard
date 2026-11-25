# To verify on the real board

The board support was written from M5Stack's documentation and their display
library's CoreS3 setup, before the hardware was available. These are the
things to check on a real board, by part of the board.

## First

- The microSD card (`sd`). A FAT32 card mounts and reads and writes, with
  G35 handed between the display (its D/C) and the card (its data out)
  around each call. The display still draws right afterwards, and during a
  script that logs every second. A card put in before power-on doesn't
  disturb the display (its select line is high, but it isn't in SPI mode
  yet). An exFAT card and an empty slot give their two messages, and note
  how long the empty slot's attempt takes.

## Display and touch

- The LCD reset bit on the AW9523B (P1_1 per the docs, one report mentions
  P1_5).
- The LCD controller: an ILI9342C, or an ILI9342E on boards made from August
  2026. The firmware tells them by the touch firmware's id (0x10 or 0x12 in
  register 0xA6), as M5GFX 0.2.27 and the factory firmware do, and sends
  each M5GFX's set-up for it (`lcd_ili9342.c`). The picture is right on a
  Lite with the E (its factory firmware read the id as 0x12). Still to
  check on a board with the C: contrast, colors, no flicker.
- The AXP2101 DLDO1 backlight register values.
- Screen sleep puts the panel to sleep as well (display off, sleep in), and
  waking brings it back with its picture. Before that a Lite's panel, left
  on the Settings page while asleep, showed it faintly in other apps
  afterwards. Check on the board: after a sleep of a few seconds and of an
  hour the picture is back at once without a flash, what an app drew
  meanwhile is there, and no page leaves a mark any more. If marks still
  come from pages shown for long, look at the panel's voltages in the
  set-up (`lcd_ili9342.c`, from M5GFX).
- The backlight: changing it (menu drag, `disp.brightness`, leaving an app)
  leaves the other AXP2101 LDO rails on. Register 0x90 is read, modified and
  written back, and a failed read writes nothing.
- An LCD or framebuffer init failure. The "continuing headless" path isn't
  finished (`lcd_flush` and the gfx calls use the missing buffers), so a
  board whose display fails to start boot-loops instead of staying usable
  over USB.
- The touch strip. The SE's gives touches at y 258..279. A Lite's gives
  none, so its menu shows no button hints (`hw_has_buttons`). Still to
  check on a CoreS3, which is taken to be like the Lite.
- `disp.jpeg` speed for a 300 px cover, and that Spotify's covers are
  baseline JPEGs (TJpgDec can't decode progressive ones).
- Our TJpgDec overriding the copy in the ESP32-S3 ROM (it links, but hasn't
  run yet).
- `disp.png` of a 1000x600 RGBA PNG at scale 4 (the BMW CarData app's car
  picture): time on the chip, with the ROM's inflate.

## Power and sleep

- USB power in. The IO expander's P0_5 is held low, which lets it in: seen
  on a Lite, where left an input (pulled high) the board ran from its
  battery on USB without charging. Still to check: an SE, which has no
  battery to carry it, starts and stays up on USB alone.
- The PMIC's rails (register 0x90 and the voltages in 0x92..0x95) are left
  as the chip has them, and it keeps what the factory firmware wrote while
  it has power. M5Unified writes them at every start. Check a board whose
  PMIC lost all power (a flat battery, an SE unplugged): the display, the
  microSD card and the speaker work.
- 5V out to the ports is switched on in one step. M5Unified first charges
  the bus with short pulses, as on a weak USB supply the inrush can pull
  the PMIC's converters under their limit and power the board off. Check
  on a weak supply and a long cable.
- The IO expander's port 0 as push-pull (0x11 bit 4, set at boot). After
  reset it's open-drain, and the touch reset, the 5V out to the ports and
  the amplifier's reset are on that port.
- Battery detection: AXP2101 status1 bit3 reads 0 on an SE without a
  battery (so `sys.battery()` gives `nil` and the menu shows no battery).
- Model detection: the BMI270 at 0x69 answers chip ID 0x24 on CoreS3 and
  Lite, and nothing answers there on the SE.
- The power key: AXP2101 IRQ status 1 (0x49) bit 3 latches a short press
  once 0x41 bits 3:2 are enabled, and is write-1-to-clear.
- Light sleep (`CONFIG_PM_ENABLE`, tickless idle, the lock released while
  asleep on battery). Wi-Fi stays joined and the PC reaches the device,
  timers fire on time, a touch wakes it within a moment, a sound starts
  cleanly afterwards, and there are no brown-out resets.
  `POWER_LIGHT_SLEEP` 0 rules it out. Pins keep their state while the chip
  sleeps (a script's outputs, the LCD's lines): ESP-IDF's workaround that
  isolates them is off (`sdkconfig.defaults`), so also watch for resets it
  would have prevented.
- Power off: AXP2101 register 0x10 bit 0 powers the board down on battery,
  the power key turns it on again, and so does plugging in USB.
- The timed wake-up: the BM8563's timer and alarm interrupt turns the board
  back on. It shares the PMIC's IRQ line, as M5Unified's notes say, and the
  PMIC may need IRQ-low enabled as a power-on source. It comes on time (a
  count of n is taken to run n-1 to n periods, never longer), and the
  firmware reads the chip's flags after the boot (`sys.info().boot` is
  `"timer"`).
- The fallback when the PMIC doesn't cut the power: deep sleep that a touch
  ends (the touch interrupt through the AW9523B to GPIO21, low when
  touched) or the chip's timer.
- The current drawn awake, asleep and off (M5Stack gives about 110 mA
  working and 0.1 mA in standby). The CPU runs at 240 MHz: compare the
  current awake at 160 MHz (`hw.c` and `sdkconfig.defaults`) to see what
  the higher clock costs.

## Clock

- SNTP (`esp_netif_sntp`, pool.ntp.org) sets the time a second or two after
  Wi-Fi connects, and local time follows the POSIX TZ string.
- The BM8563 RTC at 0x51 (listed for the SE in M5Stack's docs): it's fitted
  and powered on the SE, keeps time over a reset, and sets VL after the
  board was unplugged.

## Wi-Fi, TLS and updates

- Wi-Fi: joining a real network, the lwIP `poll()`/`fcntl()` usage, the mdns
  component and throughput of the 150 KB live-preview frames.
- The encrypted Wi-Fi link: PSA AES-256-GCM in ESP-IDF's mbedTLS (with the
  S3's AES hardware), and the live preview's speed in 4 KB frames.
- TLS: handshake time and memory (mbedTLS buffers in PSRAM, 4 connections),
  the certificate bundle against Spotify's servers, and the 24 KB main task
  stack during a handshake.
- TLS 1.3: a connection to a server that takes only 1.3, such as BMW's
  CarData stream or `mqtts` to a broker limited to 1.3. Handshake time, and
  free heap and PSRAM before and after.
- The connection core (`conn.c`, shared by Lua sockets and the update
  check): a plain and a TLS `net.connect` to a name and to an address, a
  refused port and a name that doesn't resolve giving their messages,
  `net.listen` and `accept`, an `http.get` over https, and a pull update
  check and download, all on lwIP.
- A real pull update from GitHub: the manifest and `luaboard-m5stack-cores3-update.bin` through
  the redirect to GitHub's download host, the certificate bundle accepting
  both hosts, and the 1.6 MB image over TLS (time, memory, the main task's
  stack) into the other slot, then the reboot into it.
- The longest stall of `update_poll` (so of the main loop) during a check's
  TLS handshake while an app animates. Each handshake step runs in the
  loop, and a step that does the certificate's signature math may take long
  enough to show as a dropped frame.

## Bluetooth

- NimBLE next to Wi-Fi (coexistence, memory with its buffers in PSRAM),
  scanning and connecting to a real peripheral, and pairing with one that
  asks for it.

## Storage and memory

- LittleFS: mounting and formatting the ~9.9 MiB `apps` partition on first
  boot, and `rename()` replacing an existing file.
- How long `FORMAT`, `SAVE`, `INSTALL` and `UNINSTALL` take on the chip, so
  how much of the CLI's wait for each is margin (`FORMAT_TIMEOUT` 5 min
  15 s, `FLASH_TIMEOUT` 60 s in pc-tool/src/device.rs). A `luaboard format`
  and an install of a big app finish without "timeout waiting for reply".
- Free PSRAM with the 4 MiB Lua heap full (a script filling it), Wi-Fi and
  a TLS connection up. The firmware's own allocations (an update,
  Bluetooth) must still fit.

## Sensors, sound and pins

- The motion sensor (`imu`, BMI270): the configuration file loads (status
  1 in register 0x21) with the 256 byte bursts, the readings are sane (1 g
  at rest, near 0 degrees a second), which way the axes point on the board
  (the docs don't say yet), and that it's back in its idle state after the
  script.
- The light sensor (`light`, LTR-553ALS at 0x23): its part ID, lux in a
  room against a phone's reading, proximity rising as a hand comes near,
  and that the default measurement rates are what the code assumes.
- The speaker, set up as M5Unified does it: the AW88298 at 0x36 out of
  reset on the IO expander's P0_2, I2S on G34 (BCLK), G33 (WS) and G13
  (data) at 48 kHz, its register values (0x04, 0x05, 0x06 rate code 8,
  0x61, 0x0C). It plays on a Lite. At 44.1 kHz it stayed silent there: the
  amplifier's status (register 0x01) showed its PLL never locking onto the
  bit clock, where at 48 kHz it reads 0x0211. Still to check: how loud the
  settings are (30 dB from 1 % to 100 %), no pop when it turns on or off,
  and that the SE has it.
- The pins reserved for the speaker, microphone and camera (G0, G13-16,
  G33-34, G38-42, G45-48), taken from M5Stack's pin map.
- The free pins (Ports A, B and C, G5..G7, G10) are in ESP-IDF's reset
  state (not driven, pulled up weakly) once the firmware has started and
  again after a script used them, and the first `adc.read` of a pin is
  right (the pull-up goes off 10 ms before it).
- `gpio.pwm` (LEDC on the 80 MHz APB clock, 13 bits at most): the frequency
  and duty on a scope or a servo, at both ends of the range, duty 1 really
  fully on, and that the pin lets go when it's stopped.
- `uart` on Port C (UART1 on G18/G17): bytes both ways at a few baud rates,
  the pins free again after `uart.close()`, and a write just before
  `uart.close()` arriving whole (at 9600 baud a full 2 KB buffer is cut at
  0.5 s).

## USB, crashes and watchdogs

- USB: 150 KB frames and big `GET` replies arrive whole in 2 KB writes, and
  `usb_serial_jtag_is_connected()` goes false on battery and on a charger,
  so logging doesn't stall the loop.
- Opening the serial port (CLI and studio, on macOS) doesn't reset the
  board. The S3's USB-Serial/JTAG resets the chip when RTS is up and DTR
  down, and `open_link` raises DTR, then RTS. Test with a `put` and then a
  separate `run`: if the run says "no such app", the open reset the board.
  It matters most after an update, when the CLI opens the port again while
  the new firmware boots: a reset then makes the bootloader roll it back.
- The crash record: the wrapped `esp_panic_handler`
  (`app/idf/main/panic_hook.c`) runs on a crash, a watchdog and `abort()`,
  its stack trace matches the one ESP-IDF prints, and the record in RTC
  memory survives the reset (and reads as no crash after power-on).
- The crash-loop guard: `esp_reset_reason()` reports TASK_WDT, PANIC or
  BROWNOUT after those resets, and POWERON/USB/SW after unplugging, the
  reset button and `luaboard flash`, so only crashes count.
- The task watchdog (30 s, panic) doesn't fire during long but legitimate
  work: the boot (it starts right after NVS, paused only while the apps
  partition mounts or formats), installing big apps, the first `LS` after
  boot reading every file.
- The main task on core 1 (`sdkconfig.defaults`): the touch, I2C, SPI and
  sound interrupts it sets up land on core 1 and work. Compare an app's
  frame times during a big download with the main task on core 0.
