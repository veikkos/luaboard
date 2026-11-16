#!/usr/bin/env bash
# The README's showcase GIF: eight example apps on a CoreS3 SE, a second each,
# with made-up live data. Needs the simulator and the CLI built (the default
# preset's tool target and esp32-sim), ImageMagick and ffmpeg.
#
#   pc-tool/demo/showcase/make.sh [out.gif]      (default: pc-tool/demo/showcase.gif)
#
# One headless simulator on a fixed clock runs the real apps. Their data comes
# from small Lua stand-ins next to this script (an app's own http.lua or
# gcal.lua replaces the built-in or the example's one; .jpg files next to
# it are uploaded too, for the pictures it serves), and the heart rate
# strap and the Di2 derailleur are the examples' simulated Bluetooth devices,
# driven over the control channel. Frames are taken a tenth of a second of
# the device's time apart and put into the picture of the device
# (docs/img/cores3se.png) with the simulator's pixel grid.
#
# Settings (environment): FPS (10), SECS per app (1), CONTROL port (3395),
# EPOCH (UTC seconds the clock starts at: Tue 10 Nov 2026 12:24 in Finland),
# KEEP=1 keeps the work folder.

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
OUT=${1:-$ROOT/pc-tool/demo/showcase.gif}
FPS=${FPS:-10}
SECS=${SECS:-1}
CONTROL=${CONTROL:-3395}
EPOCH=${EPOCH:-1794306245}
SIM=${SIM:-$ROOT/build/m5stack-cores3/esp32-sim}
CLI=${CLI:-$ROOT/pc-tool/target/release/luaboard}
FRAME_PNG=$ROOT/docs/img/cores3se.png
LCD_AT=+114+192                       # where the LCD is in that picture (2 px per LCD pixel)

for tool in magick ffmpeg; do
  command -v "$tool" >/dev/null || { echo "make.sh: $tool is needed" >&2; exit 1; }
done
[ -x "$SIM" ] && [ -x "$CLI" ] || { echo "make.sh: build the simulator and the CLI first" >&2; exit 1; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/showcase.XXXXXX")
mkdir -p "$WORK/frames" "$WORK/png"
export LUABOARD_PORT=$WORK/tty LUABOARD_SIM_CONTROL=127.0.0.1:$CONTROL

cleanup() {
  "$CLI" sim-ctl quit=1 >/dev/null 2>&1 || true
  if [ "${KEEP:-0}" = 1 ]; then echo "work folder: $WORK"; else rm -rf "$WORK"; fi
}
trap cleanup EXIT

lb() { "$CLI" --linger 0 "$@"; }
ctl() { "$CLI" sim-ctl "$@" >/dev/null; }
eval_lua() { lb eval "$1" 2>/dev/null || true; }

# Until EXPR is true on the device, letting its time run on meanwhile.
wait_lua() {
  for _ in $(seq 100); do
    [ "$(eval_lua "$1")" = true ] && return 0
    ctl advance=100
  done
  echo "make.sh: timed out waiting for: $1" >&2
  exit 1
}

# An example with the stand-ins of this folder (if any), uploaded and started.
start_app() {
  local app=$1 dir=$WORK/apps/$1
  mkdir -p "$dir"
  cp "$ROOT/marketplace/$app/"*.lua "$dir/"
  if [ -d "$HERE/$app" ]; then
    cp "$HERE/$app/"*.lua "$dir/"
    for f in "$HERE/$app/"*.jpg; do if [ -e "$f" ]; then cp "$f" "$dir/"; fi; done
  fi
  if [ -n "${2:-}" ]; then printf '%s\n' "$2" > "$dir/secrets.env"; fi
  local files=("$dir/"*.lua)
  for f in "$dir/"*.jpg; do if [ -e "$f" ]; then files+=("$f"); fi; done
  lb put "${files[@]}" --app "$app" --run main.lua >/dev/null
}

FRAMES=$((FPS * SECS))
STEP=$((1000 / FPS))
n=0
# FRAMES screenshots STEP ms apart; `hook i` runs before frame i (1-based).
record() {
  local hook=${1:-}
  for i in $(seq "$FRAMES"); do
    if [ -n "$hook" ]; then "$hook" "$i"; fi
    ctl "advance=$STEP"
    n=$((n + 1))
    lb screenshot "$WORK/frames/$(printf %03d $n).ppm" >/dev/null
  done
}

echo "simulator..."
"$SIM" --headless --loopback --link "$WORK/tty" --nvs "$WORK/nvs.bin" --control "$CONTROL" \
  --set "time=$EPOCH" \
  --ble-device "$ROOT/marketplace/heart_rate/sim_device.json" \
  --ble-device "$ROOT/marketplace/di2/sim_device.json" > "$WORK/sim.out" 2>&1 &
for _ in $(seq 50); do lb ping >/dev/null 2>&1 && break; sleep 0.1; done
lb wifi set "Showcase" pw >/dev/null       # the clock comes from the network
wait_lua "sys.time() ~= nil"

# ---- heart rate: two minutes of a workout first, then rising in zone 4.
# The heart follows the effort with a lag (about 9 s up, 16 s down) and a
# beat or two of jitter: steady, a hard 30 s into zone 5 (so the session's
# max is above what shows), a recovery and a steady push.
echo "heart rate..."
start_app heart_rate
wait_lua 'HR.phase == "live"'
lines=$(awk 'BEGIN { hr = 124; r = 7
  for (i = 0; i < 125; i++) {
    target = (i < 50) ? 146 : (i < 80) ? 178 : (i < 100) ? 152 : 166
    hr += (target - hr) / ((target > hr) ? 9 : 16)
    r = (r * 16807) % 2147483647
    bpm = int(hr + 1.5 * sin(i / 2.7) + (r / 2147483647 - 0.5) * 3 + 0.5)
    printf "ble_notify=hr:2a37:16 %02x 00 04\nadvance=1000\n", bpm } }')
args=()
while IFS= read -r l; do args+=("$l"); done <<< "$lines"
ctl "${args[@]}"
hr_hook() {
  case $1 in
    1) ctl "ble_notify=hr:2a37:16 a6 00 04" ;;   # 166
    5) ctl "ble_notify=hr:2a37:16 a7 00 04" ;;   # 167
    9) ctl "ble_notify=hr:2a37:16 a8 00 04" ;;   # 168
  esac
}
record hr_hook

