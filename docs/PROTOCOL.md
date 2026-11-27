# Wire protocol

The same protocol runs over two links at the same time:

* USB: the ESP32-S3's built-in USB-Serial/JTAG port (USB CDC) on the
  CoreS3's USB-C connector, `/dev/cu.usbmodem*` on macOS. There's no
  separate USB-UART chip, and the baud rate setting is ignored. In the
  simulator the same bytes go over a pseudo-terminal.
* Wi-Fi: TCP port 3333 once the device has joined a network (see `WIFI`),
  addressed as `tcp://host:port`. The device announces itself with mDNS as
  `luaboard.local`, service `_luaboard._tcp`, with TXT `board`, `fw`, `api`
  (the Lua API version) and `auth=key|none` (paired or not, as of when it was
  announced). The link is encrypted and needs pairing, see
  [Wi-Fi link](#wi-fi-link). USB is trusted (physical access) and never needs
  it.

Replies go to the link that sent the command. `LOG` and `EVT` lines go to
all links that may use the device, so a Wi-Fi client sees script output and
errors even when commands come in over USB.

The protocol is plain text and line based, so over USB you can also use it by
hand from a terminal (`screen /dev/cu.usbmodemXXXX` or `screen /tmp/esp32-sim`).
Over Wi-Fi it is wrapped in encrypted frames, so a terminal won't do there.

## Framing

* Host → device: one command per line, terminated by `\n` (`\r` is ignored).
  Maximum line length is 2047 bytes, and a longer line gets `ERR line too
  long (max 2047)`. Command names are case-insensitive, and so are the words
  after `WIFI`, `PAIR`, `TIME`, `TOUCH`, `KEY` and `TUN`. Everything else is
  case-sensitive, the subcommands of `SECRET` and the `CANCEL` of `BROWSER`
  included.
* Device → host: lines terminated by `\n`. Every line starts with a tag:

| Tag | Meaning |
|---|---|
| `OK [text]` | Final reply: command succeeded. |
| `ERR <message>` | Final reply: command failed. |
| `FILE <app>/<file> <size> <crc32hex>` | Data line of an `LS` reply (comes before the `OK`). |
| `APP <name> <files> <bytes> <state>` | Data line of an `APPS` reply. |
| `SECRET <app> <name> <bytes>` | Data line of a `SECRETS` reply. |
| `LOG <text>` | Asynchronous: output of `print()` in a script (one line per line of output), and device notices about a script that draws nothing. |
| `EVT <event> [details]` | Asynchronous: device state change. |
| `TUN <frame>` | Asynchronous: a connection of the [USB tether](#usb-tether). |
| anything else | Not part of the protocol (e.g. ESP32 ROM boot messages on real hardware). Ignore it. |

Each command gets exactly one `OK` or `ERR` reply (the computer's `TUN`
frames aren't commands and get none). `LOG`/`EVT`/`TUN` lines can appear at
any time, including between a command and its reply (for example
`print()` output during `RUN` or `EVAL`).

Messages never contain newlines: the device replaces them with spaces. The
raw payloads after `GET` and `FB` replies are the only non-line data.

## Commands

| Command | Reply | Notes |
|---|---|---|
| `PING` | `OK PONG` | Use to sync after opening the port. |
| `INFO` | `OK fw=.. ver=.. board=.. model=cores3\|cores3se lua=.. api=N apps=N flash=used/total ram=used/cap heap=used/limit running=<app/file or -> autorun=<app or -> ip=<addr or -> auth=key\|none time=<ISO 8601 or -> target=esp32s3\|sim slot=ota_0\|ota_1 ota=ok\|rolledback screen=on\|off reset=poweron\|sw\|panic\|task-wdt\|brownout\|... update=<version, manual or -> app_updates=N mem=free/low awake=<minutes or ->` | `flash`: the apps partition, `ram`: uploads not installed yet. `ver`: the build's `git describe`. `api`: the [Lua API version](LUA_API.md#api-version). `target`: what firmware images it takes (`sim`: the simulator). `slot` and `ota`: see [Firmware update](#firmware-update-ota). `screen`: off while the screen sleeps. `reset`: why the chip last started (`panic` and the `-wdt` ones are crashes, `brownout` a power dip). `update`: the newer release the device has found (such as `v1.1.0`), `-` if none, see [Release manifest](#release-manifest). `app_updates`: installed apps the [marketplace](https://github.com/veikkos/luaboard-marketplace) has a newer version of. `mem`: the chip's internal RAM in bytes, free now and the least there has been free since it started (`0/0` in the simulator). The radios, the display's and speaker's buffers and the chip's AES and SHA can use no other memory, and TLS fails when it runs out. `awake`: the minutes left of an `AWAKE`. |
| `LS` | `FILE <app>/<file> <size> <crc32hex>` lines, then `OK <count>` | Every file as apps see it (an upload in RAM shadows the installed copy). |
| `CRASH` | `FRAME <call>` lines, then `OK ver=<version> frames=N cause=<text>`, or `OK none` | The last crash, if the last reset was one: its cause, the firmware that crashed, and its stack trace, the innermost call first. On the board a call is an address (`0x4200a1b4`, for `addr2line` with that build's ELF), and in the simulator a function name (`draw_footer+0xe0`). |
| `APPS` | `APP <name> <files> <bytes> installed\|ram\|modified` lines, then `OK <count>` | `ram`: uploaded only, gone after a restart. `modified`: installed, with newer uploads in RAM. |
| `INSTALL <app>` | `OK INSTALLED <app>` + `EVT installed <app>` | Writes the app's uploads to flash (LittleFS). Files already installed and not uploaded again stay. If the app is on the screen (running, or on its error screen), it's started again with the new code (`EVT started`). |
| `UNINSTALL <app>` | `OK UNINSTALLED <app>` + `EVT uninstalled <app>` | Deletes it from RAM and flash and deletes its secrets, stored values and Bluetooth bonds (and stops it if running, clears it as autostart). Also works for a name with only secrets or stored values left. |
| `APPINSTALL <app>` | `OK APPINSTALL <app>`, later `EVT app install <app> ok` or `EVT app install <app> failed <why>` | Installs or updates the app from the [marketplace](https://github.com/veikkos/luaboard-marketplace) index the device last fetched: only files that differ are downloaded, each checked against its size and sha256, then the new version replaces the old at once. A failure leaves the installed version, and `<why>` is the whole reason (the menu shows a shorter one). Its secrets and stored values stay. `ERR` if an install is under way, no index is known yet, the app isn't in it, it is running, or there's no Wi-Fi. |
| `PUT <app>/<file> <size> <crc32hex>` | `OK READY`, then after the payload `OK STORED <name> <size>` | Binary upload to RAM, see below. |
| `OTA <size> <sha256hex>` | `OK READY`, then after the image `OK UPDATED <version>` | Firmware update over this link, see [below](#firmware-update-ota). The device reboots into it. |
| `GET <name>` | `OK <size> <crc32hex>` followed by exactly `<size>` raw bytes | Name resolved like `RUN`. `ERR no such file: <name>`. |
| `FB [<crc32hex>]` | `OK <w> <h> rgb565 <bytes> <crc32hex>` followed by exactly `<bytes>` raw bytes, or `OK SAME` | Display framebuffer: `w*h` pixels, row-major, RGB565 little-endian (320×240 = 153600 bytes on the CoreS3). Used for the live preview. With the CRC of the frame the client has, `OK SAME` (no pixels) if the screen hasn't changed. |
| `TOUCH <x> <y>` | `OK TOUCH <x> <y>` | A finger from the PC, down (or moved) at `x` 0..319, `y` 0..279 of the touch area: `y` 240 and up is the A/B/C strip under the screen (A `x` < 130, B 130..199, C 200 and up). See [Remote touch](#remote-touch). |
| `TOUCH UP` | `OK TOUCH UP` | That finger is let go. |
| `KEY POWER` | `OK KEY POWER` | A short press of the power key: back to the home menu. |
| `AWAKE [<minutes>\|off]` | `OK AWAKE <minutes left>\|off` | Stay awake for 1 to 1440 minutes from now, as if touched all the while: the screen comes on and stays on, the device doesn't turn itself off, and the chip doesn't go into light sleep. `off` ends it; without an argument only the reply. For working on a device from a computer, whose commands don't count as a touch. Not stored: it ends with a restart, and the settings count from its end. |
| `RM <name>` | `OK REMOVED <app>/<file>` | Deletes the file from RAM and flash. Name resolved like `RUN`, and must name a file (`<app>/<file>` or `<file>.lua`), not a bare app. Otherwise `ERR no such file: <name>`. |
| `RUN [name]` | `OK RUNNING <app>/<file>` + `EVT started <app>/<file>` | `<app>` runs its `main.lua`, `<app>/<file>` a given script. A bare `<file>.lua` is in the app last uploaded to or run. No name = `main.lua` of that app. Starts a fresh Lua VM. Load, syntax, top-level and `setup()` errors come back as `ERR` (and show on the error screen). |
| `STOP` | `OK STOPPED` (+ `EVT stopped` if a script was running) | The device goes back to its home menu. The VM is kept, so `EVAL` still sees its globals. |
| `SAVE` | `OK SAVED <n> apps` | `INSTALL` for every app with uploads in RAM (restarting the one on the screen if it changed). |
| `FORMAT` | `OK FORMATTED` | Stops the script, deletes all apps in RAM and flash, the autostart setting, all secrets and Bluetooth bonds. |
| `TIME` | `OK synced=0\|1 utc=<seconds or -> local=<ISO 8601 or -> tz=<POSIX TZ> source=ntp\|rtc\|usb\|- last_sync=<utc or -> syncs=<n>` | The clock. At boot it's taken from the board's RTC chip (BM8563) if that kept its time (`source=rtc`). NTP (`pool.ntp.org`) sets it once Wi-Fi is up (`source=ntp`) and the chip is updated from it. Without Wi-Fi the [USB tether](#usb-tether)'s computer can set it (`source=usb`). Otherwise unknown (`-`). `local` is like `2026-11-06T15:30:00+02:00`. |
| `TIME SYNC` | `OK SYNCING` or `ERR no network ...` | Ask the time server now (it's also asked every 6 hours by itself). `syncs` in `TIME` goes up when it has answered. |
| `TZ [<posix tz>]` | `OK tz=<POSIX TZ>` | Show or set the time zone, a POSIX TZ string such as `EET-2EEST,M3.5.0/3,M10.5.0/4` (the default, Finland) or `UTC0`. Offsets are west-positive (UTC+2 is `-2`). Stored in NVS. `ERR not a POSIX TZ string ...` if it doesn't parse. |
| `AUTORUN [<app>\|off\|-]` | `OK AUTORUN <app>\|off` | The app started at boot instead of the home menu (default off). Must be installed (`installed` or `modified`). Stored in NVS, and also set from the device's Settings. |
| `EVAL <lua>` | `OK <results>` or `ERR <message>` | Runs in the current VM (one is created if needed). Tried as `return <lua>` first, like the Lua REPL. Several results are separated by tabs. Reply text is cut at 255 bytes. |
| `WIFI` | `OK state=off\|connecting\|connected ssid=<hex or -> ip=<addr or -> rssi=<dBm> port=3333 host=luaboard.local client=0\|1` | Wi-Fi status. `rssi=0` when not connected. |
| `WIFI SET <ssid-hex> [<password-hex>]` | `OK <status as above>` | Store the network in NVS and (re)connect. Name (1..32 bytes) and password (up to 64 bytes) are hex-encoded UTF-8 bytes, so spaces and any characters are safe. No password = open network. |
| `WIFI OFF` | `OK <status>` | Forget the network, Wi-Fi off. |
| `PAIR` | `OK paired=on id=<16 hex>` or `OK paired=off` | Is the Wi-Fi link paired, and the key's id. |
| `PAIR NEW` | `OK PAIR <id> <key: 64 hex>` | A new random key (and id) for the Wi-Fi link. Every Wi-Fi client is dropped (`EVT bye paired anew`). USB only. |
| `PAIR KEY` | `OK PAIR <id> <key: 64 hex>` | The current key, to pair another computer. `ERR not paired ...` without one. USB only. |
| `PAIR OFF` | `OK paired=off` | Forget the key: the Wi-Fi link takes no commands, and Wi-Fi clients are dropped. USB only: over Wi-Fi every `PAIR` subcommand gets `ERR pairing only works over USB`. |
| `TUN ON <utc>`, `TUN OFF` | `OK TUN ON [started]`, `OK TUN OFF` | The computer lends the device its network, see [USB tether](#usb-tether). |
| `HELP` | `OK <command list>` | |

Files are named `<app>/<file>`: app names 1..23 and file names 1..31
characters from `[A-Za-z0-9_.-]`, not starting with `.`. File names can't end
in `.tmp` (the device's name for a file while it's written). Limits: 256 KiB per
file. Uploads not installed yet: 64 files, 512 KiB in RAM. Installed apps: the
~9.9 MiB apps partition (at most 64 apps are listed).

### Upload (`PUT`)

```
host:   PUT hello/main.lua 118 865a51bb\n
device: OK READY\n
host:   <118 raw bytes>
device: OK STORED hello/main.lua 118\n
```

Errors after the payload: `ERR crc mismatch: got <crc> expected <crc>`,
`ERR store failed: RAM for uploads full (install or remove some)`. Before
`OK READY`: `ERR size out of range (max 262144)`, `ERR bad name ...`,
`ERR usage: ...`. A successful upload makes its app the default for bare
`<file>.lua` names.

The CRC is CRC-32/IEEE (the zlib/PNG one) of the payload, in hex. If the
payload stops for 3 s, the device answers `ERR upload timeout (<got>/<size>
bytes)` and drops the upload, but the session stays logged in. The rest of
the payload may still be on its way, so the device drops what arrives until
the link has been quiet for 0.5 s. Only then does it read commands again.
Uploads only change RAM, so send `INSTALL` or `SAVE` to make them persistent.

### Firmware update (`OTA`)

```
host:   OTA 1593680 3b7e…(64 hex)\n
device: OK READY\n
host:   <1593680 raw bytes>
device: OK UPDATED 94bf241\n
        (the device reboots half a second later)
```

The image is the app part of a firmware build: the app alone (a release's
`luaboard-<board>-update.bin`, `build/idf-<board>/luaboard.bin` of a build),
or the whole flash image (a release's `luaboard-<board>.bin`, the `fw`
target's `build/luaboard-<board>.bin`) from its first app slot (0x20000) on.
`luaboard update` takes either and cuts the whole one. The simulator
(`target=sim` in `INFO`) takes a simulator executable for its board instead
(`build/<board>/esp32-sim`). The PC tools check the image against `INFO` before they send it.

The device stops the running app and shows the update screen with the
progress. It writes the image into the app slot that isn't running as it
arrives, never holding it whole in RAM, and checks it at the end: the SHA-256,
the image format for its chip, and that it is luaboard firmware. Then it marks
that slot to boot, answers `OK UPDATED <version>` and reboots. Installed apps,
settings, secrets and Wi-Fi stay: they live in partitions an update doesn't
touch. Errors after the image: `ERR sha256 mismatch: the image arrived
damaged`, `ERR not a firmware image for this device (...)`, `ERR not luaboard
firmware (...)`, `ERR writing the slot failed: ...`. The device then keeps
running the old firmware and starts again the app the update stopped (or
goes back to its menu). Before `OK READY`:
`ERR usage: OTA <size> <sha256hex>`, `ERR image size <n> out of range (the slot
holds <n>)`, `ERR an update is already running`. If the image stops for 3 s, `ERR upload timeout (<got>/<size>
bytes)` as for `PUT`.

While an update runs, other links get `ERR a firmware update is running` for
everything except `PING`, `INFO`, `LS`, `APPS`, `GET`, `FB`, `TIME`, `HELP`
and `CRASH`.

The new firmware is in use at once and takes the next update right away. The
old one stays in the other slot, and the device goes back to it if the new
one keeps crashing:

* A crash reset early in its boot, before it has opened its storage (NVS),
  makes the bootloader go back.
* After that, 5 boots in a row that end in a firmware crash (a panic, or a
  watchdog ending a hang) make the firmware go back itself. Brown-outs don't
  count: bad power isn't the firmware's fault. The crash-loop guard skips the
  autostart app after 3 such boots, so an app that crashes the device is
  ruled out first.
* The new firmware is proven once it has run 10 minutes, and from then on it
  stays. A reset that isn't a crash (power cycle, reset button) starts the
  count of crash boots over but doesn't prove it: a power cycle may be
  someone ending a hang.

The old firmware then reports `ota=rolledback` in `INFO` and `EVT ota
rolled-back`.

On a USB link the port goes away during the reboot (the chip's USB
re-enumerates), and a Wi-Fi client is dropped. The simulator keeps its serial
port, so its clients see the `EVT boot` of the new firmware.

Pulling a release from the device's own menu (Install on the update screen)
reuses the events of `OTA`: `EVT ota started`, then `EVT ota installed` and
the reboot, or `EVT ota failed <why>` with the old firmware still running.
A failed download says why in a few words there (as on the screen: `no
connection`, `timed out`, `took too long`, `Wi-Fi dropped`, `server error 404`,
`TLS error`, `download cut short`, `wrong size`, `download too big`), and the
whole story just before it in `EVT update install-failed`.

## Release manifest

The device finds new releases from a small JSON file of its board, fetched
over HTTPS from
`https://github.com/veikkos/luaboard/releases/latest/download/manifest-<board>.json`
(`manifest-m5stack-cores3.json`). That address always points to the newest release that isn't a prerelease, and
isn't the rate-limited GitHub API. It redirects (to GitHub's download host),
and the device follows redirects. The release
workflow (`tools/release.cmake`) writes it from the built image, CHANGELOG.md
and `app/src/app_config.h`, and attaches it to every release. A release's
files for a board are the manifest, `luaboard-<board>-update.bin` (the app
part alone, what the manifest points at) and `luaboard-<board>.bin` (the
whole flash image, for `luaboard flash`).

```json
{
  "format": 1,
  "version": "v1.1.0",
  "board": "m5stack-cores3",
  "url": "https://github.com/veikkos/luaboard/releases/download/v1.1.0/luaboard-m5stack-cores3-update.bin",
  "size": 1580000,
  "sha256": "(64 hex digits)",
  "lua_api": 2,
  "lua_api_min": 1,
  "notes": "One line for the device screen"
}
```

| Field | Meaning |
|---|---|
| `format` | 1, the manifest's first field. A firmware that meets a later format installs nothing from it (below). |
| `version` | The release tag (what `INFO` `ver` shows once it runs). |
| `board` | The board the image is for. A device ignores other boards' releases. |
| `url` | The app image (what `OTA` takes), over HTTPS. |
| `size`, `sha256` | Checked while downloading, as for `OTA`. |
| `lua_api`, `lua_api_min` | The Lua API range the release's firmware runs, `APP_LUA_API_MIN` to `APP_LUA_API`. |
| `notes` | The first paragraph of the CHANGELOG section in one line, plain text, at most 120 bytes. Optional for a reader. |

The address and `format: 1` are a promise to every device ever shipped: they
must keep working. A new field is fine, old firmware skips fields it doesn't
know. A change that old firmware would misread needs `format: 2`. Firmware
that reads only format 1 doesn't read past that number and can't install such
a release: its menu says "Manual update needed" instead (`update=manual` in
`INFO`), on a screen that tells to install the release from a computer. So the
manifest at this address stays format 1 for as long as old devices should
update by themselves, and a later format goes under another file name, which
newer firmware reads. The simulator can be
pointed elsewhere for tests, see [platform/sim/README.md](../platform/sim/README.md#options).

A release is offered when its version numbers are higher than the running
firmware's. A development build (such as `v1.0.0-5-gabc123`) counts as newer
than v1.0.0, so it is offered only v1.0.1 and later. A build that isn't from
a tagged release is offered nothing.

The first release (1.0.0) takes a manifest only within these limits, so every manifest
of format 1 must stay within them (`tools/release.cmake` checks the ones it
can):

* the whole answer at most 4096 bytes (`UPDATE_MANIFEST_MAX`).
* `version` and `board` under 32 bytes, `url` under 256 bytes.
* `url` starting with `https://`.
* `version` exactly `vX.Y.Z` (numbers only, no suffix) for a release that is
  offered.
* `format`, `size`, `lua_api` and `lua_api_min` plain non-negative integers
  (no sign, fraction or exponent), each at most 999999999. `size` not 0, and
  `lua_api_min` not above `lua_api`.
* `sha256` exactly 64 hex digits.
* the value of a field it doesn't know nested at most 16 arrays or objects
  deep.
* `notes` is cut to fit 127 bytes on the device (at a whole UTF-8 character),
  so its length is free, but the release script keeps it to 120.

## Remote touch

`TOUCH` and `KEY` let a PC use the device as if its hand were on it (the
studio's Display panel does this with the mouse and keyboard), over USB or
Wi-Fi. Remote readings go through the same state machine as the touch
controller, one per 10 ms reading, so the device tells taps, holds, drags,
swipes and button presses apart the same way. A tap or a swipe whose lines
arrive together still counts as one. Queued moves in a row collapse into the
newest. While the remote finger is down it stands in for the real one.

Repeat `TOUCH <x> <y>` while the finger is held, at least every second: a
remote finger not repeated for 1 s is let go, and so is the one of a link
that goes away, so a PC that vanishes never leaves a finger down.

## Wi-Fi link

The Wi-Fi link is encrypted and needs pairing. Pairing happens over USB:
`PAIR NEW` makes a random 256-bit key K with a random 8-byte id, and `PAIR KEY`
hands the same key to another computer. The PC tools keep keys in
`~/.config/luaboard/wifi-keys` (Windows: `%APPDATA%\luaboard\wifi-keys`), or
in `$LUABOARD_KEYS` when it is set, one line per device: `<id> <key hex>`,
readable only by the user. Until the device is paired, a Wi-Fi connection
gets `ERR not paired: ...` and is closed, so an unpaired device takes no
commands over Wi-Fi.

A connection starts in clear text with the client's nonce, and the device
answers with the key id (so the client knows which key to use) and its own
nonce:

```
> HELLO 1 3f9a0c…                          (version 1, 16 random bytes, hex)
< OK HELLO 1 8d21e4b0c7a95f12 77c0e1…      (key id, 16 random bytes, hex)
```

Anything else gets `ERR this link is encrypted: ...` and the connection is
closed. Both sides then derive this connection's keys:

```
prk = HMAC-SHA256(K, "luaboard wifi 1" || id || client nonce || device nonce)
c2s = HMAC-SHA256(prk, "c2s")      client to device
s2c = HMAC-SHA256(prk, "s2c")      device to client
```

From then on each direction is a stream of frames carrying the same bytes
as the USB link (command lines, replies, payloads, `LOG`/`EVT`):

```
length (2 bytes, big-endian) = n + 16
ciphertext (n bytes, 1..4096)  AES-256-GCM with c2s or s2c
tag (16 bytes)
```

The GCM nonce is 4 zero bytes followed by the number of frames already sent
in that direction, as a 64-bit big-endian count. The additional data is the
2 length bytes. A frame that doesn't decrypt ends the connection.

* The client is logged in when its first frame decrypts: only a holder of K
  can make one. Before that it gets no replies and no `LOG`/`EVT` lines.
* The client knows it reached the real device when the device's first frame
  decrypts. An impostor that answers `HELLO` can't produce one.
* Fresh nonces from both sides make every connection's keys different, so
  recorded traffic can't be replayed, and nothing on the wire helps guess K.
* `WIFI SET` and `WIFI OFF` work over Wi-Fi once logged in. Pairing is USB
  only. `PAIR NEW` and `PAIR OFF` drop every Wi-Fi client.

At most one client is logged in at a time, plus up to two that are still
logging in. A new connection only pushes out the oldest client that hasn't
logged in (`EVT bye another client connected`). A logged-in client is
replaced only when a newer client logs in (`EVT bye another client logged
in`), so a PC reconnecting after a network blip still wins over its own dead
connection, while someone without the key can't kick you off. A client that
hasn't logged in after 60 s is dropped (`EVT bye no login in time`), as is
one that stops reading for 2 s, and all of them when Wi-Fi is lost or turned
off. Connections from the device's own scripts (`net.connect` to its own
address) are refused: apps can't drive the device.

## USB tether

A computer on the USB link can lend the device its network. The device's
TCP connections (the scripts' `net`, `http` and `mqtt`, the update checks)
then go through the computer: the device asks it to connect, and the
connection's bytes travel as lines of this protocol. TLS still runs on the
device, which checks the server's certificate itself, so the computer only
ever passes on encrypted bytes. The device uses Wi-Fi when it is connected
and the tether otherwise. A connection keeps the way it started on.

The computer turns the tether on with this, and keeps it on by sending it
again at least every 2 seconds:

| Command | Reply | Notes |
|---|---|---|
| `TUN ON <utc>` | `OK TUN ON`, `OK TUN ON started` | `<utc>`: the computer's clock, seconds since 1970. The tether stays on until the device has heard nothing from the computer for 5 s (any line counts, from when the device is done with it), so it ends by itself when the computer's tool exits or the cable is pulled. `started`: it was off until now, so none of the computer's earlier connections are left on the device. When it starts, the device takes its time from `<utc>` unless NTP has set the clock (`TIME` then says `source=usb`). USB only: over Wi-Fi it's `ERR the tether works only over USB`. |
| `TUN OFF` | `OK TUN OFF` | End it now. |

When the tether ends, every connection through it closes, and the scripts
see the error `the computer's network went away`.

The connections are carried by frames, in both directions. A frame is a
line that starts with `TUN`. Frames aren't commands: they get no reply,
and they can come between a command and its reply, like `EVT` lines.
Device frames go only to the link that has the tether.

| Frame | From | Meaning |
|---|---|---|
| `TUN OPEN <id> <host> <port>` | device | Connect to `<host>` (a name or an IPv4 address) on TCP port `<port>`. |
| `TUN OPENED <id>` | computer | Connected. |
| `TUN DATA <id> <base64>` | both | Bytes of the connection, 1 to 1500 of them, in standard base64 with padding. |
| `TUN ACK <id> <n>` | both | `<n>` more bytes of the other side's data have been taken (read by the script, written to the server). |
| `TUN CLOSE <id> [<why>]` | both | The connection is over. Without `<why>` it ended normally (the server or the device closed it), else `<why>` says what went wrong, such as `cannot resolve example.com`, `connection refused` or `not allowed`. Sent instead of `OPENED` when the connection can't be made. |

The device picks the ids: whole numbers counting up from a random one, never
used twice while it runs, so a late frame for a connection that has closed
can't be taken for a new one, also after the device restarts. A frame with
an id that isn't open is answered with `TUN CLOSE <id>`, for the computer to
drop what it still has of it. `CLOSE` gets no answer, and after it neither
side sends anything more with that id. Data sent before a `CLOSE` is
delivered before it: the device reads what came before the computer's, and
the computer writes what came before the device's to the server.

Each side may have at most 8192 bytes of `DATA` for a connection that the
other side hasn't acknowledged with `ACK`. That is all the room the device
keeps for a connection, so a computer that sends more gets the connection
closed (`TUN CLOSE <id> window exceeded`), as does a frame the device can't
read (`bad frame`). A computer that stops reading long enough for the
device to drop one of a connection's frames gets the connection ended
with `USB tether: the computer stopped reading`, as the other side would
wait for that frame forever.

A program that starts lending its computer's network sends `TUN OFF`
first: connections an earlier one left on the device are over, as that
program's sockets went with it. When `TUN ON` says `started`, it drops the
connections it has: the tether had ended, so the device's ends are gone.

The computer decides where it lets the device connect. luaboard's tools
refuse addresses on the computer itself (`127.0.0.1`, `::1`, so a script
can't reach services that only listen there) unless told otherwise. Other
addresses on the computer's network are allowed, as they are to a device on
Wi-Fi.

```
> TUN OFF
< OK TUN OFF
> TUN ON 1795075200
< OK TUN ON started
< TUN OPEN 3141592653 example.com 443
> TUN OPENED 3141592653
< TUN DATA 3141592653 FgMBAgABAAH8AwM...
> TUN DATA 3141592653 FgMDAHoCAAB2AwM...
> TUN ACK 3141592653 517
< TUN ACK 3141592653 1452
< TUN CLOSE 3141592653
```

## Secrets

| Command | Reply | Notes |
|---|---|---|
| `SECRETS [<app>]` | `SECRET <app> <name> <bytes>` lines, then `OK <count>` | Names only: no command sends a value back. |
| `SECRET SET <app> <name> <hex>` | `OK SECRET SET <app> <name>` | Value hex-encoded (any bytes). Only that app's scripts can read it. |
| `SECRET PART <app> <name> <hex>` | `OK SECRET PART <app> <name> <bytes so far>` | The start of a value too long for one line (a 2048-byte value is 4096 hex characters). The next `SECRET SET` of that name from the same connection adds the rest and stores the whole value. The PC tool sends values over 768 bytes this way. |
| `SECRET RM <app> <name>` | `OK SECRET RM <app> <name>` | |
| `SECRET CLEAR <app>` | `OK SECRET CLEAR <app>` | Deletes all of the app's secrets. (Clear data in the menu also deletes its `store` values and Bluetooth bonds.) |

Secret names are 1..15
characters from `[A-Za-z0-9_.-]`, not starting with `_`. Values up to 2048
bytes (in practice less: the hex must fit the 2047-byte line). An app's
secrets up to 8 KB together, all apps' 16 KB, and at most 32 secrets on the
device. `SECRET RM` of a missing secret still answers `OK`.

A logged-in client can still get at a value: `EVAL` runs in the VM of the
app last run, as that app, so `EVAL secrets.get(name)` reads its secrets,
just as code uploaded into the app could. The pairing key guards that.

Each secret belongs to an app. `UNINSTALL` deletes the app's secrets too.
`INSTALL` keeps them, and so does a restart even if the app was only
uploaded (and so is gone): a login survives while you work on an app. To
get rid of the secrets and stored values of such a name, `UNINSTALL` it.

## Browser login

A script asks for a login through the browser (`browser.login()`, used by
`oauth.lua`), and a tool opens the URL and catches the redirect.

| Command | Reply | Notes |
|---|---|---|
| `BROWSER` | `OK BROWSER <id> <app> <redirect> <url> <title>` or `OK BROWSER none` | The pending request (tools ask after connecting). `<app>` is the app whose script asks, set by the firmware (`-` for none): tools show it, so a script can't pass its login off as another app's. |
| `BROWSER <id> <query>` | `OK BROWSER answered` | The redirect's query string, e.g. `code=...&state=...`. |
| `BROWSER <id> CANCEL` | `OK BROWSER cancelled` | The user declined. |

`redirect` is always `http://127.0.0.1:<port>/...` or `localhost`. One request
at a time. A new one cancels the pending one, and it's cancelled when the
script ends. `ERR no pending browser request <id>` if the id isn't pending.

## Events

| Event | When |
|---|---|
| `EVT boot fw=.. ver=.. board=.. model=.. slot=.. ota=.. reset=.. boot=power\|timer\|reset` | Firmware started (`slot`, `ota`, `reset` as in `INFO`). `boot`: why, as an app sees it in `sys.info().boot`: the power key or power applied, the wake-up time of `sys.off`, or a reset. |
| `EVT off wake=<seconds>\|none by=app\|setting` | The device turns itself off now: an app called `sys.off`, or the Turn off after setting's time is up. `wake`: it turns itself on again after this many seconds. |
| `EVT crash ver=.. cause=.. at=..` | At boot after a crash: what it was and its innermost call (all of it: `CRASH`). |
| `EVT ota started size=<n> slot=<slot>` | An `OTA` update began. |
| `EVT ota installed ver=<version> slot=<slot> rebooting` | The update was checked and is booted next. The reboot follows. |
| `EVT ota failed <why>` | The update was refused or stopped. The old firmware runs on. |
| `EVT ota rolled-back ver=<version>` | At boot: the update kept crashing, this is the old firmware again. |
| `EVT update available ver=<version>` | The check found a newer release (once per version per boot). Also `update=<version>` in `INFO`. |
| `EVT app updates n=<n>` | The number of installed apps the marketplace has a newer version of changed. Also `app_updates` in `INFO`. |
| `EVT apps check-failed <why>` | The marketplace check failed. It comes again on the update check's retry times. |
| `EVT update manual format=<n>` | The check found a release whose manifest is in a later format than this firmware reads: it has to be installed from a computer. Also `update=manual` in `INFO`. |
| `EVT update check-failed <why>` | A check failed (no network, a bad answer, a manifest this firmware can't use). |
| `EVT update install-failed got=<bytes>/<size> <why>` | Installing a release from the menu stopped while downloading, with the full reason (such as a TLS error's code), and `EVT ota failed <why>` with the short one follows. |
| `EVT update offer ver=<version> wont-run=<app,app or ->` | The user opened the update row's confirm screen. `wont-run` lists the installed apps whose `lua_api` is outside the release's range. |
| `EVT update offer manual` | The user opened the row of a release that needs a manual update: its screen tells what to do. |
| `EVT browser <id> <app> <redirect> <url> <title>` | A script asks for a browser login (see above). |
| `EVT browser-done <id> ok\|cancelled` | That request is over: close any prompt. |
| `EVT started <app>/<file>` | A script was started (by `RUN`, the menu, autostart, or a restart after `INSTALL`/`SAVE`). |
| `EVT stopped` | `STOP` stopped a running script. |
| `EVT stopped <app>/<file> (back button)` / `(power key)` / `(exit)` / `(off)` | The user held B or pressed the power key, the script called `sys.exit()`, or the device turns off. |
| `EVT menu` | The home menu is on the screen. |
| `EVT screen off` / `EVT screen on` | The device went to sleep (Sleep after setting, a long press of the power key, Sleep now) or woke up. |
| `EVT installed <app>` / `EVT uninstalled <app>` | The set of apps changed (`uninstalled` also from the device's menu). |
| `EVT error <message>` | A runtime error (or watchdog/memory limit) in `loop` or a timer stopped the script, or a start from the menu, autostart or an `INSTALL`/`SAVE` restart failed. |
| `EVT safe-mode reason=touch` | A finger was on the screen at boot, so the autostart app (if any) was skipped. |
| `EVT safe-mode reason=lua_api` | The autostart app was made for a Lua API version this firmware doesn't run, so it was skipped (it stays installed and stays the autostart app). |
| `EVT safe-mode reason=crash-loop boots=N` | The last N boots ended in a crash reset (panic, watchdog, brown-out) while the autostart app ran, so it was skipped. Lua errors don't count: they never reset the device. |
| `EVT wifi connected ip=<addr> port=3333` | Joined the network, and the TCP server is up. |
| `EVT time rtc <ISO 8601>` | At boot: the time was taken from the RTC chip. |
| `EVT time synced <ISO 8601>` | The clock was set from the network for the first time since boot. |
| `EVT time usb <ISO 8601>` | The [USB tether](#usb-tether) started, and the clock was set from the computer's. |
| `EVT wifi lost reason=<n>` | Lost the network after being connected (ESP-IDF disconnect reason). The device keeps retrying with back-off up to 30 s. |
| `EVT bye <why>` | Sent to a Wi-Fi client just before the device drops it: `another client connected`, `another client logged in`, `no login in time`, `paired anew` or `unpaired`. Encrypted to a logged-in client, in clear text to one that hasn't finished `HELLO`, and not sent to one in between. |
| `EVT cleared <app>` | The app's data (secrets, stored values, Bluetooth bonds) was cleared from the device's menu. |

## Example session

```
> PING
< OK PONG
> PUT hello/main.lua 31 702ae015
< OK READY
> print("hi") function loop() end
< OK STORED hello/main.lua 31
> RUN hello
< LOG hi
< OK RUNNING hello/main.lua
< EVT started hello/main.lua
> EVAL adc.read(adc.PORTB_IN)
< OK 2051
> INSTALL hello
< OK INSTALLED hello
< EVT installed hello
```

## Hardware notes

* The firmware moves the ESP-IDF console and logs to UART0, which isn't wired
  to USB on this board, so the USB port carries only protocol lines. Clients
  should still ignore untagged lines.
* The USB-Serial/JTAG port resets the chip when RTS is up and DTR down
  (that's how esptool enters the bootloader). The PC tools raise DTR, then
  RTS, so opening the port doesn't reset the board.
* A frame from `FB` is 150 KB, so the live preview uses a noticeable share of
  the link. The studio sends the CRC of the frame it has, so an unchanged
  screen costs a short `OK SAME`, and it polls only while its Display panel is
  open.
