//! The simulator's --watch: a new build of it is flashed and booted.

use std::fs;

use super::ota::{cp, has_app, info, tail};
use crate::harness::{EXE, T};

pub fn run(t: &mut T) {
    t.say("== firmware update (OTA): --watch, a new build of the simulator is flashed and booted");
    t.wipe();
    let sim = t.sim_bin.clone();
    let watched = t.work.join(format!("watched-sim{EXE}"));
    cp(&sim, &watched);
    t.sim_bin = watched.clone();
    t.boot_with(&["--watch"]);
    t.sim_bin = sim.clone();
    t.write("main.lua", "function loop() end\n");
    t.e(&["put", &t.path("main.lua"), "--app", "keep", "--save"]);
    t.eval("marker = 42");
    let sim_pid = t.sim_pid();
    // a new file, as the linker writes one
    let new_build = t.work.join(format!("new-build{EXE}"));
    cp(&sim, &new_build);
    fs::rename(&new_build, &watched).unwrap();
    let ok = t.until(10.0, |t| t.read("sim.err").contains("watch: new build"));
    t.check(ok, "a new build is noticed", |t| format!("no watch message: {}", tail(&t.read("sim.err"), 3)));
    let ok = t.until(10.0, |t| t.eval("marker == nil") == "true");
    t.check(ok, "and booted: the firmware started over", |t| format!("not rebooted: {}", t.eval("marker")));
    let ok = t.sim_running() && t.sim_pid() == sim_pid && info(t, "slot") == "ota_0";
    t.check(ok, "in the same simulator, from ota_0", |t| format!("after watch: {}", t.e(&["info"])));
    let ok = has_app(t, "keep");
    t.check(ok, "installed apps stay", |t| format!("apps after watch: {}", t.e(&["apps"])));
    t.stop();
}
