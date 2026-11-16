//! Installing a pulled update: Install on the confirm screen downloads the
//! release's image from the fake release server straight into the other app
//! slot and boots it. The image is a copy of the simulator, as in s25_ota_usb.

use std::fs;

use super::ota::{cp, has_app, info, sha256, Raw};
use super::update::{bin_url, boot, releases, wifi_up};
use crate::fakes::releases::{manifest, UPDATE_BIN};
use crate::harness::{ui, EXE, T};

/// Home: the update row (above Settings), then Install on its confirm screen.
const ROW: (i32, i32) = (60, ui::fixed(1));
const INSTALL: (i32, i32) = (60, ui::row(0));
/// The version the device runs before the update. No build is ever this
/// one: the image installed is a copy of the simulator, and on a release's
/// tag build that copy calls itself the tag (such as v1.0.0).
const FROM: &str = "v0.0.1";
/// The home menu is on the screen (as in s17).
const MENU_PX: &str = "local c = disp.get(100, 28) return (c >> 16) < 120 and (c & 255) > 200";

/// A wiped simulator on FROM that has found the release at `url`.
fn offered(t: &mut T, url: &str) -> bool {
    t.stop();
    t.wipe();
    boot(t, FROM, Some(url));
    t.e(&["wifi", "set", "Home Net", "pw"]);
    let up = wifi_up(t);
    t.advance(65_000);
    up && t.until(5.0, |t| info(t, "update") == "v1.1.0")
}

/// A `luaboard raw` session that gets the device's events.
fn events(t: &T) -> Raw {
    let mut raw = Raw::start(t);
    raw.send(b"\nPING\n");
    t.until_real(2.0, |_| raw.pongs() >= 1);
    raw
}

/// The first event of `raw` that starts with `what`, "" if none.
fn evt(raw: &Raw, what: &str) -> String {
    let want = format!("EVT {what}");
    let text = raw.text();
    let line = text.lines().map(|l| l.trim_end_matches('\r')).find(|l| l.starts_with(&want));
    line.map(|l| l[4..].to_string()).unwrap_or_default()
}

/// The update row, then Install.
fn install(t: &mut T) {
    t.tap_at(ROW.0, ROW.1);
    t.tap_at(INSTALL.0, INSTALL.1);
}

/// The download (real time) is over, then the reboot (the device's time):
/// running the other slot.
fn booted_into(t: &mut T, slot: &str) -> bool {
    let over = |t: &T| {
        let log = t.read("sim.err");
        log.contains("update: installed") || log.contains("update: install failed")
    };
    t.until(30.0, over) && t.until_sim(5.0, |t| info(t, "slot") == slot)
}

pub fn run(t: &mut T) {
    t.say("== installing a pulled update");
    let (rel, url) = releases(t);
    let log = rel.log.clone();
    let img = fs::read(&t.sim_bin).unwrap();
    rel.set_file(UPDATE_BIN, img.clone());
    let good = manifest("v1.1.0", &bin_url(&rel, "v1.1.0"), &img, 2, 1);
    rel.set_manifest(&good);

    let found = offered(t, &url);
    t.write("main.lua", "function loop() end\n");
    t.e(&["put", &t.path("main.lua"), "--app", "keep", "--save"]);
    install(t);
    let booted = booted_into(t, "ota_1");
    let ok =
        found && booted && info(t, "ota") == "ok" && log.has(&format!("OBJECT {UPDATE_BIN}")) && has_app(t, "keep");
    t.check(ok, "Install downloads and boots the new firmware", |t| {
        format!("found {found}, booted {booted}, info: {} / log: {}", t.e(&["info"]), log.tail(4))
    });

    // The new image runs its own build's version, not the one --set gave
    // the old. (Whether v1.1.0 is offered again depends on that version: a
    // tagged build, as in a release, is older than it, a commit's isn't one.)
    t.advance(65_000);
    let (upd, ver) = (info(t, "update"), info(t, "ver"));
    t.check(!ver.is_empty() && ver != FROM, "after the update it runs the new image's version", |_| {
        format!("update: {upd}, ver: {ver}")
    });

    rel.set_manifest(&good.replace(&sha256(&img), &"0".repeat(64)));
    let found = offered(t, &url);
    let raw = events(t);
    install(t);
    t.until(30.0, |_| !evt(&raw, "ota failed").is_empty());
    let failed = evt(&raw, "ota failed");
    raw.finish();
    let ok = found && failed == "ota failed damaged image" && info(t, "slot") == "ota_0" && t.wait_lua(2.0, MENU_PX);
    t.check(ok, "a damaged download is refused", |t| {
        format!("found {found}, event: {failed}, slot: {}, menu: {}", info(t, "slot"), t.eval(MENU_PX))
    });

    rel.set_manifest(&good);
    rel.cut(UPDATE_BIN, 100_000);
    let found = offered(t, &url);
    let raw = events(t);
    install(t);
    t.until(30.0, |_| !evt(&raw, "ota failed").is_empty());
    let (failed, full) = (evt(&raw, "ota failed"), evt(&raw, "update install-failed"));
    raw.finish();
    let ok = found && failed == "ota failed download cut short" && info(t, "slot") == "ota_0" && t.eval("1+1") == "2";
    t.check(ok, "a download cut short fails", |t| format!("found {found}, event: {failed}, slot: {}", info(t, "slot")));
    let ok = full.starts_with("update install-failed got=") && full.ends_with(" connection closed early");
    t.check(ok, "... and the whole reason comes as an event", |_| format!("event: {full}"));

    // A server that keeps sending, a byte at a time, is never idle: the
    // download is given up after ten minutes. The device's clock goes in
    // steps shorter than the idle time, each once more bytes have come.
    // (The release is still offered: the row is where it was.)
    rel.drip(UPDATE_BIN, 100_000);
    let raw = events(t);
    install(t);
    let started = t.until(10.0, |_| !evt(&raw, "ota started").is_empty());
    let mut steps = 0;
    while started && evt(&raw, "ota failed").is_empty() && steps < 90 {
        let (sent, t0) = (rel.dripped(), std::time::Instant::now());
        while rel.dripped() < sent + 2 && t0.elapsed().as_secs() < 2 {
            std::thread::sleep(std::time::Duration::from_millis(1));
        }
        t.advance(10_000);
        steps += 1;
    }
    let failed = evt(&raw, "ota failed");
    raw.finish();
    let ok = started && failed == "ota failed took too long" && steps >= 55 && info(t, "slot") == "ota_0";
    let ok = ok && t.wait_lua(2.0, MENU_PX);
    t.check(ok, "a download that never ends is given up", |t| {
        format!("started {started}, event: {failed}, after {steps} steps of 10 s, slot: {}", info(t, "slot"))
    });

    rel.pause(UPDATE_BIN, 100_000);
    let found = offered(t, &url);
    let raw = events(t);
    install(t);
    let started = t.until(10.0, |_| !evt(&raw, "ota started").is_empty());
    raw.finish();
    let push = t.work.join(format!("push-sim{EXE}"));
    cp(&t.sim_bin.clone(), &push);
    let out = t.e(&["update", &push.to_string_lossy()]);
    rel.resume();
    let booted = booted_into(t, "ota_1");
    let ok = found && started && out.contains("an update is already running") && booted;
    t.check(ok, "a push update is refused while a pull installs", |t| {
        format!("found {found}, started {started}, booted {booted}, push: {out} / info: {}", t.e(&["info"]))
    });
    t.stop();
}
