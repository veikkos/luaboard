# To verify on the real board

The board support was written from M5Stack's documentation and their display
library's CoreS3 setup, before the hardware was available. A CoreS3 Lite has
since run it: what was seen working there is no longer listed. These are the
things still to check, by part of the board. The last section is what needs
a board other than a Lite.

## The microSD card

- An exFAT card and an empty slot give their two messages, and how long the
  empty slot's attempt takes.
- A card put in before power-on doesn't disturb the display (its select
  line is high, but it isn't in SPI mode yet). Seen over resets, not yet
  from power off.
- A card pulled out or swapped while an app runs: the next call says "no
  card", or works on the new card. The simulator's tests cover it.

## Display

- The AXP2101 DLDO1 backlight register values: the dimmest setting is still
  readable, the brightest is full.
- An LCD or framebuffer init failure leaves the device usable over USB
  (nothing is drawn without a framebuffer, and flushes are dropped unless
  the LCD started). It can't be tried without a display that fails.
- Spotify's covers are baseline JPEGs (TJpgDec can't decode progressive
  ones). A 300 px JPEG of 10 KB draws in 0.1 s.
- `disp.png` of the BMW CarData app's car picture (1000x600 RGBA at scale
  4). A picture of that size with plain colors (42 KB) takes 0.9 s; a
  photo has more to inflate.

## Power and sleep

- 5V out to the ports is off until an app takes a pin, and switched as
  M5Unified does it: the converter, the bus charged with short pulses, then
  the switch (on a weak USB supply the inrush can pull the PMIC's
  converters under their limit and power the board off), and off with a
  200 ms wait in between. Check that a unit on Port A answers an app's
  first read, on battery and on USB, also on a weak supply and a long
  cable, and what the board draws with it off and on.
- The power key's long press: AXP2101 IRQ status 1 (0x49) bit 2, while the
  key is still held, well before the PMIC's own power-off time.
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
- The current drawn awake, asleep, off and with the camera on (M5Stack
  gives about 110 mA working and 0.1 mA in standby). The CPU runs at
  240 MHz: compare the current awake at 160 MHz (`hw.c` and
  `sdkconfig.defaults`) to see what the higher clock costs.

## Wi-Fi, TLS and updates

- Internal RAM (`mem` in `luaboard info`: free now and the least since the
  start) with a Bluetooth connection open on top of the rest. On a Lite it
  is about 140 KB free in the menu, 100 KB at the least over update checks,
  90 KB with a Bluetooth scan and the speaker running, and 84 KB (48 KB at
  the least) with four TLS connections open, the card mounted and the
  camera and the light sensor on.
- TLS 1.3: a connection to a server that takes only 1.3, such as BMW's
  CarData stream or `mqtts` to a broker limited to 1.3. The BMW app has run
  against the stream. Not measured: handshake time, and free heap and PSRAM
  before and after.
- The longest stall of `update_poll` (so of the main loop) during a check's
  TLS handshake while an app animates. Each handshake step runs in the
  loop, and a step that does the certificate's signature math may take long
  enough to show as a dropped frame.

## Storage and memory

- How long `FORMAT`, `SAVE` and `INSTALL` take on the chip, so how much of
  the CLI's wait for each is margin (`FORMAT_TIMEOUT` 5 min 15 s,
  `FLASH_TIMEOUT` 60 s in pc-tool/src/device.rs). A `luaboard format` and an
  install of a big app finish without "timeout waiting for reply", and the
  task watchdog (30 s) doesn't fire during them.
- Listing the apps is slow on the flash: `luaboard apps` takes 0.6 s with 9
  apps of 29 files, and the menu's own reload after any upload or uninstall
  about 1.5 s, in which the board answers nothing (see TODO.md).
- With the Lua heap full (3.9 of its 4 MiB) an HTTPS request still goes
  through. Still to check then: a firmware update, and Bluetooth.

## Sensors, sound and pins

