# The README's GIFs

## The studio

`../studio.gif` is the studio typing `steps.txt` into `orbit/main.lua` with Live
update on, recorded by the studio itself (`LUABOARD_STUDIO_DEMO`,
`LUABOARD_STUDIO_RECORD`, see `src/bin/luaboard-studio/demo.rs`). To make it
again, from this folder, with the simulator and the studio built:

```sh
../../build/m5stack-cores3/esp32-sim --headless --nvs /tmp/demo-nvs.bin --link /tmp/demo-tty &
LUABOARD_STUDIO_DEMO=steps.txt LUABOARD_STUDIO_RECORD=/tmp/frames \
  ../target/release/luaboard-studio --port /tmp/demo-tty orbit
kill %1
# crop to the editor and the panel, start where the panel shows a picture
# (N: the first such frame, usually 3), one palette for all frames:
cd /tmp/frames && for f in frame-*.ppm; do magick $f -crop 748x335+352+56 +repage ${f%.ppm}.png; done
ffmpeg -framerate 10 -start_number N -i frame-%05d.png \
  -vf "split[a][b];[a]palettegen=max_colors=96[p];[b][p]paletteuse=dither=none" ../studio.gif
```

Then copy `/tmp/studio.gif` over `pc-tool/studio.gif`.

## The showcase

`showcase.gif` loops seven example apps on a CoreS3 SE in the simulator, a
second each: heart rate, calendar, Di2, clock, weather, spot prices and
stocks.
`showcase/make.sh` makes it again, with the simulator and the CLI built and
ImageMagick and ffmpeg installed:

```sh
pc-tool/demo/showcase/make.sh            # writes pc-tool/demo/showcase.gif
```

It runs the real apps in one headless simulator on a fixed clock. Their data
is made up: `showcase/<app>/` holds Lua files uploaded into that app on top of
its own, replacing a module it requires (`http.lua` for weather, spot prices
and stocks, `gcal.lua` for the calendar). The heart rate strap and the Di2
derailleur are the examples' simulated Bluetooth devices, driven over the
control channel. Frames are a tenth of a second of the device's time apart,
put into the picture of the device (`docs/img/cores3se.png`)
with the simulator's pixel grid. `FPS`, `SECS` (per app) and `KEEP=1` (keep
the work folder) are settings; the top of the script lists them.

To change what an app shows, edit its stand-in or the scene in `make.sh`
(what's sent to the devices before and during its frames).
