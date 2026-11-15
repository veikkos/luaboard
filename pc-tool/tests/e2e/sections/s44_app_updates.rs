//! App updates from the marketplace: the device reads the marketplace's
//! index a minute after Wi-Fi comes up, then once a day (a fake, over HTTPS,
//! trusted through SIM_CA_FILE), and offers newer versions of installed apps.

use super::market::{self, app_json, next_check, said};
use super::ota::Raw;
use crate::harness::T;

const LOGO: &[u8] = b"\xff\xd8 the same logo";

fn install(t: &T, name: &str, version: &str, main: &str) {
    market::install(t, name, &app_json(name, version), main);
}

/// Ask the device to install `app` from the marketplace (APPINSTALL) and
/// wait for the outcome in its log: "ok" or "failed: <why>".
fn app_install(t: &mut T, app: &str) -> String {
    let mut raw = Raw::start(t);
    raw.send(format!("\nAPPINSTALL {app}\n").as_bytes());
    let started = raw.finish();
    if !started.contains(&format!("OK APPINSTALL {app}")) {
        return format!("not started: {}", started.trim());
    }
    let key = format!("apps: install {app}: ");
    t.until(5.0, |t| t.read("sim.err").contains(&key));
    let log = t.read("sim.err");
    let line = log.lines().rev().find(|l| l.contains(&key)).unwrap_or("");
    line.split(&key).nth(1).unwrap_or("no outcome").trim().to_string()
}

fn flash(t: &T, path: &str) -> String {
    String::from_utf8_lossy(&market::flash(t, path)).into_owned()
}

