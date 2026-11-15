//! The CoreS3's own sensors: `imu` (motion) and `light` (ambient light and
//! proximity). The SE has neither.

use crate::harness::T;

/// The numbers a Lua expression returns, e.g. "0.5 -0.25 0.75".
fn nums(t: &T, expr: &str) -> Vec<f64> {
    t.eval(&format!("table.concat({{ {expr} }}, ' ')")).split_whitespace().filter_map(|v| v.parse().ok()).collect()
}

fn near(got: &[f64], want: &[f64], tol: f64) -> bool {
    got.len() == want.len() && got.iter().zip(want).all(|(g, w)| (g - w).abs() <= tol)
}

pub fn run(t: &mut T) {
    t.say("== the board's sensors: imu and light");
    t.wipe();
    t.boot_with(&[
        "--model",
        "cores3",
        "--set",
        "accel=0.5,-0.25,0.75",
        "--set",
        "gyro=10,-20,30",
        "--set",
        "light=500",
        "--set",
        "proximity=300",
    ]);
    let off = t.ask("sensor=imu:off") && t.ask("sensor=light:off");
    t.check(off, "both sensors are off until a script uses them", |_| "a sensor is on at boot".into());

    let a = nums(t, "imu.accel()");
    let on = t.ask("sensor=imu:on");
    t.check(near(&a, &[0.5, -0.25, 0.75], 0.01) && on, "imu.accel() in g", |_| format!("accel {a:?}, on {on}"));
    let g = nums(t, "imu.gyro()");
    t.check(near(&g, &[10.0, -20.0, 30.0], 0.5), "imu.gyro() in degrees per second", |_| format!("gyro {g:?}"));
    t.ctl(&["accel=0,0,1", "advance=50"]);
    let a = nums(t, "imu.accel()");
    t.check(near(&a, &[0.0, 0.0, 1.0], 0.01), "flat on the table: z is 1 g", |_| format!("accel {a:?}"));

    let lux = nums(t, "light.lux()");
    let on = t.ask("sensor=light:on");
    t.check(near(&lux, &[500.0], 5.0) && on, "light.lux()", |_| format!("lux {lux:?}, on {on}"));
    let p = nums(t, "light.proximity()");
    t.check(near(&p, &[300.0], 0.0), "light.proximity()", |_| format!("proximity {p:?}"));
    t.ctl(&["light=20", "proximity=1500", "advance=200"]);
    let (lux, p) = (nums(t, "light.lux()"), nums(t, "light.proximity()"));
    t.check(near(&lux, &[20.0], 1.0) && near(&p, &[1500.0], 0.0), "the readings follow the room", |_| {
        format!("lux {lux:?}, proximity {p:?}")
    });

    // A script that used them: they're switched off again when it ends.
    t.write("tilt/main.lua", "function loop() X = imu.accel() L = light.lux() end\n");
    t.e(&["put", &t.path("tilt/main.lua"), "--app", "tilt", "--run", "main.lua"]);
    t.advance(300); // the sensors' start-up delays, in the first frame
    t.until(2.0, |t| t.eval("tostring(X) .. tostring(L)") != "nilnil");
    let on = t.ask("sensor=imu:on") && t.ask("sensor=light:on");
    t.e(&["stop"]);
    let off = t.ask("sensor=imu:off") && t.ask("sensor=light:off");
    t.check(on && off, "the sensors are switched off when the script ends", |_| format!("on {on}, off after {off}"));
    t.stop();

    t.say("   the SE has no sensors");
    t.wipe();
    t.boot();
    let v = t.eval("tostring(imu.accel()) .. ' / ' .. select(2, imu.accel())");
    t.check(v.starts_with("nil / ") && v.contains("no motion sensor"), "imu on the SE: nil and why", |_| {
        format!("imu.accel(): {v}")
    });
    let v = t.eval("tostring(light.lux()) .. ' / ' .. select(2, light.proximity())");
    t.check(v.starts_with("nil / ") && v.contains("no light sensor"), "light on the SE: nil and why", |_| {
        format!("light: {v}")
    });
    t.stop();
}
