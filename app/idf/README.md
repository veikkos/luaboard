# Firmware for the real device

ESP-IDF v6.0.3 project for the M5Stack CoreS3, CoreS3 Lite and CoreS3 SE
(one image for all three). What to check on a new board is in
[docs/BRINGUP.md](../../docs/BRINGUP.md).

## Toolchain

Espressif's installer `eim` sets up ESP-IDF, on macOS, Windows or Linux:

```sh
brew tap espressif/eim && brew trust espressif/eim && brew install eim   # macOS
# Windows, Linux: https://docs.espressif.com/projects/idf-im-ui/ (eim on the PATH)
cmake --build --preset default --target toolchain   # ESP-IDF into ./.toolchain (~2-3 GB)
```

ESP-IDF needs Python 3.10 to 3.13 (eim checks). Compilers go to
`~/.espressif`; `eim remove v6.0.3` and deleting `.toolchain` remove it again.

## Build and flash

```sh
cmake --build --preset default --target fw     # build (Windows: --preset vs)
LUABOARD_PORT=/dev/cu.usbmodem1101 cmake --build --preset default --target flash   # with idf.py
```

The `fw` target also writes `build/luaboard-m5stack-cores3.bin`, which flashes
without ESP-IDF:

```sh
pc-tool/target/release/luaboard -p /dev/cu.usbmodem1101 flash build/luaboard-m5stack-cores3.bin
```

or with Firmware... in the studio. Only the bootloader, partition table,
otadata (reset, so the first firmware slot boots) and app are written;
installed apps and settings are kept. `flash --check` lists
what would be written. If entering download mode fails, hold reset until the
green LED lights and retry.

Managed components (mDNS, LittleFS) are pinned in `dependencies.lock`.
