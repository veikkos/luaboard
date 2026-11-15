//! The crash-loop guard: an app that crashes the device at every boot stops
//! being started.

use crate::harness::{T, WINDOWS};

/// The Restarted screen's orange bar at the top.
const ORANGE_TOP: &str = "local c = disp.get(10, 1) return (c >> 16) > 200 and (c >> 8) & 255 > 140 and c & 255 < 100";

fn running_t(t: &T) -> bool {
    t.e(&["info"]).contains("running: t/main.lua")
}

/// SIGTERM is a clean power-off; a crash leaves its cause and stack trace
/// for the next boot to show.
#[cfg(unix)]
fn crash_record(t: &mut T) {
    t.say("   SIGTERM powers off cleanly");
    t.wipe();
    t.boot();
    let _ = std::fs::remove_file(t.work.join("screen.ppm"));
    let ok = t.term() && t.work.join("screen.ppm").exists();
    t.check(ok, "SIGTERM ends the simulator and it writes its screen", |_| "no clean exit on SIGTERM".into());
    t.boot();
    let info = t.e(&["info"]);
    t.check(info.contains("reset: poweron"), "the next boot isn't a crash reset", |_| format!("info: {info}"));
    t.stop();

    t.say("   a crash's record: its cause and stack trace");
    t.wipe();
    t.boot();
    t.advance(1000); // (a crash in the first second of a boot waits 0.3 s before the next: no spinning crash loop)
    t.segv();
    let ok = t.until(5.0, |t| t.e(&["info"]).contains("reset: panic"));
    t.check(ok, "a real crash restarts the chip", |t| format!("info: {}", t.e(&["info"])));
    let out = t.e(&["crash"]);
    let ok = out.contains("last crash: SIGSEGV") && out.lines().filter(|l| l.starts_with("   ")).count() >= 1;
    t.check(ok, "luaboard crash shows its cause and stack trace", |_| format!("crash: {out}"));
    let ok = t.eval(ORANGE_TOP) == "true";
    t.check(ok, "... and the Restarted screen its cause", |_| "no Restarted screen".into());
    t.stop();
    t.boot();
    let out = t.e(&["crash"]);
    t.check(out.starts_with("no crash"), "a boot that wasn't after a crash has nothing to tell", |_| {
        format!("crash: {out}")
    });
    t.stop();
}

pub fn run(t: &mut T) {
    #[cfg(unix)]
    crash_record(t);
    let _ = WINDOWS;
    t.say("== crash-loop guard");
    t.wipe();
    t.boot();
    t.write("main.lua", "function loop() end\n");
    t.e(&["put", &t.path("main.lua"), "--app", "t", "--save"]);
    t.e(&["autorun", "t"]);
    t.stop();
    for _ in 0..3 {
        // kill -9 (like a panic) right after starting the app
        t.boot();
        t.until(3.0, running_t); // (the harness runs past the Restarted screen)
        t.crash();
    }
    t.keep_restart_notice = true; // this boot's Restarted screen is checked
    t.boot();
    t.keep_restart_notice = false;
    let info = t.e(&["info"]);
    t.check(info.contains("reset: panic"), "INFO says the last reset was a crash", |_| format!("info: {info}"));
    let ok = t.eval(ORANGE_TOP) == "true";
    t.check(ok, "the screen says it restarted after a crash", |t| format!("top: {}", t.eval("disp.get(10, 1)")));
    t.advance(5000);
    let ok = t.eval(ORANGE_TOP) == "true";
    t.check(ok, "... and stays up for a while", |_| "gone before 10 s".into());
    t.advance(5500); // past its 10 s
    let ok = t.until(3.0, |t| t.e(&["info"]).contains("running: -") && t.eval(ORANGE_TOP) == "false");
    t.check(ok, "autostart skipped after 3 crashed boots", |_| "crash loop not detected".into());
    t.stop();
    t.boot();
    let ok = t.until(3.0, running_t);
    t.check(ok, "next boot runs the app again", |_| "guard not reset".into());
    t.stop();

    t.say("   a tap on the Restarted screen goes on to the autostart app");
    t.boot();
    t.crash();
    t.keep_restart_notice = true;
    t.boot();
    t.keep_restart_notice = false;
    // (boot returns before the firmware is up on a busy machine: tap once the screen is there)
    t.until(3.0, |t| t.eval(ORANGE_TOP) == "true");
    t.tap_at(160, 120);
    let ok = t.until(3.0, running_t);
    t.check(ok, "the tap starts the app, it isn't a safe-mode touch", |t| format!("info: {}", t.e(&["info"])));

    t.say("   an app the PC starts on the Restarted screen keeps running");
    t.e(&["put", &t.path("main.lua"), "--app", "u", "--save"]);
    t.crash();
    t.keep_restart_notice = true;
    t.boot();
    t.keep_restart_notice = false;
    t.e(&["run", "u"]);
    t.advance(500);
    let info = t.e(&["info"]);
    t.check(info.contains("running: u/main.lua"), "it isn't replaced by the autostart app", |_| {
        format!("info: {info}")
    });
    t.e(&["autorun", "off"]);
    t.crash();
    t.keep_restart_notice = true;
    t.boot();
    t.keep_restart_notice = false;
    t.e(&["run", "u"]);
    t.advance(500);
    let info = t.e(&["info"]);
    t.check(info.contains("running: u/main.lua"), "... and with no autostart app the menu doesn't stop it", |_| {
        format!("info: {info}")
    });
    t.stop();

    t.say("   crashes long after the app started count too");
    t.wipe();
    t.boot();
    t.e(&["put", &t.path("main.lua"), "--app", "t", "--save"]);
    t.e(&["autorun", "t"]);
    t.stop();
    for _ in 0..3 {
        // past the old 10 s window, before the 10 min one
        t.boot();
        t.until(3.0, running_t);
        t.ctl(&["advance=20000"]);
        t.crash();
    }
    t.boot();
    let ok = t.e(&["info"]).contains("running: -");
    t.check(ok, "late crashes skip autostart too", |_| "late crash loop not detected".into());
    t.stop();

    t.say("   a clean power-off in between clears the count");
    for _ in 0..2 {
        t.boot();
        t.until(3.0, running_t);
        t.crash();
    }
    // crash reset, then a clean power-off
    t.boot();
    t.stop();
    for _ in 0..2 {
        t.boot();
        t.until(3.0, running_t);
        t.crash();
    }
    t.boot();
    let ok = t.until(3.0, running_t);
    t.check(ok, "count starts over after a clean power-off", |_| "clean power-off not seen".into());
    t.stop();
}
