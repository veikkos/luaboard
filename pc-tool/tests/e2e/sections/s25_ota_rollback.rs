//! Firmware updates that crash: the bootloader or the firmware goes back.

use super::ota::{failing, has_app, info, tail, REBOOTS};
use crate::harness::{grep, EXE, RESTART_NOTICE_S, T};

pub fn run(t: &mut T) {
    t.say("== firmware update (OTA): an update that crashes early in its boot, the bootloader goes back");
    t.wipe();
    t.boot();
    // Images made to fail: one crashes where it confirms itself to the
    // bootloader, one 1 s into every boot.
    let init_sim = t.work.join(format!("init-sim{EXE}"));
    let boot_sim = t.work.join(format!("boot-sim{EXE}"));
    failing(t, "init", &init_sim);
    failing(t, "boot", &boot_sim);
    t.write("main.lua", "function loop() end\n");
    t.e(&["put", &t.path("main.lua"), "--app", "keep", "--save"]);

    let out = t.e_ahead(&["update", &init_sim.to_string_lossy()], REBOOTS);
    t.check(out.contains("crashed at boot and the device went back"), "update says the new firmware crashed", |_| {
        format!("init update: {out}")
    });
    let ok = info(t, "slot") == "ota_0" && info(t, "ota") == "rolledback";
    t.check(ok, "back on ota_0, rolled back", |t| format!("after init crash: {}", t.e(&["info"])));
    let ok = t.read("sim.err").contains("never confirmed itself, rolling back");
    t.check(ok, "the bootloader says so", |_| "no bootloader message".into());

    t.say("   an update that keeps crashing later: it goes back itself");
    // (a crash reset shows the Restarted screen: the update comes during it)
    t.crash();
    t.keep_restart_notice = true;
    t.boot();
    t.keep_restart_notice = false;
    let out = t.e_ahead(&["update", &boot_sim.to_string_lossy()], REBOOTS);
    t.check(grep("done: running .* on ota_1", &out), "it boots first", |_| format!("boot-crash update: {out}"));
    let err = t.read("sim.err");
    let at = err
        .lines()
        .rev()
        .find_map(|l| l.strip_prefix("I (")?.strip_suffix(") ota: rebooting into the new firmware"))
        .and_then(|ms| ms.parse::<i64>().ok());
    t.check(
        at.is_some_and(|ms| ms < RESTART_NOTICE_S * 1000),
        "an update during the Restarted screen doesn't wait for it",
        |_| format!("rebooted at {at:?} ms of the boot"),
    );
    // It crashes a second into each boot; a crash ends the advance it happens in.
    for _ in 0..8 {
        t.advance(1500);
    }
    let ok = t.until_sim(20.0, |t| info(t, "ota") == "rolledback") && info(t, "slot") == "ota_0";
    let crashes = t.read("sim.err").lines().filter(|l| l.contains("fail=boot")).count();
    t.check(ok, &format!("after {crashes} crashes back on ota_0"), |t| format!("boot loop: {}", t.e(&["info"])));
    let err = t.read("sim.err");
    t.check(err.contains("crashed 5 boots in a row"), "the firmware says why", |_| {
        let ota: Vec<&str> = err.lines().filter(|l| l.contains("ota:")).collect();
        format!("no boot-loop message: {}", tail(&ota.join("\n"), 3))
    });
    let ok = has_app(t, "keep");
    t.check(ok, "apps survive the rollbacks", |t| format!("apps after rollback: {}", t.e(&["apps"])));
    t.stop();
}
