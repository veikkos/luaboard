//! Off on USB power: `sys.off` is acted out by the firmware, so the PC link
//! and code that was only uploaded stay.

use crate::harness::{ui, T};

const SCRIPT: &str = r#"print("usboff", sys.info().boot)
function loop() end
"#;

fn field(t: &T, name: &str) -> String {
    let key = format!("{name}:");
    t.e(&["info"]).lines().find_map(|l| l.trim().strip_prefix(key.as_str())).unwrap_or("").trim().to_string()
}

pub fn run(t: &mut T) {
    t.say("== off on USB power: acted out, the PC link and uploaded code stay");
    t.wipe();
    t.boot_with(&["--model", "cores3"]); // a battery, and USB power
    t.write("usboff/main.lua", SCRIPT);
    t.e(&["put", &t.path("usboff/main.lua"), "--app", "usboff", "--run", "main.lua"]);
    t.until(2.0, |t| field(t, "running") == "usboff/main.lua");

    t.eval("sys.off(60)");
    let ok = t.until(2.0, |t| t.read("sim.err").contains("off wake=55 by=app"));
    let (on, screen, running) = (t.is("on"), field(t, "screen"), field(t, "running"));
    t.check(
        ok && on && screen == "off" && running == "-",
        "the app stops, the screen goes off, the board stays up and answers",
        |_| format!("event {ok}, on {on}, screen {screen}, running {running}"),
    );
    t.advance(50_000);
    let running = field(t, "running");
    t.check(running == "-", "not back before the time", |_| format!("running {running}"));
    t.advance(6_000);
    let ok = t.until(2.0, |t| field(t, "running") == "usboff/main.lua");
    let (boot, screen) = (t.eval("sys.info().boot"), field(t, "screen"));
    t.check(
        ok && boot == "timer" && screen == "on",
        "at the time: the uploaded code runs again, boot reason timer",
        |t| format!("running {}, boot {boot}, screen {screen}", field(t, "running")),
    );

    t.eval("sys.off()");
    t.advance(200);
    let stopped = field(t, "running") == "-";
    t.tap_at(160, 120);
    let dark = field(t, "screen") == "off";
    t.check(stopped && dark, "sys.off() with no time: a touch doesn't bring it back", |t| {
        format!("stopped {stopped}, screen {}", field(t, "screen"))
    });
    t.ctl(&["power=short", "advance=300"]);
    let ok = t.until(2.0, |t| field(t, "running") == "usboff/main.lua");
    let boot = t.eval("sys.info().boot");
    t.check(ok && boot == "power", "the power key ends it: the app again, boot reason power", |t| {
        format!("running {}, boot {boot}", field(t, "running"))
    });

    // The power key pressed from the PC (KEY POWER) is a press like any other.
    t.eval("sys.off()");
    t.advance(200);
    let out = t.raw(&["KEY POWER"]);
    let ok = t.until(2.0, |t| field(t, "running") == "usboff/main.lua");
    t.check(out.contains("OK KEY POWER") && ok, "the power key from the PC ends it too", |t| {
        format!("{out}; running {}, screen {}", field(t, "running"), field(t, "screen"))
    });

    // Safe mode: a finger held on the screen as the key is pressed gives the
    // menu instead (an app that turns the device off at every start could
    // otherwise only be left by unplugging). Letting go taps nothing.
    t.eval("sys.off()");
    t.advance(200);
    t.ctl(&[&format!("touch=60,{}", ui::row(0)), "power=short", "advance=300", "touch=-", "advance=300"]);
    let (running, screen) = (field(t, "running"), field(t, "screen"));
    t.tap_at(60, ui::row(0)); // the app's row: a tap of its own does start it
    let tapped = field(t, "running");
    t.check(
        running == "-" && screen == "on" && tapped == "usboff/main.lua",
        "a finger held as the key is pressed: the menu, not the app",
        |_| format!("running {running}, screen {screen}, a tap there after: {tapped}"),
    );

    t.eval("sys.off(600)");
    t.advance(200);
    t.ctl(&["usb=0", "advance=1100"]); // USB power is read once a second
    let off = t.wait_off();
    t.check(off, "USB unplugged meanwhile: it powers down for real", |_| "still on".into());
    t.stop();
}
