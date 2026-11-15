//! Firmware updates over the serial link (OTA), and what the device refuses.

use std::fs;

use super::ota::{cp, has_app, info, raw_ota, sha256, REBOOTS};
use crate::harness::{grep, EXE, T};

pub fn run(t: &mut T) {
    t.say("== firmware update (OTA) over the serial link");
    t.wipe();
    t.boot();
    let sim = t.sim_bin.clone();
    let new_sim = t.work.join(format!("new-sim{EXE}"));
    let new_s = new_sim.to_string_lossy().into_owned();
    cp(&sim, &new_sim);
    t.write("main.lua", "function loop() end\n");
    t.e(&["put", &t.path("main.lua"), "--app", "keep", "--save"]);

    let ok = info(t, "slot") == "ota_0" && info(t, "target") == "sim";
    t.check(ok, "fresh simulator runs ota_0", |t| format!("slot/target: {}", t.e(&["info"])));
    let out = t.e(&["update", &t.path("main.lua")]);
    t.check(out.contains("not a firmware image"), "update refuses a file that isn't an image", |_| {
        format!("non-image: {out}")
    });

    let out = t.e_ahead(&["update", &new_s], REBOOTS);
    let ok = out.contains("installed") && grep("done: running .* on ota_1", &out);
    t.check(ok, "update installs and reboots into ota_1", |_| format!("update: {out}"));
    let v = info(t, "ota");
    t.check(v == "ok", "no trial: it's in use at once", |t| format!("state after update: {}", info(t, "ota")));
    let out = t.e_ahead(&["update", &new_s], REBOOTS);
    t.check(grep("done: running .* on ota_0", &out), "a second update can follow right away", |_| {
        format!("second update: {out}")
    });
    let ok = has_app(t, "keep");
    t.check(ok, "installed apps survive the updates", |t| format!("apps after update: {}", t.e(&["apps"])));

    t.stop();
    t.boot(); // a power cycle: the bootloader keeps booting the update
    let ok = info(t, "slot") == "ota_0" && info(t, "ota") == "ok";
    t.check(ok, "after a power cycle it still runs the update", |t| format!("power cycle: {}", t.e(&["info"])));
    t.e_ahead(&["update", &new_s], REBOOTS);
    t.stop();
    // a build the slots don't know: flashed over USB
    let usb_sim = t.work.join(format!("usb-sim{EXE}"));
    cp(&sim, &usb_sim);
    t.sim_bin = usb_sim;
    t.boot();
    t.sim_bin = sim.clone();
    let ok = info(t, "slot") == "ota_0" && info(t, "ota") == "ok";
    t.check(ok, "flashing over USB right after an update isn't a rollback", |t| {
        format!("USB flash after update: {}", t.e(&["info"]))
    });

    t.say("   the device checks what it gets");
    let img = fs::read(&new_sim).unwrap();
    let size = img.len();
    let r = raw_ota(t, size, &"0".repeat(64), &new_sim);
    t.check(r.contains("ERR sha256 mismatch"), "a damaged image is refused", |_| format!("sha mismatch: {r}"));
    let mut junk = vec![0u8; 4096];
    getrandom::fill(&mut junk).unwrap();
    let junk_path = t.work.join("junk.bin");
    fs::write(&junk_path, &junk).unwrap();
    let sha = sha256(&junk);
    let r = raw_ota(t, 4096, &sha, &junk_path);
    t.check(r.contains("ERR not a firmware image"), "something that isn't an image is refused", |_| {
        format!("junk: {r}")
    });
    let r = raw_ota(t, 99999999, &sha, &junk_path);
    t.check(r.contains("ERR image size"), "an image bigger than the slot is refused", |_| format!("too big: {r}"));
    let ok = info(t, "slot") == "ota_0" && t.eval("1+1") == "2";
    t.check(ok, "after refused updates it runs on, unchanged", |t| format!("after refusals: {}", t.e(&["info"])));
    t.stop();
}
