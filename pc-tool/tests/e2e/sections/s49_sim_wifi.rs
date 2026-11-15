//! The simulator's own network (--set wifi_net=, which the run target
//! passes): a fresh simulator joins it without `luaboard wifi set`, and a
//! network set up for real wins.

use crate::harness::T;

/// The `luaboard wifi` lines: (state, ssid).
fn wifi(t: &T) -> (String, String) {
    let out = t.e(&["wifi"]);
    let field = |k: &str| {
        out.lines()
            .find_map(|l| l.trim().strip_prefix(&format!("{k}:")).map(|v| v.trim().to_string()))
            .unwrap_or_default()
    };
    (field("state"), field("ssid"))
}

pub fn run(t: &mut T) {
    t.say("== the simulator's own network");
    t.wipe();
    t.boot_with(&["--set", "wifi_net=Simulator"]);
    let up = t.wifi_up();
    t.check(up && wifi(t) == ("connected".into(), "Simulator".into()), "a fresh simulator joins its network", |t| {
        format!("{:?}", wifi(t))
    });
    t.e(&["wifi", "set", "Home Net", "pw"]);
    let ok = t.wifi_up() && wifi(t).1 == "Home Net";
    t.check(ok, "a network set up for real wins", |t| format!("{:?}", wifi(t)));
    t.stop();
}
