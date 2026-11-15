//! The simulator's own network (--set wifi_net=, which the run target
//! passes): a fresh simulator joins it without `luaboard wifi set`, a network
//! set up for real wins, and Wi-Fi turned off stays off until a restart.

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
    t.e(&["wifi", "off"]);
    t.advance(500);
    t.check(wifi(t).0 == "off", "Wi-Fi turned off stays off", |t| format!("{:?}", wifi(t)));
    t.stop(); // the next start: nothing is set up any more, so its own network again
    t.boot_with(&["--set", "wifi_net=Simulator"]);
    let up = t.wifi_up();
    t.check(up && wifi(t).1 == "Simulator", "at the next start it joins its network again", |t| {
        format!("{:?}", wifi(t))
    });
    t.stop();

    t.wipe(); // without wifi_net (the tests' simulators): no Wi-Fi until it's set up
    t.boot();
    t.advance(1000);
    t.check(wifi(t).0 == "off", "without it, nothing is joined", |t| format!("{:?}", wifi(t)));
    t.stop();
}