pub fn run(t: &mut T) {
    t.say("== app updates from a fake marketplace");
    let market = market::start(t);
    let log = market.log.clone();
    let hello_json = app_json("hello", "1.0.1");
    market.add(
        "hello",
        "Hello",
        "1.0.1",
        1,
        &[("app.json", hello_json.as_bytes()), ("logo.jpg", LOGO), ("main.lua", b"-- new hello")],
        &[],
    );
    let paint_json = app_json("paint", "1.0.0");
    market.add("paint", "Paint", "1.0.0", 1, &[("app.json", paint_json.as_bytes()), ("main.lua", b"-- paint")], &[]);
    market.add("big", "Big", "1.0.0", 2, &[("main.lua", b"-- needs api 2")], &[]);

    market::boot(t, &market);
    t.write("hello/logo.jpg", LOGO);
    install(t, "hello", "1.0.0", "-- old hello");
    install(t, "paint", "1.0.0", "-- paint");
    install(t, "mine", "1.0.0", "-- not in the marketplace");
    t.e(&["wifi", "set", "Home Net", "pw"]);
    let up = t.wifi_up();
    t.check(up, "Wi-Fi joins", |t| format!("info: {}", t.e(&["info"])));

    t.advance(30_000);
    t.check(!log.has("INDEX"), "no check in the first minute", |_| log.tail(3));
    t.advance(35_000);
    let ok = t.until(5.0, |t| t.info("app_updates") == "1");
    t.check(ok && log.has("INDEX"), "a minute after Wi-Fi: one update (hello)", |t| {
        format!("app_updates={} / {}", t.info("app_updates"), said(t))
    });
    let done = t.read("sim.err").contains("apps: check done (2 apps, 1 updates)");
    t.check(done, "counts the apps this firmware runs (not big, Lua API 2)", |t| said(t));

    market.set_raw(Some(r#"{"format": 2, "apps": {}}"#.into()));
    let ok = next_check(t) && t.until(2.0, |t| t.info("app_updates") == "0");
    t.check(ok, "an index for a later firmware offers nothing", |t| said(t));

    market.set_raw(Some(format!(r#"{{"format": 1, "apps": {{}}, "pad": "{}"}}"#, "x".repeat(70_000))));
    let ok = next_check(t) && t.read("sim.err").contains("apps: check failed: index too big");
    t.check(ok && t.info("app_updates") == "0", "an index over 64 KB fails the check", |t| said(t));
    market.set_raw(None);

    // Installing: only the files that changed are fetched; the app's data stays.
    t.e(&["secrets", "set", "hello", "k", "v"]);
    let ok = next_check(t) && t.info("app_updates") == "1";
    t.check(ok, "the index is back", |t| said(t));
    let out = app_install(t, "hello");
    let ok = out == "ok" && flash(t, "hello/main.lua") == "-- new hello" && flash(t, "hello/app.json") == hello_json;
    t.check(ok, "hello is updated to 1.0.1", |t| format!("{out} / main.lua {:?}", flash(t, "hello/main.lua")));
    let fetched = log.has("FILE hello/main.lua") && log.has("FILE hello/app.json") && !log.has("FILE hello/logo.jpg");
    let kept = market::flash(t, "hello/logo.jpg") == LOGO;
    t.check(fetched && kept, "the unchanged logo is kept, not fetched", |_| format!("{} / kept {kept}", log.tail(4)));
    let secrets = t.e(&["secrets"]);
    let has = secrets.lines().any(|l| l.starts_with("hello ") && l.contains(" k "));
    t.check(has, "its secret is still there", |_| secrets.clone());
    let ok = t.until(2.0, |t| t.info("app_updates") == "0");
    t.check(ok, "no update left", |t| t.info("app_updates"));

    // A file that doesn't match the index: the old version stays.
    let paint2 = app_json("paint", "1.0.1");
    market.add("paint", "Paint", "1.0.1", 1, &[("app.json", paint2.as_bytes()), ("main.lua", b"-- paint 2")], &[]);
    market.corrupt("paint/main.lua");
    let newapp = app_json("newapp", "1.0.0");
    market.add("newapp", "New app", "1.0.0", 1, &[("app.json", newapp.as_bytes()), ("main.lua", b"-- new app")], &[]);
    next_check(t);
    let out = app_install(t, "paint");
    let ok = out.starts_with("failed")
        && flash(t, "paint/main.lua") == "-- paint"
        && flash(t, "paint/app.json") == paint_json;
    t.check(ok, "a corrupted download leaves the old version", |t| {
        format!("{out} / main.lua {:?}", flash(t, "paint/main.lua"))
    });
    let apps0: i32 = t.info("apps").parse().unwrap_or(-1);
    let out = app_install(t, "newapp");
    let apps1: i32 = t.info("apps").parse().unwrap_or(-1);
    let ok = out == "ok" && flash(t, "newapp/main.lua") == "-- new app" && apps1 == apps0 + 1;
    t.check(ok, "an app that wasn't installed is installed", |_| format!("{out}, apps {apps0} -> {apps1}"));

    // An app with uploads waiting in RAM (being worked on) isn't offered an
    // update: installing one would drop those uploads.
    let newapp2 = app_json("newapp", "1.0.1");
    let files: &[(&str, &[u8])] = &[("app.json", newapp2.as_bytes()), ("main.lua", b"-- new app 2")];
    market.add("newapp", "New app", "1.0.1", 1, files, &[]);
    next_check(t);
    let before = t.info("app_updates");
    t.write("work.lua", "-- being worked on");
    t.e(&["put", &t.path("work.lua"), "--app", "newapp"]);
    let ok = t.until(2.0, |t| t.info("app_updates").parse::<i32>().ok() == before.parse::<i32>().ok().map(|n| n - 1));
    t.check(ok, "an app with uploads in RAM isn't offered its update", |t| {
        format!("app_updates {before} -> {}", t.info("app_updates"))
    });

    // A restart (a crash here; a new build under --watch does the same)
    // keeps the marketplace address.
    t.advance(1000);
    t.segv();
    let ok = t.until(5.0, |t| t.e(&["info"]).contains("reset: panic"));
    let log = t.read("sim.err");
    let kept = log.matches("apps: marketplace at").count() == 2 && !log.contains("no marketplace address");
    t.check(ok && kept, "a restart keeps the marketplace address", |_| {
        log.lines().filter(|l| l.contains(" apps: ")).take(3).collect::<Vec<_>>().join(" / ")
    });
    t.stop();
}
