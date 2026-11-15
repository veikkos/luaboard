//! Asleep on battery: light sleep is allowed, everything keeps running, a
//! touch wakes it; `disp.sleep`.

use crate::harness::{ui, T};

const SCRIPT: &str = r#"N, TAPS, FIRED = 0, 0, nil
function loop() N = N + 1 end
touch.on("tap", function() TAPS = TAPS + 1 end)
function arm() local t0 = sys.millis(); timer.after(5000, function() FIRED = sys.millis() - t0 end) end
"#;

fn screen(t: &T) -> String {
    t.e(&["info"]).lines().find_map(|l| l.trim().strip_prefix("screen:")).unwrap_or("").trim().to_string()
}

/// The value: the last line (the device's notices may come before it).
fn num(t: &T, expr: &str) -> i64 {
    t.eval(expr).lines().last().unwrap_or("").parse().unwrap_or(-999)
}

pub fn run(t: &mut T) {
    t.say("== asleep on battery: light sleep, and disp.sleep");
    t.wipe();
    t.boot_with(&["--model", "cores3", "--set", "usb=0"]);
    t.write("sleeper/main.lua", SCRIPT);
    t.write("sleeper/app.json", r#"{ "title": "Sleeper", "version": "1.0", "lua_api": 1 }"#);
    t.e(&["install", &t.path("sleeper"), "--run"]); // installed: it can start again after a power-off
    t.until(2.0, |t| t.e(&["info"]).contains("running: sleeper/main.lua"));

    let v = t.eval("tostring(disp.sleep())");
    t.check(v == "false", "awake at start", |_| format!("disp.sleep(): {v}"));
    let light = t.is("light");
    t.check(!light, "no light sleep while awake", |_| "light sleep allowed while awake".into());

    t.eval("arm() disp.sleep(true)");
    t.advance(100);
    let ok = screen(t) == "off" && t.is("light");
    t.check(ok, "disp.sleep(true): asleep, light sleep allowed", |t| {
        format!("screen {}, light {}", screen(t), t.is("light"))
    });
    // No rates or upper bounds here: the simulator's clock runs in real time
    // too, so a stalled host moves it on and the firmware drops the ticks it
    // fell behind by.
    t.eval("N0 = N");
    t.advance(1000);
    let n = num(t, "N - N0");
    t.check(n > 0, "loop() still runs while asleep", |_| format!("loop() calls in a second: {n}"));
    t.advance(4200);
    let f = num(t, "FIRED or -1");
    t.check(f >= 5000, "a timer fires while asleep, not early", |_| format!("fired after {f} ms"));

    t.ctl(&["usb=1", "advance=1100"]);
    let ok = !t.is("light") && screen(t) == "off";
    t.check(ok, "USB plugged in: still asleep, no light sleep", |t| {
        format!("screen {}, light {}", screen(t), t.is("light"))
    });
    t.ctl(&["usb=0", "advance=1100"]);
    let ok = t.is("light");
    t.check(ok, "unplugged: light sleep again", |_| "no light sleep after unplugging".into());

    t.tap_at(160, 120);
    let ok = screen(t) == "on" && !t.is("light") && t.eval("TAPS") == "0";
    t.check(ok, "a touch wakes it and isn't a tap", |t| format!("screen {}, taps {}", screen(t), t.eval("TAPS")));

    t.eval("disp.sleep(true)");
    t.advance(100);
    let was = screen(t);
    t.eval("disp.sleep(false)");
    let ok = was == "off" && t.eval("tostring(disp.sleep())") == "false" && screen(t) == "on";
    t.check(ok, "disp.sleep(false) wakes it", |t| format!("was {was}, now {}", screen(t)));

    t.say("   the Turn off after setting, and sys.keep_on");
    t.ctl(&["power=short", "advance=300"]); // home
    t.tap_at(60, ui::SETTINGS); // Settings
    t.tap_at(60, ui::row(3)); // Turn off after: never -> 5 min
    t.e(&["run", "sleeper"]);
    t.until(2.0, |t| t.e(&["info"]).contains("running: sleeper/main.lua"));
    let v = t.eval("tostring(sys.keep_on())");
    t.check(v == "false", "keep_on is off at start", |_| format!("sys.keep_on(): {v}"));
    t.eval("sys.keep_on(true) disp.sleep(true)");
    t.advance(301_000);
    let ok = t.is("light");
    t.check(ok, "kept on: asleep past the time, still on", |t| format!("on {}, light {}", t.is("on"), t.is("light")));
    t.ctl(&["usb=1"]);
    t.eval("sys.keep_on(false)");
    t.advance(2_000);
    let ok = t.is("on");
    t.check(ok, "on USB power the setting does nothing", |_| "it turned off on USB power".into());
    t.ctl(&["usb=0", "advance=2000"]);
    let ok = t.until(2.0, |t| t.read("sim.err").contains("off wake=none by=setting"));
    let off = t.wait_off();
    t.check(ok && off, "on battery, not kept on: it turns off", |_| format!("event {ok}, off {off}"));

    t.say("   coming back: the app again, safe mode skipping it, USB turning the board on");
    t.ctl(&["power=short"]);
    let ok = t.until(3.0, |t| t.eval("sys.info().boot") == "power");
    let app = t.e(&["info"]).contains("running: sleeper/main.lua");
    t.check(ok && app, "the power key: the app the setting turned off under starts again", |t| {
        format!("boot {}, info: {}", t.eval("sys.info().boot"), t.e(&["info"]))
    });
    t.eval("sys.off()");
    let off = t.wait_off();
    t.ctl(&["touch=60,60", "power=short"]); // a finger held as it comes on
    let ok = t.until(3.0, |t| t.eval("sys.info().boot") == "power");
    t.ctl(&["touch=-", "advance=300"]);
    let menu = t.e(&["info"]).contains("running: -");
    t.check(off && ok && menu, "a finger held at power-on: the menu, not the app", |t| {
        format!("off {off}, on {ok}, info: {}", t.e(&["info"]))
    });
    t.eval("sys.off()");
    let off = t.wait_off();
    t.ctl(&["usb=1"]);
    let ok = t.until(3.0, |t| t.eval("sys.info().boot") == "power");
    t.check(off && ok, "USB plugged into a board that is off turns it on", |_| format!("off {off}, on {ok}"));
    t.stop();
}
