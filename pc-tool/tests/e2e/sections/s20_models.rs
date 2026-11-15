//! Board models (CoreS3, its SE and Lite variants) and the power key.

use crate::harness::{grep, T};

const MENU_PX: &str = "local c = disp.get(100, 28) return (c >> 16) < 120 and (c & 255) > 200";

fn line_with(text: &str, pat: &str) -> String {
    text.lines().filter(|l| l.contains(pat)).collect::<Vec<_>>().join("\n")
}

pub fn run(t: &mut T) {
    t.say("== board models and the power key");
    t.wipe();
    t.boot();
    let info = t.e(&["info"]);
    t.check(info.contains("model: cores3se"), "SE by default (no IMU)", |_| {
        format!("default model: {}", line_with(&info, "model"))
    });
    let ok = t.eval("sys.info().model") == "cores3se";
    t.check(ok, "sys.info().model", |_| "sys.info().model".into());
    let hello = t.root.join("marketplace/hello/main.lua").to_string_lossy().into_owned();
    t.e(&["put", &hello, "--run", "main.lua"]);
    t.until(2.0, |t| t.e(&["info"]).contains("running: hello/main.lua"));
    t.ctl(&["power=short"]);
    let ok = t.until(2.0, |t| t.e(&["info"]).contains("running: -") && t.eval(MENU_PX) == "true");
    t.check(ok, "power key leaves a running app", |t| format!("power key: {}", line_with(&t.e(&["info"]), "running")));
    t.tap_at(60, 200); // Settings
    t.ctl(&["power=short", "advance=300"]);
    t.press("C"); // on home: C wraps from Settings to the first app, B opens it
    t.press("B");
    let info = t.e(&["info"]);
    t.check(info.contains("running: hello/main.lua"), "power key in Settings goes home", |_| {
        format!("power key in Settings: {}", line_with(&info, "running"))
    });
    t.ctl(&["power=short", "advance=300"]);
    t.ctl(&["power=long", "advance=300"]);
    let info = t.e(&["info"]);
    let ok = info.contains("running: -") && info.contains("screen: off");
    t.check(ok, "a long press turns the screen off", |_| format!("long press: {}", line_with(&info, "screen")));
    t.ctl(&["power=short", "advance=300"]);
    t.stop();

    t.wipe();
    t.boot_with(&["--model", "cores3"]);
    let info = t.e(&["info"]);
    t.check(grep("model: cores3$", &info), "CoreS3 detected by its IMU", |_| {
        format!("cores3 model: {}", line_with(&info, "model"))
    });
    let v = t.eval("sys.battery()");
    t.check(v == "80\ttrue\ttrue", "CoreS3 has a battery", |_| format!("cores3 battery: {v}"));
    t.stop();

    t.wipe();
    t.boot_with(&["--model", "cores3-lite"]);
    let info = t.e(&["info"]);
    t.check(grep("model: cores3$", &info), "Lite reads as CoreS3", |_| {
        format!("lite model: {}", line_with(&info, "model"))
    });
    t.say("   luaboard sim-ctl: the control channel from the command line");
    let out = t.sim_ctl(&["battery=37", "advance=100"]);
    let ok = out.is_empty() && t.until(2.0, |t| t.eval("sys.battery()").starts_with("37\t"));
    t.check(ok, "sim-ctl sets a board input", |t| format!("sim-ctl: {out} battery: {}", t.eval("sys.battery()")));
    let out = t.sim_ctl(&["battery=44", "bogus=1"]);
    t.check(
        out.contains("refused: bogus=1") && !out.contains("battery=44"),
        "sim-ctl says which line was refused",
        |_| format!("sim-ctl refused: {out}"),
    );
    t.ctl(&["battery=50"]); // the harness has the channel again
    let ok = t.until(2.0, |t| t.eval("sys.battery()").starts_with("50\t"));
    t.check(ok, "and the tests have it back afterwards", |t| format!("battery: {}", t.eval("sys.battery()")));
    t.stop();
}
