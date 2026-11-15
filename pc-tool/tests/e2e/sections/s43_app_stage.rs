//! An app update is staged in /apps/.stage/<app> and swapped in with folder
//! renames. A power cut at any step leaves, after the next boot, the old
//! version or the new one: never half of each, never no app.

use crate::harness::T;

const FLASH: &str = "nvs.bin.flash";

fn plant(t: &T, path: &str, text: &str) {
    t.write(&format!("{FLASH}/{path}"), text);
}

fn gone(t: &T, path: &str) -> bool {
    !std::path::Path::new(&t.path(&format!("{FLASH}/{path}"))).exists()
}

pub fn run(t: &mut T) {
    t.say("== staged app updates: a power cut at each step");
    t.wipe();
    // a: cut while the new version was still being written (no .done)
    plant(t, "a/main.lua", "old a");
    plant(t, ".stage/a/main.lua", "half");
    // b: cut after the old version moved aside, before the new moved in
    plant(t, ".old/b/main.lua", "old b");
    plant(t, ".stage/b/main.lua", "new b");
    plant(t, ".stage/b/.done", "");
    // c: cut after the new version moved in, before the cleanup
    plant(t, "c/main.lua", "new c");
    plant(t, "c/.done", "");
    plant(t, ".old/c/main.lua", "old c");
    // d: cut after the old version moved aside, the stage never completed
    plant(t, ".old/d/main.lua", "old d");
    t.boot();
    // boot returns once the control channel answers, which can be before
    // the firmware has mounted the apps (and recovered them) on a busy machine.
    let recovered = |t: &T| {
        let log = t.read("sim.err");
        ["a", "b", "c", "d"].iter().all(|a| log.contains(&format!("vfs: recovered {a}:")))
    };
    let logged = t.until(2.0, recovered);

    let main = |t: &T, app: &str| t.read(&format!("{FLASH}/{app}/main.lua"));
    t.check(main(t, "a") == "old a" && gone(t, ".stage/a"), "a half-written stage is dropped", |t| {
        format!("a/main.lua {:?}, stage gone {}", main(t, "a"), gone(t, ".stage/a"))
    });
    t.check(
        main(t, "b") == "new b" && gone(t, ".stage/b") && gone(t, ".old/b") && gone(t, "b/.done"),
        "a complete stage is moved in",
        |t| format!("b/main.lua {:?}", main(t, "b")),
    );
    t.check(main(t, "c") == "new c" && gone(t, "c/.done") && gone(t, ".old/c"), "a finished swap is cleaned up", |t| {
        format!("c/main.lua {:?}, .done gone {}, old gone {}", main(t, "c"), gone(t, "c/.done"), gone(t, ".old/c"))
    });
    t.check(main(t, "d") == "old d" && gone(t, ".old/d"), "the old version comes back when nothing replaced it", |t| {
        format!("d/main.lua {:?}", main(t, "d"))
    });
    let apps = t.info("apps");
    t.check(apps == "4", "all four apps are there", |_| format!("apps={apps}"));
    t.check(logged, "each recovery is logged", |t| {
        t.read("sim.err").lines().filter(|l| l.contains("recovered")).collect::<Vec<_>>().join(" | ")
    });
    t.stop();
}
