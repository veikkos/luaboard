# End-to-end tests

```sh
cmake --build --preset default --target unit   # Rust unit tests
cmake --build --preset default --target e2e    # end-to-end suite
E2E_ONLY="oauth menu" cmake --build --preset default --target e2e   # only sections whose name contains a word
E2E_KEEP=1 ...                                 # keep failed sections' work folders (logs, screenshots)
E2E_LIMIT=300 ...                              # time limit per section in seconds (default 120)
E2E_TIMES=1 ...                                # each check's time, each section's time by what it went to
```

From `pc-tool/`, the same with cargo: `cargo test --release --test e2e -- oauth menu`
(the simulator must be built, and `SIM=path` picks another one).

The suite is a Rust test of the PC tools with its own runner (`main.rs`). It
boots headless simulators and drives them with the CLI, as a user would. Each
section (`sections/`) has its own simulator, work folder and ports, and all
run in parallel, each in a thread. The whole suite takes about two seconds.
A section over the time limit fails.

The servers the device talks to are fakes in the same process (`fakes/`),
each on a free port, logging every event as a line the tests look for:

| Fake | Stands in for |
|---|---|
| `mqtt.rs` | An MQTT 3.1.1 broker (QoS 0/1, retained messages, last will, a "silent" mode that hangs) |
| `oauth.rs` | An OAuth provider with a small API, plus chunked, close-delimited and large HTTP bodies |
| `marketplace.rs` | The app marketplace: its index and the apps' files |
| `releases.rs` | GitHub's release downloads: the manifest and the image, behind redirects |
| `http.rs` | The small HTTP/1.1 server the fakes are built on, and the tests' own client |
| `tls.rs` | A self-signed certificate for 127.0.0.1, for the HTTPS and MQTT over TLS servers |

Logins run without a browser: the test executable itself is the browser
(`e2e --browse URL` fetches the address and follows its redirects).

Tests don't sleep. They wait for what they check (a reply, a log line, the
screen) with `until`, and the device's own timers (button holds, timeouts,
keepalives, back-off) run in the simulator's time, which a test moves ahead
instead of waiting it out (`harness.rs`):

| Helper | What it does |
|---|---|
| `t.until(secs, cond)` | Retry the condition for SECS of real time, the device's clock running at 20 times real time meanwhile |
| `t.until_real(secs, cond)` | The same with the device's clock in real time |
| `t.advance(ms)` | Fast-forward the device's clock by MS: it runs through them tick by tick in milliseconds |
| `t.until_sim(secs, cond)` | Retry for SECS of the device's time, 100 ms ahead between checks: for what the device does by its own clock |
| `t.until_net(secs, cond)` | `until_sim` with a real 5 ms between the steps: for what a fake server has a part in |
| `t.press(b)`, `t.press_for(b, ms)`, `t.tap_at(x, y)` | Hold a button (default 150 ms) or touch the screen, in device time |
| `t.ctl(&[lines])` | Any simulator control line (`battery=55`, `power=short`, ...), waited for |
| `t.is(state)` | Is the board `"on"`, `"off"` (powered down) or `"light"` (asleep with light sleep allowed)? |
| `t.wait_off()` | Wait (up to 2 s) for the board to have powered down, after the firmware said it would |
| `t.ask(line)` | Any question on the control channel (`is=off`, `pwm=8:1000:250`): true for yes |
| `t.e(&[args])` | The CLI on this section's simulator: its output and errors as one text |
| `t.e_ahead(&[args], from)` | `e` for a call that waits for the device (an update's reboot): once the CLI has printed FROM the device's clock runs ahead |
| `t.eval(expr)`, `t.info(key)` | What `luaboard eval` prints and a field of INFO, asked from the test program itself (no CLI process) |
| `t.eval_skipping(expr, ms)` | `eval` for a call that never waits (a busy loop): once it runs, the device's clock skips MS ahead |
| `t.shot(name)` | The simulator's LCD as a picture (the simulator writes it, but the CLI's `screenshot` sends the frame over the serial link) |
| `t.check(cond, ok, bad)` | Count a check. The failure message is made only when it fails |

A fast-forward waits for a fake server's answer to what the device sent,
but not for the rest of an answer that comes in pieces, and not for a
server that speaks first. Where the device's timeout for a server matters,
wait with `until` or `until_net`, which leave the server real time.

A section should take well under a second. `E2E_TIMES=1` shows where one's
time goes. What costs real time: starting the CLI (5 ms and more each, far
more when every section does it at once), a wait for something that never
comes (wait for what the device says instead, and look after an `advance`),
and a device-side delay that nothing fast-forwards.

No test depends on the real date: every simulator starts at the same fixed
time (`E2E_TIME`, Fri 2026-11-06 13:30 UTC), and the fakes use it too.

Each simulator's control channel is a TCP port on 127.0.0.1 it picks itself
(`--control 0`), and a simulator is stopped with the control line `quit=1`,
the same on every system. Only its serial port differs: a pty on macOS and
Linux, a named pipe on Windows (`harness.rs` does all this).

`data/` holds what the tests hand out: `cover.jpg` (a 300x300 JPEG the fake
web server serves) and `ble_device.json` (a simulated Bluetooth device).

The tests are about the firmware, the simulator and the CLI. Of the
marketplace's apps, only that each one starts and draws is checked (the apps
section).
