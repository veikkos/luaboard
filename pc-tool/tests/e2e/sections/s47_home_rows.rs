//! The home's fixed rows together: App updates, Update available (firmware)
//! and Settings. The selection stays on the row it was on when another
//! appears, and the buttons reach every row, "+ Get apps" too.

use super::market::{self, app_json, DAY};
use crate::fakes::marketplace::FakeMarket;
use crate::fakes::releases::{manifest, FakeReleases};
use crate::fakes::tls;
use crate::harness::T;

fn logged(t: &T, what: &str) -> usize {
    t.read("sim.err").matches(what).count()
}

pub fn run(t: &mut T) {
    t.say("== the home's fixed rows");
    let cert = tls::self_signed();
    let market = FakeMarket::start(&cert);
    let rel = FakeReleases::start(&cert);
    let ca = t.write("ca.pem", &cert.pem);
    t.sim_env = vec![("SIM_CA_FILE".into(), ca)];
    let bin = format!("https://127.0.0.1:{}/download/v1.1.0/luaboard.bin", rel.port);
    rel.set_manifest(&manifest("v1.1.0", &bin, b"firmware", 1, 1));
    let hello = app_json("hello", "1.0.0");
    market.add("hello", "Hello", "1.0.0", 1, &[("app.json", hello.as_bytes()), ("main.lua", b"-- hello")], &[]);

    t.wipe();
    let (apps, upd) = (market.url(), format!("https://127.0.0.1:{}/latest/download/manifest.json", rel.port));
    t.boot_with(&[
        "--set",
        "fw_version=v1.0.0",
        "--set",
        &format!("update_every={DAY}"),
        "--set",
        &format!("apps_url={apps}"),
        "--set",
        &format!("update_url={upd}"),
    ]);
    market::install(t, "hello", &hello, "-- hello");
    let ok = market::first_check(t) && t.until(3.0, |t| t.info("update") == "v1.1.0");
    t.check(ok, "the firmware row is there, no app updates", |t| {
        format!("update={} app_updates={}", t.info("update"), t.info("app_updates"))
    });

    // Rows: hello, + Get apps; Update available, Settings. Up twice: Update available.
    t.press("A");
    t.press("A");
    let hello2 = app_json("hello", "1.0.1");
    market.add("hello", "Hello", "1.0.1", 1, &[("app.json", hello2.as_bytes()), ("main.lua", b"-- hello 2")], &[]);
    let n = market::checks(t);
    t.advance(DAY);
    let ok = t.until(5.0, |t| market::checks(t) > n && t.info("app_updates") == "1");
    t.check(ok, "App updates appears above it", |t| t.info("app_updates"));
    t.advance(300);
    let before = logged(t, "menu: firmware update offered");
    t.press("B");
    let ok = t.until(2.0, |t| logged(t, "menu: firmware update offered") > before);
    t.check(ok && logged(t, "menu: app updates offered") == 0, "the selection stayed on Update available", |t| {
        market::said(t)
    });

    // Home again; from Update available, up twice: App updates, then + Get apps.
    t.ctl(&["button=B", "advance=900", "button=-", "advance=300"]);
    t.press("A");
    t.press("A");
    t.press("B");
    let ok = t.until(2.0, |t| logged(t, "menu: store opened") > 0);
    t.check(ok, "the buttons reach + Get apps past all three fixed rows", |t| market::said(t));

    // Settings > Check now: both checks run at once, no waiting for the day.
    let newer = app_json("newer", "1.0.0");
    market.add("newer", "Newer", "1.0.0", 1, &[("app.json", newer.as_bytes()), ("main.lua", b"-- newer")], &[]);
    t.ctl(&["button=B", "advance=900", "button=-", "advance=300"]); // home
    t.tap_at(60, 200); // Settings
                       // Check now is the eighth row: drag the list up two rows, it's the last one shown.
    t.ctl(&["touch=60,150", "advance=50", "touch=60,90", "advance=50", "touch=-", "advance=250"]);
    t.tap_at(60, 195);
    let ok = t.until(5.0, |t| t.read("sim.err").contains("apps: check done (2 apps"));
    t.check(ok, "Check now fetches the marketplace at once", |t| market::said(t));
    let ok = t.until(2.0, |t| t.read("sim.err").contains("menu: check now: firmware v1.1.0, 1 app update"));
    t.check(ok, "and says what it found", |t| market::said(t));
    t.stop();
}
