# Lua API reference

Board: M5Stack CoreS3, CoreS3 Lite or CoreS3 SE, an ESP32-S3 with a 320×240
color touch screen. `sys.info().model` says which.

Scripts run on the device in a sandboxed Lua 5.4 VM. Besides the safe parts
of the standard library (`string`, `table`, `math`, `utf8`, `coroutine`, base
functions, but no `io`, `os`, `debug`, `package`, `dofile`, `loadfile`), the
firmware exposes the modules below. `load` takes Lua source only: precompiled
bytecode is refused (unchecked bytecode can break out of the VM), and
`string.dump` is gone. `math.random` is seeded from the hardware random
number generator each time an app starts. Names starting with `_` (`net._join`, `touch._event`)
are the firmware's own, not part of the API: they can change in any release.

## API version

This is version 2 of the Lua API. An app says which version it was written
for with `lua_api` in its `app.json` (no `lua_api` means 1):

```json
{ "title": "Clock", "version": "1.0.0", "license": "MIT", "lua_api": 1 }
```

The firmware runs apps from the oldest version it still supports up to its
own (`sys.info().lua_api`, `api=` in `INFO`). Additions raise the version,
so an app that uses something new sets `lua_api` to that version and an
older firmware says "needs newer firmware" instead of failing halfway. A
change that breaks existing apps also raises the oldest version supported.
Those changes will be listed here. An installed app outside the range stays
on the device, greyed in the menu, until a firmware or a new version of the
app that fits comes. Version 1 is what the first release, 0.9.0, had.

| Version | Added |
|---|---|
| 2 | `imu.mag`, `camera` |

## Errors

A wrong call raises an error: a missing or mistyped argument, a value out
of range, a pin the firmware owns. That stops the script, as any Lua error
does, unless it's caught with `pcall`. The message names the app's line
that made the call, also for the libraries written in Lua (`http`, `mqtt`,
`json`, tasks and timers), not a line inside them. Something that can go wrong however
right the call is (the network, a device, a file, data from outside such
as a JPEG or WAV) comes back as `nil, err`, with `err` a message saying
what happened, so `assert(...)` or an `if` handles it:

```lua
local w, h = disp.jpeg(0, 0, data)
if not w then print(h) end                -- e.g. "disp.jpeg: not a JPEG"
```

Running out of room in `store` or `secrets` raises too: an app that went on
without noticing would lose what it meant to keep.

A flag (`fill` of `disp.rect`, `on` of `disp.keep_awake`, `opts.tls`) is
`true` or `false`, and leaving it out means `false`. Anything else, a `0`
or `1` included, is an error: in Lua `0` counts as true, so `disp.sleep(0)`
would otherwise put the device to sleep. Levels, such as `gpio.write`'s,
take numbers.

## Script structure

```lua
-- An app is a folder; it starts with its main.lua (from the menu, RUN, or autostart).
-- Top-level code runs once when the script starts.

function setup()        -- optional: called once after the top-level chunk
end

function loop(dt)       -- optional: called repeatedly, dt = ms since last call
end                     -- default rate 30 Hz, change with sys.rate(hz)
```

* The display is flushed automatically after top-level code and `setup()`,
  and after every tick (due timers, tasks and `loop()`), if anything was drawn.
  Only the changed rectangle is sent to the panel. `disp.show()` forces a flush
  immediately.
