//! The RTC chip keeps the time over a restart.

use crate::harness::{E2E_TIME, RESTART_NOTICE_S, T};

pub fn run(t: &mut T) {
    t.say("== RTC chip keeps the time over a restart");
    t.wipe();
    t.boot();
    t.e(&["wifi", "set", "Home Net", "pw"]);
    t.wait_lua(3.0, "sys.time() ~= nil");
    // the main loop writes the chip after the sync
    t.until(2.0, |t| t.work.join("nvs.bin.rtc").metadata().is_ok_and(|m| m.len() > 0));
    t.crash();
    t.boot_with(&["--set", "wifi=0"]); // restart, no network: only the chip knows the time
                                       // the first reply after boot can carry "time rtc"
    let out = t.eval("sys.time() or 0");
    let last = out.lines().rfind(|l| !l.starts_with("[evt]")).unwrap_or("");
    let secs: i64 = last.trim().parse().unwrap_or(0);
    // The world starts at E2E_TIME again; the chip was set from it: it agrees
    // (after the Restarted screen, which boot() ran past).
    let dt = (secs - E2E_TIME as i64 - RESTART_NOTICE_S).abs();
    let time = t.e(&["time"]);
    t.check(dt <= 5 && time.contains("RTC chip"), "time from the RTC chip after a restart, without Wi-Fi", |t| {
        format!("rtc: off by {dt} s, {}", t.e(&["time"]))
    });
    t.crash();
    t.boot_with(&["--set", "wifi=0", "--set", "rtc=lost"]); // the chip lost power: its time isn't trusted
    let v = t.eval("sys.time()");
    t.check(v == "nil", "RTC that lost power is ignored", |t| format!("lost RTC used: {}", t.eval("sys.time()")));
    let v = t.e(&["time", "--sync"]);
    t.check(v.contains("no network"), "time --sync without Wi-Fi says so", |_| "sync without network".into());
    t.crash();
}
