//! Board models (CoreS3, its SE and Lite variants) and the power key.

use crate::harness::{grep, T};

const MENU_PX: &str = "local c = disp.get(100, 28) return (c >> 16) < 120 and (c & 255) > 200";

/// Lit pixels in the menu's bottom row, where the hints for the buttons A, B and C are.
fn hint_px(t: &T, name: &str) -> usize {
    t.shot(name).count(0..320, 220..240, |r, g, b| r.max(g).max(b) > 120)
}

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
    let ok = t.until(2.0, |t| hint_px(t, "se.ppm") > 0);
    t.check(ok, "the SE's menu has the hints for its buttons", |_| "no button hints on the SE".into());
    let hello = t.root.join("marketplace/hello/main.lua").to_string_lossy().into_owned();
    t.e(&["put", &hello, "--run", "main.lua"]);
    t.until(2.0, |t| t.e(&["info"]).contains("running: hello/main.lua"));
    t.ctl(&["power=short"]);
    let ok = t.until(2.0, |t| t.e(&["info"]).contains("running: -") && t.eval(MENU_PX) == "true");
    t.check(ok, "power key leaves a running app", |t| format!("power key: {}", line_with(&t.e(&["info"]), "running")));
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
    t.boot_with(&["--model", "cores3-lite", "--set", "panel=e"]);
    let info = t.e(&["info"]);
    t.check(grep("model: cores3$", &info), "Lite reads as CoreS3", |_| {
        format!("lite model: {}", line_with(&info, "model"))
    });
    // The menu's blue as the panel shows it: washed out unless it got the E's own set-up.
    let ok = t.until(2.0, |t| t.shot("panel-e.ppm").count(100..101, 28..29, |r, _, b| r < 120 && b > 200) == 1);
    t.check(ok, "a board with the ILI9342E panel gets that panel's set-up", |t| {
        format!("ILI9342E picture: {}", line_with(&t.read("sim.err"), "ILI9342"))
    });
    let px = hint_px(t, "lite.ppm");
    t.check(px == 0, "a board without the buttons gets no hints for them", |_| {
        format!("button hints on a Lite: {px} px")
    });
    // Its touch panel ends where the LCD does: a finger under it isn't felt, and there's no button to hold.
    t.ctl(&["touch=161,270", "advance=100"]);
    let (held, at) = (t.eval("touch.button()"), t.eval("touch.read()"));
    let refused = !t.ask("button=B");
    t.ctl(&["touch=-", "advance=100"]);
    t.check(held == "nil" && at == "nil" && refused, "a Lite has no touch strip under the screen", |_| {
        format!("strip touch on a Lite: button {held}, read {at}, button=B refused {refused}")
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
