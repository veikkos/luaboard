//! Off on battery: `sys.off` powers the board down, the clock chip turns it
//! on again in time, and `sys.info().boot` says why it started.

use crate::harness::T;

const APP: &str = r#"BOOT = sys.info().boot
print("offtest", BOOT)
function loop() end
"#;

fn running(t: &T) -> String {
    t.e(&["info"]).lines().find_map(|l| l.trim().strip_prefix("running:")).unwrap_or("").trim().to_string()
}

pub fn run(t: &mut T) {
    t.say("== off on battery: sys.off, the timed wake-up, the boot reason");
    t.wipe();
    t.boot_with(&["--model", "cores3", "--set", "usb=0"]);
    t.write("offtest/main.lua", APP);
    t.write("offtest/app.json", r#"{ "title": "Off test", "version": "1.0", "lua_api": 1 }"#);
    let out = t.e(&["install", &t.path("offtest"), "--run"]);
    t.check(!out.contains("Error"), "the test app installs and runs", |_| out.clone());
    let v = t.eval("sys.info().boot");
    t.check(v == "power", "a fresh boot says power", |_| format!("boot: {v}"));

    // No clock yet (no Wi-Fi): a wait over 255 minutes needs it.
    for bad in ["9", "-1", "27 * 86400 + 1", "'x'", "16000"] {
        let v = t.eval(&format!("tostring((pcall(sys.off, {bad})))"));
        t.advance(100);
        let ok = v == "false" && t.is("on");
        t.check(ok, &format!("sys.off({bad}) is refused"), |_| format!("sys.off({bad}): pcall {v}"));
    }
    let v = t.eval("select(2, pcall(sys.off, 9))");
    t.check(v.contains("sys.off"), "the error names sys.off", |_| format!("error: {v}"));
    let v = t.eval("select(2, pcall(sys.off, 16000))");
    t.check(v.contains("needs the time"), "over 255 minutes with no clock: it says why", |_| format!("error: {v}"));

    t.e(&["wifi", "set", "Home Net", "pw"]);
    t.wait_lua(3.0, "sys.time() ~= nil");
    let t0: i64 = t.eval("sys.time()").parse().unwrap_or(0);
    t.eval("sys.off(600)"); // 595 s left: the minutes timer, 9 minutes
    let ok = t.until(2.0, |t| t.read("sim.err").contains("off wake=540 by=app"));
    let off = t.wait_off();
    t.check(ok && off, "sys.off(600): the off event, then the board is off", |t| {
        format!("event {ok}, off {off}: {}", t.read("sim.err").lines().last().unwrap_or(""))
    });
    t.advance(538_000);
    let still = t.is("off");
    t.check(still, "still off before the wake-up time", |_| "on too early".into());
    t.advance(3_000);
    let ok = t.until(3.0, |t| t.eval("sys.info().boot") == "timer");
    t.check(ok, "the clock chip turns it on: boot reason timer", |t| format!("boot: {}", t.eval("sys.info().boot")));
    let dt = t.eval("sys.time()").parse::<i64>().unwrap_or(0) - t0;
    // Not early; how much later depends on the host, so only a loose bound.
    t.check((540..600).contains(&dt), "about 541 s went by", |_| format!("seconds since sys.off: {dt}"));

    t.say("   coming back: the app that turned it off, then the power key into the menu");
    let ok = running(t) == "offtest/main.lua" && t.eval("BOOT") == "timer";
    t.check(ok, "the app that turned it off is running again", |t| {
        format!("running {}, BOOT {}", running(t), t.eval("BOOT"))
    });
    t.ctl(&["power=short", "advance=300"]); // home
    let home = running(t) == "-";
    t.eval("sys.off()"); // from the menu: no app to come back to
    let ok = t.until(2.0, |t| t.read("sim.err").contains("off wake=none by=app"));
    let off = t.wait_off();
    t.check(home && ok && off, "sys.off() with no time, from the menu", |_| {
        format!("home {home}, event {ok}, off {off}")
    });
    t.advance(3_600_000);
    let still = t.is("off");
    t.check(still, "an hour later it is still off", |_| "on by itself".into());
    t.ctl(&["power=short"]);
    let ok = t.until(3.0, |t| t.eval("sys.info().boot") == "power");
    let menu = running(t) == "-";
    t.check(ok && menu, "the power key turns it on, into the menu", |t| {
        format!("boot {}, running {}", t.eval("sys.info().boot"), running(t))
    });
    t.stop();

    t.say("   a clock chip that doesn't answer: the chip's own timer wakes the device");
    t.boot_with(&["--model", "cores3", "--set", "usb=0", "--set", "rtc=none"]);
    t.eval("sys.off(60)");
    let ok = t.until(2.0, |t| t.read("sim.err").contains("off wake=55 by=app"));
    let off = t.wait_off();
    t.advance(56_000);
    let on = t.is("on");
    let woke = on && t.until(3.0, |t| t.eval("sys.info().boot") == "timer");
    t.check(ok && off && woke, "it still turns itself on in time", |_| {
        format!("event {ok}, off {off}, on {on}, woke {woke}")
    });
    t.stop();
}
