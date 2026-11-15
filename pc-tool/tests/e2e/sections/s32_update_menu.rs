//! The update in the home menu: a row "Update available" above Settings
//! once a check found a newer release, its confirm screen (with the apps the
//! new firmware won't run), and Settings > Check for updates.

use super::ota::{info, Raw};
use super::update::{bin_url, boot, next_check, releases, wifi_up};
use crate::fakes::releases::manifest;
use crate::harness::T;

/// Home: where the update row is when there is one (with two apps, nothing
/// else is there).
const ROW: (i32, i32) = (60, 170);

/// A `luaboard raw` session that gets the device's events (it has said PONG).
fn events(t: &T) -> Raw {
    let mut raw = Raw::start(t);
    raw.send(b"\nPING\n");
    t.until_real(2.0, |_| raw.pongs() >= 1);
    raw
}

/// The events of `raw` that start with `what`.
fn evts(raw: &Raw, what: &str) -> Vec<String> {
    evts_in(&raw.text(), what)
}

/// The events in what a raw session read that start with `what`.
fn evts_in(text: &str, what: &str) -> Vec<String> {
    let want = format!("EVT {what}");
    text.lines()
        .map(|l| l.trim_end_matches('\r'))
        .filter(|l| l.starts_with(&want))
        .map(|l| l[4..].to_string())
        .collect()
}

/// Tap the update row's place at home: the offer event, "" if none came.
fn tap_row(t: &mut T) -> String {
    let raw = events(t);
    t.tap_at(ROW.0, ROW.1); // (returns once the device has run through the tap)
    evts_in(&raw.finish(), "update offer").join(" / ")
}

/// Settings > Check for updates, then home again (hold B).
fn toggle_setting(t: &mut T) {
    // Settings; Check for updates is its seventh row, one below the six that
    // show: drag the list up a row, then it's the last one shown.
    t.tap_at(60, 200);
    t.ctl(&["touch=60,150", "advance=50", "touch=60,120", "advance=50", "touch=-", "advance=250"]);
    t.tap_at(60, 195);
    t.press_for("B", 1000);
}

pub fn run(t: &mut T) {
    t.say("== the update row in the home menu, its confirm screen, the setting");
    let (rel, url) = releases(t);
    let img = b"not really firmware".to_vec();
    rel.set_manifest(&manifest("v1.1.0", &bin_url(&rel, "v1.1.0"), &img, 2, 2));

    t.wipe();
    boot(t, "v1.0.0", Some(&url));
    for (name, title) in [("old", "Old one"), ("old2", "Old two")] {
        t.write(&format!("{name}/main.lua"), "function loop() end\n");
        t.write(&format!("{name}/app.json"), format!("{{ \"title\": \"{title}\" }}"));
        t.e(&["install", &t.path(name)]);
    }
    t.e(&["wifi", "set", "Home Net", "pw"]);
    let up = wifi_up(t);

    let offer = tap_row(t);
    let ok = up && offer.is_empty() && info(t, "running") == "-";
    t.check(ok, "no update row before a check", |t| {
        format!("Wi-Fi {up}, offer: {offer}, running: {}", info(t, "running"))
    });

    t.advance(65_000);
    t.until(5.0, |t| info(t, "update") == "v1.1.0");
    let offer = tap_row(t);
    t.check(offer == "update offer ver=v1.1.0 wont-run=old,old2", "the row appears after the check", |t| {
        format!("offer: {offer}, update: {}", info(t, "update"))
    });

    let raw = events(t);
    t.tap_at(60, 68); // Back, the second of the screen's rows
    let ok = t.until(2.0, |_| !evts(&raw, "menu").is_empty());
    raw.finish();
    t.check(ok, "Back leaves the confirm screen", |_| "no menu event".into());

    rel.set_manifest(&manifest("v1.1.0", &bin_url(&rel, "v1.1.0"), &img, 2, 1));
    let checked = next_check(t);
    let offer = tap_row(t);
    t.check(offer == "update offer ver=v1.1.0 wont-run=-", "apps in range aren't listed", |_| {
        format!("checked {checked}, offer: {offer}")
    });
    let raw = events(t);
    t.press_for("B", 1000);
    let ok = t.until(2.0, |_| !evts(&raw, "menu").is_empty()) && !offer.is_empty();
    raw.finish();
    t.check(ok, "holding B leaves the confirm screen", |_| format!("no menu event (offer: {offer})"));

    toggle_setting(t);
    let ver = info(t, "update");
    let offer = tap_row(t);
    let ok = ver == "-" && offer.is_empty() && info(t, "running") == "-";
    t.check(ok, "turning the setting off hides the row", |t| {
        format!("update: {ver}, offer: {offer}, running: {}", info(t, "running"))
    });

    toggle_setting(t);
    t.advance(30_000);
    let early = info(t, "update");
    t.advance(35_000);
    let ok = t.until(5.0, |t| info(t, "update") == "v1.1.0");
    let offer = tap_row(t);
    let ok = ok && early == "-" && offer.starts_with("update offer ver=v1.1.0");
    t.check(ok, "... on again: a minute later the row is back", |_| format!("after 30 s: {early}, offer: {offer}"));

    // A manifest in a later format than this firmware reads: nothing to
    // install from here, the row opens a screen that says what to do.
    t.press_for("B", 1000);
    let later = manifest("v2.0.0", &bin_url(&rel, "v2.0.0"), &img, 2, 1).replace("\"format\":1", "\"format\":2");
    rel.set_manifest(&later);
    let checked = next_check(t);
    let upd = info(t, "update");
    let offer = tap_row(t);
    let raw = events(t);
    t.tap_at(60, 44); // Back, the screen's only row
    let back = t.until(2.0, |_| !evts(&raw, "menu").is_empty());
    raw.finish();
    let ok = checked && upd == "manual" && offer == "update offer manual" && back && info(t, "slot") == "ota_0";
    t.check(ok, "a manifest in a later format: the row tells to update by hand", |_| {
        format!("checked {checked}, update: {upd}, offer: {offer}, back {back}")
    });
    t.stop();
}
