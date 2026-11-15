//! The heart_rate example on its simulated strap (examples/heart_rate/sim_device.json).

use crate::harness::T;

/// Pixels of a zone colour in the big number's area, on a fresh screenshot.
fn zone_px(t: &T, want: (i32, i32, i32)) -> usize {
    let near = |v: u8, w: i32| (v as i32 - w).abs() < 12;
    t.shot("hr.ppm").count(0..200, 30..95, |r, g, b| near(r, want.0) && near(g, want.1) && near(b, want.2))
}

const Z3: (i32, i32, i32) = (70, 205, 95);
const Z4: (i32, i32, i32) = (255, 165, 40);

pub fn run(t: &mut T) {
    t.say("== example heart_rate: a strap with the Heart Rate service (simulated)");
    t.wipe();
    let dev = t.root.join("examples/heart_rate/sim_device.json").to_string_lossy().into_owned();
    t.boot_with(&["--ble-device", &dev, "--set", "ble=hr:off"]);
    let app = t.root.join("examples/heart_rate").to_string_lossy().into_owned();
    t.e(&["install", &app, "--run"]);
    t.advance(500);
    let v = t.eval("HR.phase");
    t.check(v == "search", "no strap: looking for one", |_| format!("phase: {v}"));

    t.ctl(&["ble=hr:on"]);
    let ok = t.until_sim(5.0, |t| t.eval("HR.phase .. ' ' .. tostring(HR.bpm)") == "live 72");
    t.check(ok, "finds the strap by its service and shows its reading", |t| {
        t.eval("HR.phase .. ' ' .. tostring(HR.bpm)")
    });
    let v = t.eval("store.get('addr') .. ' ' .. HR.battery .. ' ' .. HR.rr");
    t.check(v == "c4:52:0d:18:00:01 75 1000", "remembers it, reads its battery and the RR interval", |_| {
        format!("addr, battery, rr: {v}")
    });

    // flags 16: contact supported and detected, RR intervals; 150 bpm is 79 % of 190
    t.ctl(&["ble_notify=hr:2a37:16 96 00 04", "advance=100"]);
    let n = zone_px(t, Z3);
    t.check(n > 1000, &format!("150 bpm of 190 max: zone 3, green ({n} px)"), |_| format!("zone 3: {n} px"));
    // flags 01: a 16-bit rate, 0x00a0 = 160 (84 %)
    t.ctl(&["ble_notify=hr:2a37:01 a0 00", "advance=100"]);
    let n = zone_px(t, Z4);
    let v = t.eval("HR.bpm");
    t.check(v == "160" && n > 1000, "a 16-bit rate: 160, zone 4, orange", |_| format!("bpm {v}, zone 4: {n} px"));
    // flags 04: contact supported, not detected
    t.ctl(&["ble_notify=hr:2a37:04 a0", "advance=100"]);
    let v = t.eval("tostring(HR.contact)");
    let n = zone_px(t, Z4);
    t.check(v == "false" && n == 0, "off the skin: the number dims", |_| format!("contact {v}, orange {n} px"));
    t.ctl(&["ble_notify=hr:2a37:16 96 00 04", "advance=1100"]);

    t.ctl(&["touch=160,150", "advance=1000", "touch=-", "advance=250"]);
    let v = t.eval("tostring(HR.settings)");
    t.check(v == "true", "holding the screen opens the settings", |_| format!("settings: {v}"));
    t.press("C");
    t.press_for("C", 1000);
    t.press("A");
    let v = t.eval("HR.max_hr .. ' ' .. store.get('max_hr')");
    t.check(v == "195 195", "C +1, held C +5, A -1: max 195, kept", |_| format!("max: {v}"));
    t.press("B");
    let v = t.eval("tostring(HR.settings)");
    t.check(v == "false", "B closes the settings", |_| format!("settings: {v}"));

    t.ctl(&["ble=hr:off"]);
    let ok = t.until_sim(4.0, |t| t.eval("HR.phase") != "live");
    t.check(ok, "losing the strap: looks for it again", |t| format!("phase: {}", t.eval("HR.phase")));
    t.stop();
}