* A watchdog limits each call into Lua to 1000 ms: the top-level chunk,
  `setup()`, and each tick (all due timers, task slices and `loop()`
  together). Never busy-wait: use `loop()`, timers or tasks. The time spent
  in `sys.delay`, `disp.jpeg`, TLS handshakes and `store`'s flash writes
  doesn't count, but no call can run longer than 10 s in all. Once it
  trips, `pcall` and `xpcall` still catch the error, but the next Lua
  instruction raises it again, so the call ends anyway (an `xpcall` handler
  isn't called then).
* The VM heap is capped at 4 MiB (in PSRAM). `sys.heap()` shows the usage.
* Nesting is limited by the device's C stack, not a fixed count: Lua code
  calling Lua code costs nothing, but each level that goes through C (a
  `pcall`, a metamethod, a `string.gsub` or `table.sort` callback, a
  coroutine) uses some. Too deep is the error "C stack overflow". Expect a
  dozen or so nested `gsub` callbacks and a few dozen `pcall`s. The
  simulator has the same stack size, so it shows where an app stands.
* An error stops the script: a syntax error, a runtime error in top-level
  code, `setup()`, `loop()` or a timer, the watchdog, or out-of-memory. The
  device then shows an error screen with the file, line and message, and
  how to recover (with its Wi-Fi address if connected). Pressing B or tapping
  the screen runs the script again, and holding B or a finger on the screen
  goes back to the menu. The PC also gets it as `EVT error <message>`.
* `print(...)` sends its output to the PC as `LOG` lines (arguments
  tab-separated).
* `require(name)` loads `<name>.lua` (or `name` as is if it ends in `.lua`)
  from the same app, or else a library built into the firmware (`mqtt`,
  `http`, `json`, `oauth`). A file of the app with the same name replaces the
  built-in one. Each module runs once. Its return value (`true` if it returns
  nothing) is cached. A module that requires itself, directly or through
  another, is an error. Error messages name the file without the app
  (`main.lua:11: ...`).
* `asset(name)` -> `data` or `nil, err` reads a file of the app as a string:
  a picture for `disp.jpeg` or `disp.png`, or a sound for `sound.play`. The
  PC tools send an app's `.jpg`, `.jpeg`, `.png` and `.wav` files with its
  Lua files (not those in subfolders). Each file can
  be up to 256 KiB, and uploads that aren't installed yet share 512 KiB. Only
  the app's own files: a name with `/` is an error.

  ```lua
  disp.jpeg(0, 0, assert(asset("logo.jpg")))
  sound.play(assert(asset("ding.wav")))
  ```
* Holding B or a short press of the power key always leaves the app (see
  [touch](#touch---capacitive-touch-screen)). `sys.exit()` leaves from the
  script itself, at once: nothing after it runs, `setup()` included when
  it's called from the top-level code.

### A script that draws nothing

The firmware doesn't leave the screen black without a reason. If a script
finishes without drawing anything and leaves no `loop()`, timer or task, the
screen says so. If the file returned a table, the screen says it looks like a
module that another script should `require()`. If it keeps running but hasn't
drawn anything after 1.5 s, the screen says it's running. The first drawing
call replaces the notice with a black screen, as if the notice was never there.
The same text goes to the log.

### What carries over between scripts

Every run starts clean. When a script starts, is stopped (`STOP`) or fails:

* Lua state: the next run gets a fresh VM, so no globals, timers or tasks
  survive. After `STOP` the VM is kept for `EVAL`, but nothing in it runs.
* GPIO: every pin the script configured or drove is reset to how a pin no
  app uses is (see `gpio`), so it's no longer driven, and `gpio.pwm` stops.
  A relay or motor switched on by a script doesn't stay on after it (with
  the pull-down the `gpio` section asks for).
* Serial port: `uart` is closed (what it was still sending gets up to
  0.5 s to go out) and its pins are free.
* Sensors: `imu` and `light` are switched off, and so is the `camera`,
  its picture no longer mirrored.
* SD card: the card is let go of (nothing is left open on it).
* Network: all of the script's sockets (and so MQTT and kept HTTP
  connections) are closed.
* Browser login: a pending `browser.login` is cancelled.
* Sound: what's playing stops and the queue is dropped.
* Display: brightness goes back to the Brightness setting, and a new script starts on a
  black screen. A failed script gets the error screen instead.
* Settings: `sys.rate()` goes back to 30 Hz, and `disp.keep_awake` and
  `sys.keep_on` to off.

The device's own settings (Wi-Fi network, pairing key, installed apps, brightness,
autostart) belong to the firmware, and scripts can't change them. What an app
wants to keep goes in [`store`](#store---values-an-app-keeps) (or
[`secrets`](#secrets---tokens-and-api-keys) for tokens).

## disp - 320x240 color LCD

Coordinates: x = 0..319 (left to right), y = 0..239 (top to bottom).
Drawing clips to the screen. Coordinates and sizes beyond -32767..32767 are
clamped to that range. Colors are integers `0xRRGGBB`. The panel stores 16-bit color (RGB565),
so `disp.get()` returns the nearest representable color. `color` defaults
to white (black for `disp.clear`). Values outside `0..0xFFFFFF` are an error.

| Function | Description |
|---|---|
| `disp.clear([color])` | Fill the screen, default black. |
| `disp.pixel(x, y, [color])` | Set one pixel (default white). |
| `disp.get(x, y)` -> `color` | Read a pixel from the framebuffer. |
| `disp.line(x0, y0, x1, y1, [color])` | Line. |
| `disp.rect(x, y, w, h, [color], [fill])` | Rectangle outline, or filled if `fill` is true. |
| `disp.circle(x, y, r, [color], [fill])` | Circle outline or filled disc. |
| `disp.text(x, y, str, [color], [scale], [bg], [align])` -> `width` | 5x7 font, 6 px per character at scale 1, `scale` 1..8. With `bg` the character cells are filled first (no flicker when updating text). `\n` starts a new line. `align` is `"left"` (the default), `"center"` (each line centered on `x`) or `"right"` (each line ending at `x`): `disp.text(disp.W // 2, y, s, c, 2, nil, "center")`. `str` is UTF-8. The font has ASCII, Latin-1 (Nordic and Western European letters, ° £ µ and the like), Œ Š Ž Ÿ and €. Other accented letters are drawn without the accent (č as c), other characters as `?`. Every character is one cell (6 px), except `…`, drawn as `...`. |
| `disp.text_width(str, [scale])` -> `width` | Width in pixels without drawing. |
| `disp.wrap(str, width, [scale])` -> `lines` | `str` broken at spaces into lines `disp.text` draws at most `width` px wide (a list). `\n` starts a new line, and a word longer than a line is cut. |
| `disp.image(x, y, w, h, pixels)` | Draw a `w`x`h` image (each 1..4096). `pixels` is a string of `w*h*2` bytes: RGB565, big-endian, rows top to bottom. Clipped to the screen. |
| `disp.grab(x, y, w, h)` -> `pixels` | The screen's rectangle in the form `disp.image` takes, to keep what was drawn and draw it again later (a decoded picture, say). Pixels off the screen are black. |
| `disp.jpeg(x, y, data, [scale])` -> `w, h` or `nil, err` | Decode a baseline JPEG (a string: an `asset`, or from `http.get`) onto the screen. `scale` 1, 2, 4 or 8 shrinks while decoding (a 300 px image at 2 is 150 px). Progressive JPEGs aren't supported: data it can't decode is `nil, "disp.jpeg: ..."`. |
| `disp.jpeg_size(data)` -> `w, h` or `nil, err` | Size of a JPEG without drawing it. |
| `disp.png(x, y, data, [scale])` -> `w, h` or `nil, err` | Decode a PNG (a string: from `http.get`, say) onto the screen. Every color type and bit depth, but not interlaced ones. Transparent and partly transparent pixels blend over what is on the screen. `scale` 1, 2, 4 or 8 shrinks while decoding, each pixel the average of the ones it covers. Only a few rows are in memory at a time, whatever the image's size. Data it can't decode is `nil, "disp.png: ..."`. |
| `disp.png_size(data)` -> `w, h` or `nil, err` | Size of a PNG, from its header. |
| `disp.rgb(r, g, b)` -> `color` | Build a color from 0..255 components. |
| `disp.show()` | Push changes to the panel now. |
| `disp.brightness([0..100])` -> `level` | Backlight. 0 turns it off. Starts at the device's Brightness setting and goes back to it when the script ends. Without an argument, returns the current level. |
| `disp.keep_awake([on])` -> `on` | While on, the device's Sleep after setting doesn't put it to sleep, for an app meant to be watched (a clock, a dashboard). Holding the power key or Sleep now still does. Off again when the script ends. Without an argument, returns the current state. |
| `disp.sleep([on])` -> `on` | `true` puts the device to sleep, the same as Sleep now in the settings: the screen goes off, the app keeps running, and a touch or the power key wakes it. `false` wakes it, for an app that has something to show (an alarm). Without an argument, returns whether it's asleep. |
| `disp.W`, `disp.H` | 320, 240 |
| `disp.BLACK WHITE GRAY DARKGRAY RED GREEN BLUE YELLOW CYAN MAGENTA ORANGE NAVY` | Color constants. |

## touch - capacitive touch screen

The touch panel is 320×280: it extends 40 px below the LCD, and that strip
works as three touch buttons A, B and C, left to right (printed `‹‹`, `○`,
`››` on the SE's glass). A Lite's strip gives no touches, and the CoreS3 is
taken to be the same: an app that should work there (`sys.info().model` is
`"cores3"`) can't count on the buttons.

The firmware tells presses from long presses, with the same timing as the
home menu (a long press is 0.8 s), and gives the script one event per
touch. A long press of B and a short press of the side power key belong to
the firmware: they always leave the app and return to the home menu, the
script stopped as with `STOP`, and never reach the script. Where the finger went down decides what the touch is: one that starts
on the strip is a button press, one that starts on the screen is a tap, hold
or drag, even if the finger drifts across the edge. A screen touch that
stays within 16 px of where it went down is a tap or a hold. One that goes
further is a drag, never a tap. Use these for buttons and taps rather than
timing touches yourself. Each event goes only to its own handler: a script
that wants a slow press to count like a short one registers the same
function for both.

| Event | Handler | When |
|---|---|---|
| `"press"` | `fn(button)`: `"A"`, `"B"` or `"C"` | A button let go before the long-press time. Sliding off the button cancels the press. |
| `"long"` | `fn(button)`: `"A"` or `"C"` | A button held that long (once, while it's still held). No `"press"` follows. |
| `"tap"` | `fn(x, y)` | The screen touched and let go before that time, without dragging. (x, y) is where it went down. |
| `"hold"` | `fn(x, y)` | The screen held in place that long. No `"tap"` follows. |
| `"drag"` | `fn(x, y, x0, y0, ended)` | The finger went more than 16 px from (x0, y0), where it went down: at each new (x, y), and once more where it was let go, with `ended` true. |
| `"swipe"` | `fn(dir, x, y)` | A drag let go more than 16 px from where it started (after its last `"drag"`): `dir` is `"up"`, `"down"`, `"left"` or `"right"`, from (x, y), where it started. |

| Function | Description |
|---|---|
| `touch.on(event, fn)` | Call `fn` for that event (one handler per event, and `nil` removes it). Handlers run before `loop()`, and a script with a handler keeps running even without `loop()` or timers. |
| `touch.read()` -> `x, y` or `nil` | Where the finger is on the LCD, or `nil` when not touched (or the touch went down on the button strip). For drawing. |
| `touch.button()` -> `"A"`/`"B"`/`"C"` or `nil` | The button held, for "while held" states: from the touch going down on it until it's let go, the same frame its `"press"` comes (sliding off cancels it). For presses use `"press"`. |
| `touch.W`, `touch.H` | 320, 280 (the whole panel, button strip included) |

A frame (the handlers, timers and `loop()` of one tick) sees one moment of
the touch: `touch.read()`, `touch.button()` and the events agree and don't
change during it. The touch is still read every 10 ms meanwhile, also during
a long call, so a short tap isn't missed: it counts from the next frame.

```lua
touch.on("press", function(b) print("pressed " .. b) end)
touch.on("long", function(b) print("held " .. b) end)
touch.on("tap", function(x, y) disp.circle(x, y, 6, disp.YELLOW, true) end)
touch.on("drag", function(x, y, x0, y0, ended)
  disp.line(x0, y0, x, y, ended and disp.GREEN or disp.GRAY)
end)
```

## adc - analog inputs (ESP32-S3 ADC1, 12-bit)

Pins are GPIO numbers, as in `gpio`: `adc.PORTB_IN` is 9, the same as
`gpio.PORTB_IN`.

| Function | Description |
|---|---|
| `adc.read(pin)` -> `0..4095` | Raw reading of an analog pin, GPIO1..10 (G3 and G4 are the LCD and SD pins and can't be used). |
| `adc.read_mv(pin)` -> `mV` | Approximate voltage, 0..~3100 mV (12 dB attenuation, uncalibrated). |
| `adc.PORTB_IN` | G9, Grove Port B yellow wire (e.g. Angle unit). |
| `adc.PORTB_OUT` | G8, Port B white wire. |
| `adc.PORTA_SDA`, `adc.PORTA_SCL` | G2 / G1 (unless the script uses `i2c`). |
| `adc.MAX` | 4095 |

The first reading of a pin in a run takes 10 ms: the pull-up a free pin has
(see `gpio`) is switched off, and the voltage is given time to settle. That
is enough unless the source is weak and has a capacitor on the pin (over
about 10 kΩ with 100 nF): such a pin can read high for some tens of
milliseconds more, so read it again after a `sleep`.

## gpio - digital pins

| Function | Description |
|---|---|
| `gpio.mode(pin, mode)` | `gpio.IN`, `gpio.OUT`, `gpio.IN_PULLUP`, `gpio.IN_PULLDOWN`. |
| `gpio.write(pin, v)` | `v` is 0/1 or a boolean. The pin must be an output (`gpio.mode(pin, gpio.OUT)`) or pulsing, or it's an error. |
| `gpio.read(pin)` -> `0/1` | Input level (for outputs: the driven level). |
| `gpio.toggle(pin)` -> `new level` | Toggle an output. |
| `gpio.pwm(pin, hz, duty)` | Pulse the pin `hz` times a second (10..40000), high for `duty` of the time (0..1): dim an LED, drive a buzzer or a hobby servo. Calling it again changes it. `gpio.pwm(pin, 0)` stops it. |
| `gpio.pwm(pin)` -> `hz, duty` or `nil` | What the pin is pulsing at, `nil` if it isn't. |
| `gpio.PORTA_SDA`, `gpio.PORTA_SCL` | G2, G1 |
| `gpio.PORTB_IN`, `gpio.PORTB_OUT` | G9, G8 |
| `gpio.PORTC_RX`, `gpio.PORTC_TX` | G18, G17 |

Up to 8 pins can pulse at a time, at up to 4 different frequencies (pins at
the same frequency share one). The duty has 8192 steps up to about 9.7 kHz
and fewer above (1024 at 40 kHz). 0 is always off and 1 always on.
`gpio.mode`, `gpio.write`, `gpio.toggle` and `adc.read` on a pulsing pin
stop the pulses, and so does the end of the script. A stopped pin goes
back to how it was before any app used it (below). While a pin pulses, a device asleep on battery doesn't go into its
light sleep (the pulses would stop).

```lua
gpio.pwm(gpio.PORTB_OUT, 50, 0.075)              -- a servo on Port B: 1.5 ms pulses, its middle
gpio.pwm(gpio.PORTB_OUT, 1000, 0.2)              -- or an LED at a fifth of its brightness
```

A pin no app is using is always in the same state, from when the firmware
has started and after an app let go of it: not driven, with the chip's weak
pull-up (about 45 kΩ to 3.3 V), as ESP-IDF resets a pin. So `gpio.read`
needs `gpio.mode` first, and something that must stay off while no app
drives it (a transistor's gate, a relay) needs its own pull-down resistor,
4.7 kΩ or less (10 kΩ leaves 0.6 V on the pin, enough to half open a
small transistor). An LED wired straight to a pin can glow faintly on the
pull-up. For about a second after power-on or a reset, until the firmware
has started, the pins aren't pulled either way: what must be held high
through that needs its own pull-up.

Pins used by the firmware or wired to chips on the board can't be touched
from Lua, with `gpio`, `adc` or `uart`: LCD (G3, G35, G36, G37), SD card
select (G4), internal I2C (G11, G12) and its interrupt line (G21), USB (G19,
G20), flash/PSRAM (G26..G32), the speaker and microphone (G0, G13, G14, G33,
G34), the camera (G15, G16, G38..G42, G45..G48) and the firmware's log
output (G43, G44). GPIO22..25 don't exist on the ESP32-S3. That leaves the
ports' pins (G1, G2, G8, G9, G17, G18) and G5, G6, G7 and G10.

## i2c - add-on units on Port A

The I2C bus on Grove Port A (G2 SDA, G1 SCL), for sensors and other units,
such as M5Stack's ENV units. Only Port A: the internal bus with the power
chip, touch and clock is out of reach. Addresses are 7-bit, 0x08..0x77.

| Function | Description |
|---|---|
| `i2c.scan()` -> `{ addr, ... }` | Addresses that answer. `nil, err` if the bus is stuck. |
| `i2c.write(addr, data)` -> `true` or `nil, err` | Write bytes (1..1024). |
| `i2c.read(addr, n)` -> `data` or `nil, err` | Read `n` bytes (1..1024). |
| `i2c.write_read(addr, data, n)` -> `data` or `nil, err` | Write, then read after a repeated START: a register read. |
| `i2c.speed([hz])` -> `hz` | Bus clock, 10000..400000 (default 100000). |

`err` is e.g. `"no answer from 0x44"` when nothing is at that address. The
bus is set up on first use and released when the script ends. Meanwhile
`gpio` and `adc` can't use the Port A pins, and the other way round. Each
call blocks while it runs (well under a millisecond for a few bytes at
100 kHz). A sensor that needs time to measure is given it with `sleep`:

```lua
task(function()                                  -- SHT40 (ENV IV unit) at 0x44
  i2c.write(0x44, "\xFD")                         -- measure
  sleep(10)
  local d = assert(i2c.read(0x44, 6))
  local temp = -45 + 175 * string.unpack(">I2", d, 1) / 65535
  print(string.format("%.1f C", temp))
end)
```

The units get their power (5V) from the port. It comes on when the script
first uses a port (here: the first `i2c` call), so a unit needs a moment
before it answers, and goes off when the script ends. `sys.port_power(false)`
turns it off. `marketplace/ha_env` reads one and reports it to Home Assistant. In the
simulator, `--set porta=env` plugs in a simulated one.

## uart - a serial port

One serial port for units that talk over a UART: a GPS, a serial sensor,
another board. It is on Grove Port C (G18 in, G17 out) unless other pins are
given, at 3.3 V levels.

| Function | Description |
|---|---|
| `uart.open(baud, [opts])` -> `true` | Open the port at `baud` (300..2000000), 8 data bits, no parity, 1 stop bit. `opts` changes that: `{ rx = pin, tx = pin, bits = 5..8, parity = "none"\|"even"\|"odd", stop = 1\|2 }`. Opening an open port closes it first. |
| `uart.write(data)` -> `n` | Send bytes. `n` is how many were taken: up to about 1000 at a time, fewer while the output buffer (2 KB) is filling up. Send the rest later. |
| `uart.read([max])` -> `data` | What has arrived, up to `max` bytes (default 1024, at most 4096): `""` if nothing. |
| `uart.line()` -> `line` or `nil` | A whole line that has arrived, without its `\n` or `\r\n`. `nil` until one is complete. A line over 1024 bytes comes in pieces. |
| `uart.close()` | Close the port and free its pins (also done when the script ends). What is still queued for sending goes out first: the call waits up to 0.5 s for it, and a slow rate's leftover past that is dropped. Opening the port again does the same, so a command that changes the device's rate can be written just before. |

Nothing waits: `read` and `line` return at once, so poll them from `loop()`
or a task. Up to 2048 bytes are kept between reads. What arrives beyond
that is lost. While the port is open, `gpio` and `adc` can't use its pins,
and a device asleep on battery doesn't go into its light sleep (bytes would
be lost), so close it when it isn't needed.

```lua
uart.open(9600)                                  -- a GPS on Port C
function loop()
  local line = uart.line()
  while line do
    if line:sub(1, 6) == "$GPGGA" then print(line) end
    line = uart.line()
  end
end
```

In the simulator, `--set portc=loopback` wires the port's output to its
input.

## imu - the motion sensor

The accelerometer and gyroscope on the board (a BMI270) and the compass
behind it (a BMM150). CoreS3 and CoreS3 Lite have them. The SE doesn't, and
there every call returns `nil, "no motion sensor on this board"`.

| Function | Description |
|---|---|
| `imu.accel()` -> `x, y, z` | Acceleration in g, up to 8 either way. At rest it is gravity: about 1 on the axis that points up. |
| `imu.gyro()` -> `x, y, z` | Rotation in degrees a second, up to 2000 either way. |
| `imu.mag()` -> `x, y, z` | The magnetic field in microtesla. The earth's is 25 to 65 in all, about 50 in Finland, most of it pointing down. |

The sensor is switched on by the first call, which takes about half a
second, and off again when the script ends. `nil, err` also if it
doesn't answer.

The compass measures every magnet and piece of iron near it along with the
earth, so a heading needs the app to find the middle of what it reads
while the board is turned around (the Sensors app does). A magnet right
next to it is beyond its range: `nil, "the field is too strong for the
compass"`. The CoreS3 Lite's back cover has magnets in it, and with the
cover on that is what a Lite answers.

```lua
function loop()                                  -- a level: where is down?
  local x, y = imu.accel()
  if not x then return end
  disp.clear(disp.BLACK)
  disp.circle(math.floor(160 + x * 100), math.floor(120 + y * 100), 10, disp.GREEN, true)
end
```

## light - ambient light and proximity

The light and proximity sensor above the screen (an LTR-553). CoreS3 and
CoreS3 Lite have one. On the SE both calls return `nil, "no light sensor on
this board"`.

| Function | Description |
|---|---|
| `light.lux()` -> `lux` | Ambient light in lux, roughly: about 300 to 500 in a lit room, tens of thousands in sunlight. It follows the room with half a second's delay. |
| `light.proximity()` -> `0..2047` | How near something is to the sensor: the nearer, the bigger. Not a distance: it depends on the thing's color and size. |

The sensor is switched on by the first call, which takes a quarter of a
second, and off again when the script ends.

```lua
timer.every(1000, function()                     -- dim the screen in the dark
  local lux = light.lux()
  if lux then disp.brightness(lux < 20 and 20 or 80) end
end)
```

In the simulator, `--set accel=X,Y,Z`, `gyro=X,Y,Z`, `mag=X,Y,Z`,
`light=LUX` and `proximity=N` are what the sensors measure (with `--model
cores3`).

## camera - the camera

The camera above the screen (a GC0308), looking the way the screen faces.
CoreS3 and CoreS3 Lite have one. On the SE every call returns `nil, "no
camera on this board"`. API version 2.

| Function | Description |
|---|---|
| `camera.draw([x, y, [scale]])` -> `w, h` | Draw the newest picture with its corner at `x`, `y` (0, 0 if left out). It is 320x240, the screen's size. `scale` 2 or 4 draws it at a half or a quarter of that. Clipped to the screen. |
| `camera.jpeg([quality])` -> `data` | The newest picture as a JPEG (a string), 320x240, to save with `sd.write` or send with `http.post`. `quality` is 1..100, 80 if left out: about 10 KB of a lit room. Takes a few tenths of a second. |
| `camera.mirror([on])` | Mirror the picture left to right from the next one on (`true`, or nothing), or not (`false`, as at a script's start). It takes a moment: two pictures' time. The camera faces whoever looks at the screen: mirrored, they see themselves as in a mirror; not mirrored, as others see them, and text reads right. |
| `camera.stop()` | Switch the camera off before the script ends, when it's done with it: on, it draws a good part of what the whole board does. |

The camera is switched on by the first call, which takes about a second,
and off again when the script ends. A new picture comes about every 40 ms,
and a call waits for one. `nil, err` if the camera doesn't answer or gives
no picture. Its first pictures are dark, until it has settled on the light.

```lua
function loop() camera.draw() end                -- a viewfinder: a tap takes the photo

touch.on("tap", function()
  local photo = camera.jpeg()
  if photo then sd.write("photo.jpg", photo) end
end)
```

In the simulator the camera sees a test picture: color bars, a gradient and
a white square that `--set camera=N` moves (with `--model cores3`).

## sound - the speaker

Tones and WAV clips, queued and played in order while the script goes on:
nothing waits for a sound to end. How loud is the device's Volume setting
(Settings, off included) times each sound's own `volume`, so an app can't
be louder than the user allows. Sounds stop when the script ends. In the
simulator they play through the computer's speakers.

| Function | Description |
|---|---|
| `sound.tone(hz, ms, [volume])` -> `true` or `nil, err` | A tone (a square wave) of 20..20000 Hz, or `hz` 0 for a rest, for 1..10000 ms. `volume` 0..100, default 100. |
| `sound.play(wav, [volume])` -> `true` or `nil, err` | A WAV file's audio (a string: an `asset("ding.wav")` or from `http.get`): PCM, 8 or 16 bit, mono or stereo, 4000..48000 Hz. `nil, "not a WAV file"` and such otherwise. |
| `sound.stop()` | Drop what's queued and end what's playing. |
| `sound.playing()` -> `bool` | Is a sound playing or waiting? |
| `sound.volume()` -> `0..100` | The Volume setting (0: off). |

Up to 64 sounds can wait, with 1 MB of clips together. More is
`nil, "sound queue full"`.

```lua
-- a two-note chime
sound.tone(880, 120)
sound.tone(0, 40)
sound.tone(1320, 200, 80)
```

## ble - Bluetooth LE devices

Scan for Bluetooth LE devices nearby and connect to them as a client: read
their values, write them, and get notified when they change. In the
simulator the devices are simulated ones, plus on macOS the computer's own
Bluetooth devices (see [the simulator's README](../platform/sim/README.md#bluetooth)).
The waiting functions need a `task`.

| Function | Description |
|---|---|
| `ble.scan(fn, [opts])` -> `true` or `nil, err` | Start scanning. `fn(adv)` is called for each device heard, and again whenever it advertises (at most once a tick per device). `opts.active = true` also asks devices for their scan response: more data, more radio. |
| `ble.stop_scan()` | Stop scanning. |
| `ble.connect(addr)` -> `conn` or `nil, err` | Start connecting to the device with address `addr` (`"c7:18:2c:93:8f:cc"`), then `conn:wait_connected()`. At most 3 connections at a time. |
| `ble.uuid(s)` -> `uuid` or `nil` | A UUID in its usual form: `"0x2A19"` and `"00002a19-0000-1000-8000-00805f9b34fb"` are both `"2a19"`. `nil` if it isn't one. |
| `conn:wait_connected([timeout_ms])` -> `true` or `nil, err` | Wait until connected and the device's services are known (default 20 s). A device that doesn't answer gives up after 15 s. |
| `conn:status()` | `"connecting"`, `"connected"` or `"closed"` with a reason: `"connection lost"`, `"disconnected by the device"`, `"cannot connect: timeout"`, `"closed"`. |
| `conn:services()` -> `{ uuid, ... }` | The device's services. |
| `conn:characteristics([svc])` -> `{ c, ... }` | Its characteristics (of service `svc`, or all): `{svc, uuid, read, write, write_nr, notify, indicate}`, the last five true or false. |
| `conn:read(svc, chr)` -> `data` or `nil, err` | Read a value. |
| `conn:write(svc, chr, data)` -> `true` or `nil, err` | Write a value (at most 244 bytes): with a response if the characteristic takes one, else without. |
| `conn:subscribe(svc, chr, fn)` -> `true` or `nil, err` | Turn on notifications (or indications) of a value: `fn(data, chr, svc)` is called with each new value. |
| `conn:unsubscribe(svc, chr)` | Turn them off. |
| `conn:addr()`, `conn:close()` | The address. Close (also done when the connection is garbage-collected or the script ends). A connection with a subscription is kept until it's closed or lost, even if the script holds it nowhere else. |

Services and characteristics are UUIDs: 16-bit ones like `"180f"` or the full
128-bit form, in any case, with or without dashes. Values are byte strings.
`string.byte` and `string.unpack` take them apart. Errors come back as
`nil, err`: `"read 2a19: not readable"`, `"write 2a19: not writable"`, `"read 2a00:
the device has no characteristic 2a00 in service 180f"`.

What `fn(adv)` of `ble.scan` gets:

| Field | |
|---|---|
| `addr` | The address, to connect to. |
| `rssi` | Signal strength in dBm (-40 near, -90 far). |
| `name` | The name it advertises, or `nil`. |
| `mfg` | Manufacturer data: `{ [company ID] = data }`. |
| `svc` | Service data: `{ [uuid] = data }`. |
| `uuids` | The services it advertises: `{ uuid, ... }`. |
| `tx` | Transmit power in dBm, or `nil`. |
| `raw` | The advertising data as received (AD structures). |

A device that refuses a read or write until paired gets paired at once ("just
works", without a PIN) and the request is sent again. The bond is kept for
next time. The bond belongs to the app that made it: another app can't
connect to that device (`"cannot connect: paired with app 'name'"`) until
the app is uninstalled or its data cleared, which deletes its bonds. The
device keeps up to 8 bonds. A new one only replaces an older one of the same
app or of no app. Devices that want a PIN typed in aren't supported. Bluetooth shares
the radio with Wi-Fi: a scan listens about a third of the time, and pauses
while connecting.

```lua
-- Battery level of a device with the standard battery service
task(function()
  local c = assert(ble.connect("c7:18:2c:93:8f:cc"))
  assert(c:wait_connected())
  print("battery", assert(c:read("180f", "2a19")):byte(), "%")
  c:subscribe("180f", "2a19", function(d) print("now", d:byte(), "%") end)
end)                          -- the subscription keeps the connection and the script going
```

The [di2 app](https://github.com/veikkos/luaboard-marketplace/blob/main/di2/main.lua) finds a Shimano Di2 derailleur by
scanning and shows its gear position and battery.

## timer - callbacks

| Function | Description |
|---|---|
| `timer.after(ms, fn)` -> `id` | Call `fn()` once after `ms`. |
| `timer.every(ms, fn)` -> `id` | Call `fn()` every `ms` (>= 10). |
| `timer.cancel(id)` | Cancel a timer. `nil`, or the id of a timer that has already run, does nothing. |

Timers are serviced at the loop rate (`sys.rate`), so their resolution is one
loop period (33 ms at the default 30 Hz).

## sys - system

| Function | Description |
|---|---|
| `sys.millis()` -> `ms` | Milliseconds since boot. |
| `sys.delay(ms)` | Block for 0..1000 ms. Prefer timers: the USB link is not serviced while blocked. |
| `sys.rate([hz])` -> `hz` | Get or set the `loop()` rate. Clamped to 1..100 Hz. Returns the effective rate (1000 / whole ms period, so 60 gives 62). |
| `sys.heap()` -> `used, peak, limit` | Lua heap usage in bytes. |
| `sys.battery()` -> `percent, charging, usb` | Battery level (0..100), whether it's charging, and whether USB power is present. CoreS3 and Lite have one. The SE has none (it runs on USB-C power) unless a battery module is added. Without one, `percent` is `nil` and `charging` is `false`. |
| `sys.port_power([on])` -> `on` | 5V out to the Grove ports (A, B, C). Off until the script takes a pin (`i2c`, `uart`, `gpio`, `adc`), which turns it on, and off again when the script ends: making it draws from the battery. Set by hand it stays as set: `false` cuts it, e.g. to save battery between readings or to restart a stuck unit, and `true` has it on before any pin is taken. |
| `sys.info()` -> `table` | `{fw, version, board, model, lua, lua_api, script, app, boot, id}`: `lua_api` is the [API version](#api-version) the firmware has. `model` is `"cores3"` (CoreS3 or Lite) or `"cores3se"`. `script` is `"<app>/<file>"` (`""` when no script is running, e.g. `EVAL` after `STOP`). `boot` says why the device started: `"power"` (the power key, USB plugged in), `"timer"` (the wake-up time of `sys.off`) or `"reset"` (a restart, an update, a crash). `id` is the board's own: 12 hex digits, the same after restarts and updates, different on every board (made from its factory MAC address, but not the address itself). For telling boards apart, such as an MQTT client id. |
| `sys.exit()` | Leave the app, back to the home menu. It ends the script at once: nothing after it runs, and a `pcall` around it doesn't keep the app going. |
| `sys.keep_on([on])` -> `on` | While on, the device's Turn off after setting doesn't turn the device off, for an app that works in the dark (a running countdown, a logger). Off again when the script ends. Without an argument, returns the current state. |
| `sys.off([seconds])` | Turn the device off: the app is stopped at once, as by `sys.exit()` (what it keeps in `store` is written), and the board powers down. With `seconds` (10 to 27 days' worth) the device also turns itself on in time for the app to be running by then. It comes early, never late, by up to a minute or two for waits over 255 s, so the app waits out the rest itself. A wait over 255 minutes needs the time to be known (`sys.time()`). A time it can't honor is an error. See below. |
| `sys.time()` -> `seconds` or `nil` | The time: UTC seconds since 1970, or `nil` while the time isn't known. At boot the time comes from the board's RTC chip if it kept it (it runs while the board has power: a reset or a crash, or with a battery also while off), otherwise from the network (NTP) a second or two after joining Wi-Fi. |
| `sys.time(text, [zone])` -> `seconds` or `nil, err` | A date and time as web APIs send them, in seconds: `"2026-11-15T10:30:00Z"`, `"2026-11-15T12:30:00.250+02:00"`, `"2026-11-15 12:30"`, `"2026-11-15"` (midnight), `"Sun, 15 Nov 2026 10:30:00 GMT"`. The last form is also read as RSS feeds write it: any case, the month's name in full, a two-digit year (`00`..`49` is 20xx), a zone as a name (`UT`, `EST`, `PDT`...) or a number (`+0200`). Without an offset it's a time in `zone`. Fractions of a second are dropped. |
| `sys.time(fields, [zone])` -> `seconds` | The other way from `sys.date`: `{year, month, day, [hour], [min], [sec]}` in `zone`, the times left out 0. A field out of range carries over, as in `os.time` (`month = 13` is January, `day = 0` the last of the month before). A time skipped when the clocks go forward counts as before the change (02:30 is 03:30), one repeated when they go back is the first. |
| `sys.date([t], [zone])` -> `table` or `nil` | `t` (default: now, but `nil` if the time isn't known yet) in `zone`: `{year, month, day, hour, min, sec, wday, yday, isdst, offset, zone}`. `wday` 1 = Sunday (like `os.date`), `offset` = seconds east of UTC, `zone` e.g. `"EET"`/`"EEST"`. |
| `sys.date(format, [t], [zone])` -> `text` or `nil` | The same time as text, as `os.date` formats it, in English: `%Y` year, `%y` its last two digits, `%m` month, `%d` day, `%H` hour, `%I` hour 1..12, `%M` minute, `%S` second, `%j` day of the year, `%p` AM/PM, `%a` Mon, `%A` Monday, `%b` Nov, `%B` November, `%Z` the zone (EET), `%z` its offset (+0200), `%%` a %. Numbers have leading zeros; `%-d` (and so on) leaves them out. `sys.date("%a %-d %b %H:%M")` is `"Sun 15 Nov 12:30"`. |
| `sys.timezone()` -> `string` | The device's time zone as a POSIX TZ string, e.g. `"EET-2EEST,M3.5.0/3,M10.5.0/4"` (Finland, the default). Set with `luaboard tz`. |

`zone` is the device's time zone when left out, or a POSIX TZ string:
`"UTC"`, `"EST5EDT,M3.2.0,M11.1.0"` (New York), `"CET-1CEST,M3.5.0,M10.5.0/3"`
(Berlin). An app that follows one place's time, whatever the device's
zone, passes that place's string. Times go from year 0 to 9999.

```lua
local t = assert(sys.time("2026-11-15T10:30:00Z"))   -- from an API
local ny = "EST5EDT,M3.2.0,M11.1.0"
print(sys.date(t, ny).hour)                          -- 5
print(sys.date("%H:%M %Z", t, ny))                   -- 05:30 EST
local open = sys.time({ year = 2026, month = 11, day = 16, hour = 9, min = 30 }, ny)
```

When the device comes on again after `sys.off`, by the power key or by
itself, it starts the app that turned it off, once. That needs an installed
app: code that was only uploaded is gone after a power-off, and the device
starts in its menu. On USB power the board isn't powered down, so the PC
link stays, and the same script starts again from its beginning, uploaded
code included. `sys.info().boot` says why the device started. What the user
sees is in [Asleep and off](DEVICE.md#asleep-and-off).

## Tasks: waiting without blocking

`task(fn, ...)` runs `fn(...)` as a coroutine next to `loop()` and the timers.
Inside a task:

| Function | Description |
|---|---|
| `sleep([ms])` | Pause this task (default 0 ms). Everything else keeps running. |
| `yield()` | Let everything else run, continue on the next tick. |
| `in_task()` -> `bool` | Are we inside a task? |

This is how network code is written top to bottom without stalling the
device. `sleep` and `yield` outside a task are an error. A coroutine of the
app's own running inside a task (a generator that fetches pages, say) can
call them, and so the network functions, too: the task waits with it, and
the coroutine goes on where it was. The 1 s watchdog
covers each tick, not the whole task: a slice between `sleep`/`yield` must
fit in it with the timers, other tasks and `loop()` of that tick. An error
inside a task stops the script like any other error.

```lua
task(function()
  while true do
    print("tick", sys.millis())
    sleep(1000)
  end
end)
```

## net - TCP sockets

Non-blocking TCP client and server (Wi-Fi on the device, the computer's network
in the simulator). Without Wi-Fi a device on USB can use the computer's network
(`luaboard tether`): connections then go through the computer, TLS still
checked on the device, and `net.listen` takes none. The waiting helpers need a `task`.

| Function | Description |
|---|---|
| `net.wifi()` -> `state, [ip]` | The device's Wi-Fi: `"off"` (no network set up), `"connecting"` (joining, or rejoining after losing it) or `"connected"`, with the IP address. In the simulator, the simulated access point's state. |
| `net.connect(host, port, [opts])` -> `socket` or `nil, err` | Start connecting (host name or IPv4 address). Then `s:wait_connected()`. A connection opened while Wi-Fi is still joining waits for it (an app started at boot runs a few seconds before the network is there), and so do `http` and `mqtt`, which are built on this. The device's own control port (3333) refuses connections from scripts. `opts.tls = true`: TLS (HTTPS, MQTTS, ...) with the server's certificate checked against the built-in root certificates. `opts.ca = pem` trusts that CA instead (your own server) and implies `tls`. |
| `net.listen(port)` -> `server` or `nil, err` | Accept incoming connections: `srv:accept()` -> `socket` or `nil`. Port 3333 is the device's own. |
| `s:wait_connected([timeout_ms])` -> `true` or `nil, err` | Wait until connected (default 5 s). Closes the socket on timeout. |
| `s:send(data)` -> `n` | Bytes accepted right now (may be fewer or 0, and 0 while connecting). `nil, "closed"` (or a TLS error) at the end. |
| `s:recv([max])` -> `data` | What has arrived, up to `max` bytes (default 1024, at most 16384): `""` if nothing yet, `nil, "closed"` at the end. |
| `s:write_all(data)` -> `true` or `nil, err` | Send everything (waits inside a task, but outside one it gives up after about 50 ms with a full buffer). |
| `s:reader()` -> `r` | Buffered reading, inside a task: `r:read(n, [timeout_ms])` (exactly n bytes), `r:line([timeout_ms], [max])` (without the `\n` or `\r\n`; a longer line than `max` bytes, default 16 KiB, is `nil, "line too long ..."`), `r:chunk([timeout_ms])` (whatever has arrived). `nil, "closed"` at the end, `nil, "timeout"` once `timeout_ms` has passed since the call, even while data keeps trickling in. |
| `s:status()` | `"connecting"`, `"connected"`, `"listening"` or `"closed"` (+ error). |
| `s:peer()`, `s:close()` | `"host:port"`. Close (also done when the socket is garbage-collected or the script ends). |

At most 16 sockets at a time, 4 of them TLS (each needs about 40 KB, in PSRAM
on the device). Opening more is `nil, "too many open sockets (max 16)"` or
`nil, "too many TLS connections (max 4)"`. A host name is looked up in the
background, so `connect` returns at once and the socket says `"connecting"`
meanwhile. A name that doesn't exist ends as
`"closed", "cannot resolve <host>"`. A TLS socket reports `"connecting"` until
its handshake is done. The first one to a server takes about a second on the
chip, so keep connections open (`http` does). Certificate problems come back
as `nil, "TLS: certificate not trusted (...)"`.

```lua
local srv = assert(net.listen(8080))          -- a tiny inbound server
task(function()
  while true do
    local c = srv:accept()
    if c then c:write_all("knob=" .. adc.read(adc.PORTB_IN) .. "\n") c:close() end
    yield()
  end
end)
```

## mqtt - MQTT 3.1.1 client (built in)

`local mqtt = require("mqtt")`. Written in Lua on top of `net`. Upload your
own `mqtt.lua` to replace it.

| Function | Description |
|---|---|
| `mqtt.connect(url, [opts])` -> `client` or `nil, err` | `url` is `mqtt://host[:port]`, `mqtts://host[:port]` (TLS) or `host[:port]` (default port 1883, 8883 with TLS, or `opts.port`). Inside a task. Options below. |
| `c:publish(topic, payload, [opts])` -> `true` / `true, "queued"` / `nil, err` | `opts.qos` 0 (default) or 1, `opts.retain`. While disconnected the message is queued. QoS 1 waits for the broker's confirmation inside a task (`opts.timeout`, default 5000 ms). |
| `c:subscribe(filter, fn, [qos])` -> `true` / `true, "queued"` / `nil, err` | `fn(topic, message)` for matching messages (`+` and `#` wildcards). `qos` 0 (default) or 1. Kept across reconnects. Inside a task it waits for the broker's answer (`opts.timeout`): `nil, "refused by the broker"` (the broker doesn't allow the filter) or `nil, "timeout"`. A subscription refused later, when it is sent again after a reconnect, is dropped. |
| `c:unsubscribe(filter)` -> `true` or `nil, "timeout"` | Stop a filter's messages: every callback `subscribe` gave it. Inside a task it waits for the broker's answer. |
| `c:close()` | Disconnect cleanly and stop reconnecting. The broker does not publish the will. |
| `c:queued()`, `c.dropped` | Messages waiting in the offline queue. Messages dropped because it was full. |
| `c.connected` | Current state. |
| `c.on_connect = function(c) end` | Called after every connection, e.g. to publish `online`. Give it as `opts.on_connect` to have it called for the first one too (when set on the client, it isn't there yet for the first). |
| `c.on_close = function(err) end` | Called when the connection is lost (it will reconnect), and by `c:close()`. Can also be given as `opts.on_close`. |
| `mqtt.matches(filter, topic)` -> `bool` | Topic filter matching, as used for subscriptions. |

Options for `mqtt.connect`:

| Option | Default | |
|---|---|---|
| `client_id` | `luaboard-<id>` | `id` is `sys.info().id`, so two boards running the same app don't push each other off the broker. |
| `port` | 1883, 8883 with TLS | Used when `url` has no port. |
| `tls` | `false` | TLS, as `mqtts://` does. The broker's certificate is checked against the root certificate bundle. |
| `ca` | none | PEM text of a CA to trust instead of the bundle (a broker with its own certificate). Implies `tls`. |
| `username`, `password` | none | |
| `keepalive` | 60 s | A broker silent for 1.5× this is treated as gone (e.g. Wi-Fi dropped). 0 turns it off: no pings, and a dead connection goes unnoticed until a write fails. |
| `will` | none | `{ topic, payload, qos, retain }`: published by the broker if the device disappears without `close()` (power cut, crash, lost Wi-Fi). |
| `reconnect` | `true` | Reconnect after 1, 2, 4 … 30 s, then re-subscribe and send the queue. The first connection is not retried: `mqtt.connect` returns `nil, err`. |
| `queue` | 100 | Max messages kept while offline (oldest dropped first). 0 = don't queue. |
| `max_size` | 64 KiB | The biggest packet taken from the broker. A bigger one drops the connection (then reconnects). |
| `clean` | `true` | Clean session. |
| `timeout` | 5000 ms | For connecting and the broker's answers (to CONNECT, SUBSCRIBE, UNSUBSCRIBE). |
| `on_connect`, `on_close` | none | As `c.on_connect` and `c.on_close`, set before the first connection. |

Delivery: queued messages are sent in order after reconnecting. QoS 1
messages that weren't confirmed when the connection dropped are sent again, so
the backend can occasionally get a duplicate (at-least-once). The queue lives in
RAM, so a script restart or reboot loses it.

Errors come back as values, e.g. `nil, "broker refused: bad user name or
password"` or `nil, "cannot connect to 192.168.1.10:1883: Connection refused"`.
See `marketplace/ha_env` for a complete app. For testing with the simulator,
run a broker on the computer, e.g. Mosquitto (`mosquitto -p 1883`).

```lua
local mqtt = require("mqtt")
task(function()
  local c = assert(mqtt.connect("mqtt://192.168.1.10", {
    client_id = "weather-1",
    will = { topic = "weather-1/status", payload = "offline", retain = true },
    on_connect = function(c) c:publish("weather-1/status", "online", { retain = true }) end,
  }))
  c:subscribe("weather-1/cmd/#", function(topic, msg) print(topic, msg) end)
  while true do
    c:publish("weather-1/knob", adc.read(adc.PORTB_IN), { qos = 1 })   -- queued while offline
    sleep(10000)
  end
end)
```

## http - HTTP/1.1 client (built in)

`local http = require("http")`, inside a `task`:

| Function | Description |
|---|---|
| `http.request{ method, url, headers, body, timeout, max_size, ca, redirects, private_headers }` -> `status, body, headers` | `nil, err` if the request couldn't be made. `method` defaults to `GET`. A string argument is taken as `url`. `https://` uses TLS (`ca` as in `net.connect`). Header names in the result are lowercase. Repeated headers are joined with `", "`. `timeout` in ms, default 15000 (the whole request, redirects included). Maximum body 256 KB. `redirects`: how many to follow, default 5 (below). |
| `http.get(url, [headers], [opts])`, `http.post(url, body, [headers], [opts])` | Shortcuts. `opts` takes the other `request` fields. |
| `http.form(t)` / `http.parse_query(q)` | `{ a = 1 }` <-> `"a=1"` (URL-encoded, and `form` sorts the keys). |
| `http.urlencode(s)`, `http.urldecode(s)` | |
| `http.parse_url(url)` -> `scheme, host, port, path` | |
| `http.close_idle()` | Close kept connections (also done when the script ends). |

Connections are kept open and reused per server, so polling an API every few
seconds doesn't pay for a new TLS handshake each time. When the device is
out of sockets, the connection kept longest is closed to make room. A kept
connection the server has closed isn't used. If one breaks under a request
anyway, a `GET` (or `HEAD`, `PUT`, `DELETE`) is sent again on a new
connection, but a `POST` or `PATCH` isn't: it may have arrived, so the
result is `nil, "closed"`.

Redirects (301, 302, 303, 307, 308 with a `Location`) are followed, and
the result is the last response's. A 303, and a 301 or 302 after a
`POST`, goes on as a `GET` without the body, as browsers do. A redirect to
another server goes without the `Authorization` and `Cookie` headers, and
without those named in `private_headers` (a list, such as `{ "X-Api-Key" }`
for a service that takes its key in a header of its own).
More redirects than `redirects` is `nil, "too many redirects"`, and
`redirects = 0` returns the 3xx response itself.

```lua
local http, json = require("http"), require("json")
task(function()
  local st, body = http.get("https://api.example.com/weather?city=Helsinki")
  local t = st == 200 and json.decode(body)
  if t then print(t.temp) end
end)
```

## json (built in)

`local json = require("json")`: `json.decode(text)` -> `value` or
`nil, err`, `json.encode(value)` -> `text`. JSON `null` is `json.null`, so
`nil` always means the text wasn't JSON, and `err` says the line and
position. An empty table encodes as `{}` unless marked with `json.array(t)`
(decoded empty arrays are marked). Encoding what JSON can't hold (a
function, a table that contains itself, NaN) is an error.

Decoding a big text (a few hundred KB) can take longer than the watchdog
allows on the board. Inside a task, `json.decode` waits for the next tick
when the time runs low, so decode big bodies in a task (as `http` calls are
anyway). Other tasks and timers may run while it waits.

## secrets - tokens and API keys

Small named values kept in flash, apart from the scripts.

- Each secret belongs to an app. A script reaches only its own app's
  secrets. Another app can't read, change or list them, and two apps can use
  the same name without colliding.
- They go with the app. Uninstalling an app deletes its secrets, and so
  does Clear data in its options (hold B on it in the menu). Reinstalling
  an app keeps them.
- The PC tools see only names. They can set, list (names only) and delete
  secrets, never read them back: `luaboard secrets`,
  `luaboard secrets set <app> <name> <value>`, `luaboard secrets rm <app>
  [<name>]`. Without a name, `rm` deletes all of the app's secrets.
- Not a vault. The isolation keeps apps away from each other's tokens. It
  doesn't stop someone who can upload a script into that app: anyone with the
  USB cable or a paired computer ([SECURITY.md](../SECURITY.md)).

| Function | Description |
|---|---|
| `secrets.get(name)` -> `value` or `nil` | |
| `secrets.set(name, value)` / `secrets.set(name, nil)` | Store / delete. Names: 1..15 characters `A-Z a-z 0-9 _ . -`, not starting with `_`. Values up to 2048 bytes. An app's secrets up to 8 KB together, all apps' 16 KB, and 32 secrets on the device in all. Breaking a limit is an error. |
| `secrets.keys()` -> `{ name, ... }` | Sorted. |

A project can keep its own values (API keys, client IDs) out of its scripts
in a `secrets.env` file next to them, `NAME=value` per line (`#` comments).
Install and upload (CLI and studio) store each one as a secret of that app
before the scripts. The file itself never goes to the device. `secrets.env` is
gitignored: commit a `secrets.env.example` instead.

```lua
local CLIENT_ID = secrets.get("spotify_client")  -- secrets.env: spotify_client=...
```

They survive reboots and firmware updates, but `luaboard format` wipes them.

## store - values an app keeps

Small values an app keeps across runs, restarts and firmware updates:
settings, a chosen room, a counter. Each app has its own. Other apps can't
see them. Unlike secrets they are meant for ordinary data, and there's room
for more (64 KB per app).

| Function | Description |
|---|---|
| `store.get(key)` -> `value` or `nil` | |
| `store.set(key, value)` / `store.set(key, nil)` | Store / delete. Values: strings, numbers (integer or float, kept as such) and booleans. `json.encode` a table. Keys: 1..32 characters `A-Z a-z 0-9 _ . -`. |
| `store.keys()` -> `{ key, ... }` | Sorted. |
| `store.clear()` | Delete all of this app's values. |

Values go to flash at most once a second: a change soon after another one is
written a second later (or when the script ends), so a power cut can lose
the last second of changes.

Uninstalling the app or Clear data in its options deletes them, and so do
`luaboard format` and `luaboard erase`.

```lua
local room = store.get("room") or "Living room"
-- ... the user picks another one:
store.set("room", "Kitchen")
```

## sd - files on the microSD card

Files on the card in the slot, for logs, settings and data too big for
`store`. The card has to be FAT formatted: FAT16 or FAT32. Cards over 32 GB
usually come formatted as exFAT, which the device doesn't read. Format such
a card as FAT32 on a computer first (on a Mac: Disk Utility, "MS-DOS
(FAT)"). The device never formats a card.

| Function | Description |
|---|---|
| `sd.info()` -> `total, free` | The card's size and free space in bytes. |
| `sd.list([dir])` -> `{ { name, size, dir }, ... }` | What's in a folder (the card's top one without `dir`), sorted by name: `size` in bytes, `dir` true for a folder. At most 512 entries. |
| `sd.exists(path)` -> `"file"`, `"dir"` or `false` | Is it there, and what is it. |
| `sd.size(path)` -> `bytes` | A file's size. |
| `sd.read(path, [offset], [n])` -> `data` | Up to `n` bytes (1..65536, the default) from `offset` (default 0): a small file whole, a big one in parts. `""` at or past the end. |
| `sd.write(path, data)` -> `true` | Make the file, or replace what it had. |
| `sd.append(path, data)` -> `true` | Add to the end of the file (made if it isn't there). |
| `sd.mkdir(path)` -> `true` | Make a folder (its parent has to be there). |
| `sd.remove(path)` -> `true` | Delete a file, or an empty folder. |

Every one returns `nil, err` when it can't: `"no card"`, `"the card can't
be read as FAT (it has to be FAT16 or FAT32)"`, `"no such file or folder"`,
`"the card is full"` and so on. Paths are from the card's top folder, with
`/` between folders, up to 200 bytes. A path with `..`, with characters FAT
can't hold (`\ : * ? " < > |`) or with a name ending in a dot or a space is
an error. Names can be long and are UTF-8 (`sd.list` leaves out names of
128 bytes or more).

There are no open files: each call opens, does its work and closes, so
what `write` and `append` return true for is on the card, and the card can
be pulled out between calls. A card put in later is found by the next call
(an empty slot is looked at again after 2 seconds). Each call blocks while
it runs, a few milliseconds for a small read or write on most cards (the
first `sd.info()` on a big card can take seconds). The display can't be
drawn to meanwhile, as the card shares its wires.

```lua
timer.every(60000, function()                    -- a line a minute
  local t = sys.date()
  local ok, err = sd.append("log.csv", string.format("%02d:%02d,%d\n", t.hour, t.min, adc.read(adc.PORTB_IN)))
  if not ok then print("log: " .. err) end
end)
```

In the simulator, `--set sd=FOLDER` puts a card in whose files are that
folder's. `sd=unformatted` is a card that isn't FAT, `sd=none` an empty
slot (the default).

## crypto

| Function | Description |
|---|---|
| `crypto.sha256(data)`, `crypto.hmac_sha256(key, data)` | 32 raw bytes. |
| `crypto.random(n)` | `n` bytes (0..4096) from the hardware random number generator. |
| `crypto.hex(data)` | Lowercase hex. |
| `crypto.base64(data, [url])`, `crypto.unbase64(text)` | `url = true`: URL-safe alphabet, no padding. `unbase64` takes either alphabet, ignores padding and whitespace, and returns `nil` on other characters. |

## Logging in to web services (browser login, OAuth)

Many web APIs (Spotify, Google, GitHub, ...) need the user to log in through
a web page. The device can't show one, so it asks the computer:
`browser.login(url, redirect, [title], [timeout_ms])` (inside a task) makes
luaboard studio show "Log in: <title>", open `url` in the browser and catch
the redirect to `redirect`, which must be `http://127.0.0.1:<port>/...` or
`http://localhost:<port>/...`. `url` must be `http(s)://`, under 1024
characters, without spaces. A bad `url` or `redirect` is an error. It returns
the redirect's query string, or `nil, err` if the user declined or
`timeout_ms` (default 10 minutes) passed. One login at a time: a new one
cancels the pending one. Without the studio: `luaboard browser`. The prompt
names the app that asks (the firmware fills that in, not the script), so check
it: whatever the login page grants goes to that app.

`oauth.lua` (built in) does the whole OAuth 2.0 login with PKCE on top of
that, keeps the refresh token in a secret and refreshes the access by itself:

```lua
local oauth = require("oauth")
local login = oauth.new{
  name = "myservice", title = "My Service", client_id = "...", scope = "read",
  authorize_url = "https://auth.example.com/authorize",
  token_url = "https://auth.example.com/token",
  redirect = "http://127.0.0.1:8888/callback",   -- registered with the service
  on_login = function(waiting) end,              -- true: tell the user to log in
}
task(function()
  local st, body = login:request{ url = "https://api.example.com/me" }
end)
```

`name` is the secret that holds the refresh token. `name`, `client_id`,
`authorize_url`, `token_url` and `redirect` are required.

Optional fields of `oauth.new`:

- `title` (default `name`), `scope`, `on_login`.
- `login_timeout` is passed to `browser.login` as `timeout_ms`.
- `client_secret` is sent with every token request. Google's "Desktop app"
  clients need it even with PKCE, and Google itself says it isn't really
  secret for installed apps.
- `params` adds extra parameters to the login URL. Google needs
  `params = { access_type = "offline", prompt = "consent" }` to hand out a
  refresh token.

| Method | Description |
|---|---|
| `login:request{...}` | `http.request` with the access token. A 401 refreshes once. Returns `status, body, headers` or `nil, err`. |
| `login:token()` -> `token` or `nil, err` | A valid access token (logs in or refreshes when needed). Inside a task. |
| `login:logged_in()`, `login:logout()` | `logged_in`: is a login stored. `logout`: forget it. |

Re-login: if the service refuses the stored login (access removed, token
revoked or expired), the script forgets it and the next request asks for a
new browser login, calling `on_login(true)`. Network errors keep the login.
Log out on purpose with `login:logout()` or `luaboard secrets rm <app> <name>`.
The studio and CLI only pass on a one-time code that is useless without a
secret that never leaves the device. They never see passwords or tokens.

`marketplace/spotify` shows a whole web API client on top of `http`, `json` and
`oauth`.

## Performance notes

A full-screen redraw is 150 KB over SPI (about 30 ms at 40 MHz), so clearing
and redrawing everything each frame tops out at roughly 25-30 fps. For
smoother animation, redraw only what changes, e.g. `disp.text(..., bg)` over
the old value instead of `disp.clear()`.

On battery, a device that is asleep (the screen off) saves power by letting
the chip sleep between the things an app does. Timers and tasks cost the
least: the chip sleeps until the next one is due. A `loop()` keeps its rate
while asleep, so an app that only needs it for drawing can lower
`sys.rate()` while `disp.sleep()` is true.
