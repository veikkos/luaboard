//! The timer example: setting it, running, pausing, the alarm, and what it
//! keeps across app restarts and reboots (store).

use crate::harness::T;

/// The middle of a tappable zone of the app (TIMER.zones), e.g. "m+".
fn zone(t: &T, id: &str) -> (i32, i32) {
    let v = t.eval(&format!("local z = TIMER.zones['{id}'] return (z.x + z.w // 2) .. ',' .. (z.y + z.h // 2)"));
    let (x, y) = v.split_once(',').unwrap_or_else(|| panic!("no zone {id}: {v}"));
    (x.parse().unwrap(), y.parse().unwrap())
}

fn tap_zone(t: &mut T, id: &str) {
    let (x, y) = zone(t, id);
    t.tap_at(x, y);
}

/// Seconds left, rounded up as the screen shows them.
const LEFT: &str = "(TIMER.remaining() + 999) // 1000";

pub fn run(t: &mut T) {
    t.say("== example timer: a countdown that keeps its settings");
    t.wipe();
    t.boot();
    t.e(&["wifi", "set", "Home Net", "pw"]); // the clock (NTP), to keep a running timer
    t.wait_lua(3.0, "sys.time() ~= nil");
    let app = t.root.join("marketplace/timer").to_string_lossy().into_owned();
    let out = t.e(&["install", &app, "--run"]);
    t.check(!out.contains("Error"), "installs and runs", |_| out.clone());
    t.advance(200);
    let v = t.eval("TIMER.mode .. ' ' .. TIMER.set");
    t.check(v == "set 300", "starts on the set screen at 5:00", |_| format!("mode, set: {v}"));

    tap_zone(t, "m+");
    t.press("C");
    t.press_for("A", 1000);
    tap_zone(t, "s-");
    let v = t.eval("TIMER.set");
    t.check(v == "179", "m+ 6:00, C 7:00, held A 2:00, s- wraps to 2:59", |_| format!("set: {v}"));
    let (x, y) = zone(t, "s+");
    t.ctl(&[&format!("touch={x},{y}"), "advance=1500", "touch=-", "advance=250"]);
    let v: i64 = t.eval("TIMER.set").parse().unwrap_or(0);
    t.check((124..=128).contains(&v), "holding s+ repeats (and wraps: 2:0x)", |_| format!("set: {v}"));
    t.press_for("A", 1000);
    t.press("C");
    t.press("C");
    let v = t.eval("TIMER.set");
    t.check(v == "120", "held A stops at 0:00, C C 2:00", |_| format!("set: {v}"));

    t.press("B");
    let v = t.eval(&format!("TIMER.mode .. ' ' .. {LEFT} .. ' ' .. store.get('recent')"));
    t.check(v == "run 120 [120]", "B starts it and remembers the time", |_| format!("mode, left, recent: {v}"));
    let v = t.eval("tostring(disp.keep_awake())");
    t.check(v == "true", "the screen stays on while it runs", |_| format!("keep_awake: {v}"));
    t.advance(30_000);
    t.press("B");
    let v = t.eval(&format!("TIMER.mode .. ' ' .. {LEFT}"));
    t.check(v == "pause 90", "30 s later B pauses at 1:30", |_| format!("mode, left: {v}"));
    t.advance(10_000);
    let v = t.eval(LEFT);
    t.check(v == "90", "paused, it doesn't count", |_| format!("left: {v}"));

    t.e(&["run", "timer"]);
    t.advance(200);
    let v = t.eval(&format!("TIMER.mode .. ' ' .. {LEFT} .. ' ' .. TIMER.total"));
    t.check(v == "pause 90 120", "the app again: still paused at 1:30 of 2:00", |_| format!("mode, left, total: {v}"));
    t.press("B");
    t.advance(20_000);
    t.e(&["run", "timer"]);
    t.advance(200);
    let v: i64 = t.eval(LEFT).parse().unwrap_or(0);
    let mode = t.eval("TIMER.mode");
    t.check(mode == "run" && (68..=71).contains(&v), "running, the app again: still running (~70 s left)", |_| {
        format!("mode {mode}, left {v}")
    });

    t.press("C");
    let v: i64 = t.eval(LEFT).parse().unwrap_or(0);
    t.check((128..=131).contains(&v), "C adds a minute", |_| format!("left {v}"));
    t.advance(131_000);
    let mode = t.eval("TIMER.mode");
    // Three beeps at the start of each second: look over one.
    let mut beeps = 0;
    for _ in 0..10 {
        t.advance(100);
        if t.eval("tostring(sound.playing())") == "true" {
            beeps += 1;
        }
    }
    t.check(mode == "done" && beeps > 0, "when it's up: done, beeping", |_| format!("mode {mode}, beeping {beeps}/10"));
    t.advance(61_000);
    let v = t.eval("tostring(sound.playing()) .. ' ' .. tostring(disp.keep_awake())");
    t.check(v == "false false", "a minute later the beeping stops", |_| format!("sound, keep_awake: {v}"));
    t.tap_at(160, 120);
    let v = t.eval("TIMER.mode .. ' ' .. TIMER.set .. ' ' .. tostring(store.get('run'))");
    t.check(v == "set 120 nil", "a tap: back to the set screen at 2:00", |_| format!("mode, set, run: {v}"));

    let (sx, sy) = zone(t, "s+");
    t.press("B");
    t.advance(121_000);
    t.press("B");
    t.advance(1500);
    let v = t.eval("TIMER.mode .. ' ' .. TIMER.set .. ' ' .. tostring(sound.playing())");
    t.check(v == "set 120 false", "while it beeps, B is seen too: back at 2:00, quiet", |_| {
        format!("mode, set, sound: {v}")
    });
    t.press("B");
    t.advance(121_000);
    t.ctl(&[&format!("touch={sx},{sy}"), "advance=2500", "touch=-", "advance=250"]);
    let v = t.eval("TIMER.mode .. ' ' .. TIMER.set");
    t.check(v == "set 120", "a hold on where s+ will be: seen, and the time stays 2:00", |_| format!("mode, set: {v}"));

    tap_zone(t, "m+");
    t.press("B");
    t.press("A");
    let v = t.eval("TIMER.mode .. ' ' .. store.get('recent')");
    t.check(v == "set [180,120]", "3:00 started and reset: recent 3:00, 2:00", |_| format!("mode, recent: {v}"));

    // store writes at most once a second, and stop() is a power cut to the
    // firmware: the reset (deleting the kept timer) has to land first.
    t.advance(1100);
    t.stop();
    t.boot_with(&["--model", "cores3", "--set", "usb=0"]); // on battery from here
    t.e(&["run", "timer"]);
    t.advance(200);
    let v = t.eval("TIMER.set .. ' ' .. TIMER.recent[1] .. ' ' .. TIMER.recent[2]");
    t.check(v == "180 180 120", "after a reboot: the time set and the recent ones", |_| format!("set, recent: {v}"));
    tap_zone(t, "r2");
    let v = t.eval("TIMER.set");
    t.check(v == "120", "a tap on a recent time sets it", |_| format!("set: {v}"));

    t.say("   on battery: it lets the device sleep, turns it off for a long wait, and still rings");
    t.wait_lua(3.0, "sys.time() ~= nil");
    t.press("B"); // start 2:00
    let v = t.eval("tostring(disp.keep_awake()) .. ' ' .. tostring(sys.keep_on())");
    t.check(v == "false true", "on battery: the screen may sleep, the device stays on", |_| {
        format!("keep_awake, keep_on: {v}")
    });
    t.advance(70_000);
    t.eval("disp.sleep(true)");
    t.advance(1_000);
    let ok = t.is("light");
    t.check(ok, "under a minute left: asleep, not off", |t| format!("on {}, light {}", t.is("on"), t.is("light")));
    t.advance(50_000);
    let v = t.eval("TIMER.mode");
    let screen = t.e(&["info"]).contains("screen: on");
    t.check(v == "done" && screen, "it rings on time and wakes the screen", |_| {
        format!("mode {v}, screen on {screen}")
    });
    t.tap_at(160, 120);
    t.press("B"); // 2:00 again
    t.eval("disp.sleep(true)");
    // 119 or 118 s are left; less the boot's 5
    let ok = t.until_sim(2.0, |t| t.read("sim.err").contains("off wake=11"));
    let off = t.wait_off();
    t.check(ok && off, "a minute or more left: it turns the device off until then", |_| {
        format!("event {ok}, off {off}")
    });
    t.advance(115_000); // the wake-up time, a few seconds before the end
    let ok = t.until(3.0, |t| t.eval("sys.info().boot") == "timer");
    let v = t.eval("TIMER.mode");
    t.check(ok && v == "run", "the clock chip turns it on before the end: the timer runs on", |t| {
        format!("boot {}, mode {v}", t.eval("sys.info().boot"))
    });
    t.advance(8_000);
    let v = t.eval("TIMER.mode .. ' ' .. tostring(sys.millis() - TIMER.done_at < 60000)");
    t.check(v == "done true", "and rings at the end", |_| format!("mode, ringing: {v}"));
    t.stop();
}
