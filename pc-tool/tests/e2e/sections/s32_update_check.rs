//! The update check: the device looks for a newer release a minute after
//! Wi-Fi comes up, then once a day, at a fake GitHub release server (HTTPS,
//! trusted through SIM_CA_FILE).
//!
//! The simulator's "day" between checks is shortened (--set update_every):
//! 24 hours of fast-forward take the simulator over a minute.

use super::ota::{info, Raw};
use super::update::{bin_url, boot, check_times, checks, next_check, releases, said, wifi_up, DAY};
use crate::fakes::releases::manifest;
use crate::harness::T;

/// A day passes and its check fails, watched over a `luaboard raw` session:
/// the device's events, through "EVT update check-failed". (No fixed time
/// of listening: a busy computer makes the check late, not missed.)
fn next_check_events(t: &mut T) -> String {
    let mut raw = Raw::start(t);
    raw.send(b"\nPING\n");
    t.until(2.0, |_| raw.pongs() >= 1);
    let n = checks(t);
    t.advance(DAY);
    t.until(5.0, |t| checks(t) > n);
    t.until(2.0, |_| raw.has("EVT update check-failed"));
    let text = raw.text();
    raw.finish();
    text
}

pub fn run(t: &mut T) {
    t.say("== the update check against a fake release server");
    let (rel, url) = releases(t);
    let log = rel.log.clone();
    let img = b"not really firmware".to_vec();
    let bin = |v: &str| bin_url(&rel, v);
    rel.set_manifest(&manifest("v1.1.0", &bin("v1.1.0"), &img, 1, 1));

    t.wipe();
    boot(t, "v1.0.0", Some(&url));
    t.e(&["wifi", "set", "Home Net", "pw"]);
    let up = wifi_up(t);
    t.check(up, "Wi-Fi joins", |t| format!("info: {}", t.e(&["info"])));

    t.advance(30_000);
    t.check(!log.has("LATEST"), "no check in the first minute", |_| format!("log: {}", log.tail(3)));
    t.advance(35_000);
    let ok = t.until(5.0, |t| info(t, "update") == "v1.1.0");
    let lines = log.lines();
    let latest = lines.iter().position(|l| l == "LATEST");
    let object = lines.iter().position(|l| l == "OBJECT manifest.json");
    let ok = ok && matches!((latest, object), (Some(a), Some(b)) if a < b);
    t.check(ok, "checks a minute after Wi-Fi", |t| {
        format!("update {} / log: {} / device: {}", info(t, "update"), log.tail(4), said(t))
    });

    t.write("count.lua", "n = 0\nfunction loop() n = n + 1 end\n");
    t.e(&["put", &t.path("count.lua"), "--run", "count.lua"]);
    t.wait_lua(3.0, "n > 0");
    let n0: u64 = t.eval("n").parse().unwrap_or(0);
    let done = next_check(t);
    t.advance(2000);
    let n1: u64 = t.eval("n").parse().unwrap_or(0);
    // loop() runs 30 times a second: a stalled main loop would lose calls
    let want = (DAY + 2000) * 30 / 1000;
    let ok = done && n1 >= n0 + want * 9 / 10 && !t.e(&["info"]).contains("running: -");
    t.check(ok, "an app keeps running during a check", |_| {
        format!("check done {done}, loop calls {n0} -> {n1}, want about {want} more")
    });
    t.e(&["stop"]);

    rel.fail_next(2);
    rel.set_manifest(&manifest("v1.2.0", &bin("v1.2.0"), &img, 1, 1));
    let base = checks(t);
    let ev = next_check_events(t);
    let ver = info(t, "update");
    let ok = ev.contains("update check-failed") && ver == "v1.1.0";
    t.check(ok, "a failed check is silent and keeps what it knew", |t| {
        format!("update {ver}, events: {ev} / device: {}", said(t))
    });
    // It comes again in 10 minutes, then in an hour: a 144th and a 24th of
    // the day, which is short here. The device's own clock tells when.
    let (soon, later) = (DAY / 144, DAY / 24);
    t.advance(soon + 500);
    t.until(5.0, |t| checks(t) >= base + 2);
    t.advance(later + 500);
    let found = t.until(5.0, |t| checks(t) >= base + 3) && info(t, "update") == "v1.2.0";
    let at = check_times(t);
    let gaps: Vec<u64> = at.windows(2).skip(base).map(|w| w[1] - w[0]).collect();
    let ok = found && matches!(gaps[..], [a, b] if a >= soon && a < later && b >= later && b < DAY / 2);
    t.check(ok, "a failed check is retried in 10 minutes, then in an hour", |t| {
        format!("found {found}, ms between the checks {gaps:?}, want about {soon} and {later} / device: {}", said(t))
    });

    let other_board = manifest("v1.3.0", &bin("v1.3.0"), &img, 1, 1).replace("m5stack-cores3", "other-board");
    let variants = [
        ("same version", manifest("v1.0.0", &bin("v1.0.0"), &img, 1, 1)),
        ("older", manifest("v0.9.0", &bin("v0.9.0"), &img, 1, 1)),
        ("other board", other_board),
    ];
    let mut got = Vec::new();
    for (what, m) in &variants {
        rel.set_manifest(m);
        let done = next_check(t);
        got.push(format!("{what}: {} ({})", info(t, "update"), if done { "checked" } else { "no check" }));
    }
    let ok = got.iter().all(|g| g.ends_with(": - (checked)"));
    t.check(ok, "not offered: same version, older, other board", |_| got.join(", "));
    let at = check_times(t);
    let gap = at.get(base + 3).map_or(0, |next| next - at[base + 2]);
    t.check(gap >= DAY, "a good check is followed by one a day later", |_| format!("{gap} ms later, a day is {DAY}"));

    let mut big: serde_json::Value = serde_json::from_str(&manifest("v1.3.0", &bin("v1.3.0"), &img, 1, 1)).unwrap();
    big["extra"] = serde_json::Value::String("x".repeat(10 * 1024));
    rel.set_manifest(&big.to_string());
    let ev = next_check_events(t);
    let ok = ev.contains("update check-failed manifest too big") && t.eval("1+1") == "2";
    t.check(ok, "a manifest bigger than 4 KB fails the check", |_| format!("events: {ev}"));

    rel.set_manifest(&manifest("v1.3.0", &bin("v1.3.0"), &img, 1, 1));
    rel.redirect_loop(true);
    let ev = next_check_events(t);
    let ok = ev.lines().any(|l| l.contains("update check-failed") && l.contains("redirects"));
    t.check(ok, "a redirect loop fails the check", |_| format!("events: {ev}"));
    rel.redirect_loop(false);

    t.stop();
    rel.set_manifest(&manifest("v1.0.0", &bin("v1.0.0"), &img, 1, 1));
    let before = log.count("OBJECT manifest.json");
    boot(t, "v1.0.0-5-gabc123", Some(&url));
    wifi_up(t);
    t.advance(65_000);
    let done = t.until(5.0, |t| checks(t) >= 1);
    let (ver, running) = (info(t, "update"), info(t, "ver"));
    let ok = done && log.count("OBJECT manifest.json") > before && ver == "-" && running == "v1.0.0-5-gabc123";
    t.check(ok, "a dev build isn't offered its own release", |_| {
        format!("checked {done}, running {running}, update {ver}, log: {}", log.tail(3))
    });

    t.stop();
    let before = log.count("LATEST");
    boot(t, "v1.0.0", None);
    let up = wifi_up(t);
    t.advance(60_000);
    t.advance(60_000); // (a check in these minutes would be over: a fast-forward waits for the server's answer)
    let ok = up && log.count("LATEST") == before && checks(t) == 0;
    t.check(ok, "no address set: no check", |_| format!("Wi-Fi {up}, log: {}", log.tail(3)));
    t.stop();
}
