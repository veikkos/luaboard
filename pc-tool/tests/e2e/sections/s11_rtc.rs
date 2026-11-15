//! The RTC chip keeps the time over a restart.

use crate::harness::{E2E_TIME, T};

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
    let out = t.eval("sys.time() or 0");
    let secs: i64 = out.lines().last().unwrap_or("").trim().parse().unwrap_or(0);
    // The world starts at E2E_TIME again and the chip was set from it during
    // the first boot: not earlier than that, and not wildly later. (How much
    // later depends on how long the first boot ran, so no tighter bound.)
    let ok = secs >= E2E_TIME as i64 && secs < E2E_TIME as i64 + 3600;
    let time = t.e(&["time"]);
    t.check(ok && time.contains("RTC chip"), "time from the RTC chip after a restart, without Wi-Fi", |t| {
        format!("rtc: {secs}, {}", t.e(&["time"]))
    });
    t.crash();
    t.boot_with(&["--set", "wifi=0", "--set", "rtc=lost"]); // the chip lost power: its time isn't trusted
    let v = t.eval("sys.time()");
    t.check(v == "nil", "RTC that lost power is ignored", |t| format!("lost RTC used: {}", t.eval("sys.time()")));
    let v = t.e(&["time", "--sync"]);
    t.check(v.contains("no network"), "time --sync without Wi-Fi says so", |_| "sync without network".into());
    t.crash();
}
