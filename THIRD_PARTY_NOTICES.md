# Third-party notices

luaboard is MIT licensed (see [LICENSE](LICENSE)). It includes or builds
against the following third-party software.

## Included in this repository

| Component | Path | License |
|---|---|---|
| Lua 5.4.7, Lua.org, PUC-Rio | `third_party/lua/` | MIT, [third_party/lua/LICENSE](third_party/lua/LICENSE) |
| TJpgDec R0.03, ChaN | `third_party/tjpgd/` | ChaN's free license, in the header of `tjpgd.c` |
| BMI270 configuration file, Bosch Sensortec | `third_party/bmi270/` | BSD-3-Clause, [third_party/bmi270/LICENSE](third_party/bmi270/LICENSE) |
| miniz 3.0.2, Rich Geldreich and contributors | `third_party/miniz/` | MIT, [third_party/miniz/LICENSE](third_party/miniz/LICENSE) |

Lua is unmodified. The PC tool also compiles its parser (`pc-tool/build.rs`)
to syntax-check scripts. TJpgDec has a local configuration change in `tjpgdcnf.h`
(see [third_party/tjpgd/README.md](third_party/tjpgd/README.md)).

The BMI270 configuration file is the data the motion sensor needs loaded
before it measures (`bmi270_config_file` from Bosch Sensortec's BMI270
sensor API, unchanged). Only that data is included, none of the API's code.

miniz is unmodified (the release's amalgamated `miniz.c` and `miniz.h`). Only
the simulator builds it, for its inflate, which PNG drawing uses; the device
uses the older miniz inflate in the ESP32-S3's ROM.

TJpgDec license, from `tjpgd.c`:

```
Copyright (C) 2021, ChaN, all right reserved.

* The TJpgDec module is a free software and there is NO WARRANTY.
* No restriction on use. You can use, modify and redistribute it for
  personal, non-profit or commercial products UNDER YOUR RESPONSIBILITY.
* Redistributions of source code must retain the above copyright notice.
```

`libs/font5x7` holds the widely used 5x7 ASCII bitmap font found in many
embedded display libraries. The glyphs past ASCII (Latin-1, Œ Š Ž Ÿ and €) were
drawn for luaboard.

## Fetched at build time

Not part of this repository, but linked into the binaries. Anyone
distributing a built firmware image or PC tool must comply with these.

Firmware (ESP-IDF v6.0.3 and managed components, versions in
`app/idf/dependencies.lock`):

| Component | License |
|---|---|
| [ESP-IDF](https://github.com/espressif/esp-idf) (FreeRTOS, lwIP, mbedTLS and others) | Apache-2.0, with per-component licenses listed in ESP-IDF |
| [espressif/mdns](https://components.espressif.com/components/espressif/mdns) | Apache-2.0 |
| [joltwallet/littlefs](https://github.com/joltwallet/esp_littlefs) | MIT, bundling [littlefs](https://github.com/littlefs-project/littlefs) (BSD-3-Clause) |

Simulator: [SDL2](https://www.libsdl.org) (zlib) and
[Mbed TLS](https://github.com/Mbed-TLS/mbedtls) (Apache-2.0), from the system.

PC tool: Rust crates listed in `pc-tool/Cargo.lock`, among them
[egui/eframe](https://github.com/emilk/egui) and
[espflash](https://github.com/esp-rs/espflash). They are under permissive
licenses (mostly MIT and/or Apache-2.0). `cargo about` or `cargo license` lists
them.

## Services used by the examples

- `examples/spot_price_fi`: electricity prices from
  [porssisahko.net](https://porssisahko.net).
- `examples/weather`: forecasts from [Open-Meteo](https://open-meteo.com),
  under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).
- `examples/spotify`, `examples/gcal` and `examples/github` use the Spotify
  Web API, the Google Calendar API and the GitHub REST API with your own
  developer credentials, under those services' terms. Spotify, Google
  Calendar and GitHub are trademarks of their owners; this project is not
  affiliated with them.

## Screenshots

`docs/img/cores3.png` (and `cores3.png` in the marketplace's Spotify app)
shows the cover of Ghosts I–IV by Nine Inch Nails, released under
[CC BY-NC-SA 3.0](https://creativecommons.org/licenses/by-nc-sa/3.0/). That
screenshot is shared under the same license.