- The motion sensor (`imu`, BMI270): which way the axes point on the board
  (the docs don't say yet).
- The compass (`imu.mag()`, a BMM150 on the BMI270's auxiliary bus). On a
  Lite it answers with its chip ID and trim values, and the BMI270 fetches
  its data. With the magnetic back cover on, Y reads out of range and X
  about 250 microtesla, so `imu.mag()` says the field is too strong. Still
  to check with the cover off, or on a CoreS3: about 50 microtesla in all,
  and which way its axes point against the accelerometer's.
- The camera: now and then a picture comes short (216 of its 240 rows) and
  the driver drops it, seen while a JPEG was made and written to the card.
  Why isn't known.
- The speaker: how loud the settings are (30 dB from 1 % to 100 %), and no
  pop when it turns on or off.
- The pins reserved for the microphones (G0, G14), taken from M5Stack's pin
  map. The microphones aren't used yet.
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

- `usb_serial_jtag_is_connected()` goes false on battery and on a charger,
  so logging doesn't stall the loop.
- The computer giving up on the port over a restart. Twice in some forty
  updates over USB a Lite came up running but with USB dead: still listed
  on the computer, but no frames reaching the chip (its frame counter
  stood still), so no answers and no reset by a flasher either, and a
  restart didn't end it. Dropping D+'s pull-up for 0.3 s did, at once. The
  firmware now does that itself when USB power is there and no computer is
  heard three seconds after the start (`usb_host_poll` in hw.c), and the
  pulse was seen to work on the board. Still to see: the fault itself
  mended that way, as it comes too seldom to wait for. And why the computer
  gives up: the chip's USB keeps its connection over a restart while the
  clock it runs from is set up anew, and errors were latched in its status.
- Opening the serial port doesn't reset the board from the studio, and on
  Windows and Linux (the CLI on macOS doesn't). The S3's USB-Serial/JTAG
  resets the chip when RTS is up and DTR down, and `open_link` raises DTR,
  then RTS. Test with a `put` and then a separate `run`: if the run says
  "no such app", the open reset the board. It matters most after an update,
  when the port is opened again while the new firmware boots: a reset then
  makes the bootloader roll it back.
- The crash-loop guard's reset reasons: `esp_reset_reason()` after a
  brown-out, and POWERON after unplugging and the reset button. A crash and
  `abort()` (panic), a hung main loop (task watchdog), an update (sw) and
  `luaboard flash` (usb) were seen, each with its crash record or none, and
  three crashing boots in a row skipped the autostart app.
- The main task on core 1 (`sdkconfig.defaults`): compare an app's frame
  times during a big download with the main task on core 0.

## On another board

A CoreS3 SE:

- It starts and stays up on USB alone, with no battery to carry it, also
  after its PMIC lost all power (the rails are left as the chip has them,
  but for the three of the sensors and the camera, ALDO3, BLDO1 and BLDO2,
  which the firmware switches off at every start: on a Lite the display,
  the card and the speaker run that way).
- Battery detection: AXP2101 status1 bit3 reads 0 without a battery, so
  `sys.battery()` gives `nil` and the menu shows no battery.
- Model detection: nothing answers at 0x69, where a CoreS3 and a Lite have
  the BMI270.
- The BM8563 RTC at 0x51 (listed for the SE in M5Stack's docs): it's fitted
  and powered, keeps time over a reset, and sets VL after the board was
  unplugged.
- The speaker plays.
- The touch strip gives touches at y 258..279, as the three buttons.

A CoreS3:

- It starts with ALDO3, BLDO1 and BLDO2 switched off (they are for the
  light sensor and the camera, seen on a Lite), and is told from an SE.
- The touch strip, taken to be like the Lite's, which gives no touches (so
  the menu shows no button hints, `hw_has_buttons`).
- The compass, without a magnet behind it.

A board with the older LCD controller, the ILI9342C (the firmware tells it
from the E by the touch firmware's id, 0x10 or 0x12 in register 0xA6, and
sends each M5GFX's set-up for it):

- Contrast, colors, no flicker.