# ---- calendar: a meeting in a few minutes, the alert pulsing
echo "calendar..."
start_app gcal "gcal_client=showcase
gcal_secret=showcase"
wait_lua "UI.alert ~= nil"
record

# ---- di2: shifting up the cassette on the big ring
echo "di2..."
start_app di2
GEAR=00002ac1-5348-494d-414e-4f5f424c4500
gear() { ctl "ble_value=di2:$GEAR:00 06 03 02 02 $(printf %02x "$1") 0c 02"; }
wait_lua 'store.get("addr") ~= nil'
gear 3
ctl advance=500
di2_hook() {
  case $1 in
    2) gear 4 ;; 4) gear 5 ;; 6) gear 6 ;; 8) gear 7 ;;
  esac
}
record di2_hook

# ---- clock: the digital face, its hints gone
echo "clock..."
start_app clock
ctl advance=5200
record

# ---- weather and spot prices: their stand-in data
echo "weather..."
start_app weather
wait_lua 'UI.fetches >= 1 and UI.status == "ok"'
record

echo "spot price..."
start_app spot_price_fi
ctl advance=1500
record

# ---- stocks: the last session (before the opening in New York), SPY then QQQ
echo "stocks..."
start_app stocks "alpaca_key=showcase
alpaca_secret=showcase"
wait_lua 'UI.fetches >= 1 and UI.status == "ok"'
stocks_hook() {
  if [ "$1" = 6 ]; then ctl button=C advance=100 button=-; fi
}
record stocks_hook

# ---- news: a made-up feed with NASA's pictures, all loaded
echo "news..."
start_app news "feed=https://example.com/feed.xml"
wait_lua 'UI.status == "ok" and UI.stories[1].px ~= nil and UI.stories[3].thumb ~= nil'
record

# ---- the frames into the device's picture, then the GIF
echo "frames..."
# The simulator's pixel grid at 2x: the last row and column of each pixel's
# 2x2 cell let through 72.5 % (black at alpha 70), and the image is brightened
# to make up for it (its gain and the backlight, as sim_ui.c draws it).
magick -size 2x2 xc:white -fill "gray(185)" -draw "point 1,0" -draw "point 0,1" \
  -fill "gray(134)" -draw "point 1,1" "$WORK/cell.png"
magick -size 640x480 "tile:$WORK/cell.png" "$WORK/grid.png"
for f in "$WORK"/frames/*.ppm; do
  b=$(basename "$f" .ppm)
  magick "$f" -filter point -resize 200% -evaluate multiply 1.174 \
    "$WORK/grid.png" -compose multiply -composite "$WORK/lcd.png"
  magick "$FRAME_PNG" -alpha off "$WORK/lcd.png" -geometry "$LCD_AT" -compose over -composite "$WORK/png/$b.png"
done
# The palette first, then the frames with it (the palette looped, so every
# frame gets it: ffmpeg 9 drops frames when both run in one filter graph).
ffmpeg -loglevel error -y -framerate "$FPS" -i "$WORK/png/%03d.png" \
  -vf "palettegen=max_colors=128:stats_mode=full" -update 1 -frames:v 1 "$WORK/palette.png"
ffmpeg -loglevel error -y -framerate "$FPS" -i "$WORK/png/%03d.png" -loop 1 -i "$WORK/palette.png" \
  -lavfi "[0][1]paletteuse=dither=none:diff_mode=rectangle" -frames:v "$n" -loop 0 "$OUT"
echo "$OUT ($(du -h "$OUT" | cut -f1), $n frames)"
