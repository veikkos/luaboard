//! `gpio.pwm`: a pin driven at a frequency and duty, as the simulated chip's
//! PWM unit sees it (the control question `pwm=PIN:HZ:PERMILLE`, or `PIN:off`).

use crate::harness::T;

pub fn run(t: &mut T) {
    t.say("== gpio.pwm");
    t.wipe();
    t.boot_with(&["--model", "cores3", "--set", "usb=0"]);

    let v = t.eval("tostring(gpio.pwm(gpio.PORTB_OUT))");
    let off = t.ask("pwm=8:off");
    t.check(v == "nil" && off, "no PWM on a pin at start", |_| format!("gpio.pwm(8): {v}, off {off}"));
    // A pin no app uses is in one state, at boot and after an app let go of
    // it: as ESP-IDF resets a pin (not driven, its weak pull-up on). The
    // reserved pins aren't the firmware's to reset: the log's and the IO
    // expander's interrupt are as the chip started them.
    let idle = t.ask("pin=9:idle") && t.ask("pin=17:idle") && t.ask("pin=5:idle");
    let kept = t.ask("pin=43:floating") && t.ask("pin=21:floating");
    t.check(idle && kept, "at boot the free pins are in ESP-IDF's reset state, the reserved ones untouched", |_| {
        format!("free pins reset {idle}, reserved ones left alone {kept}")
    });

    t.eval("gpio.pwm(gpio.PORTB_OUT, 1000, 0.25)");
    let out = t.ask("pwm=8:1000:250") && t.ask("pin=8:out");
    let v = t.eval("table.concat({ gpio.pwm(8) }, ' ')");
    t.check(out && v == "1000 0.25", "1 kHz at 25 % on Port B's output pin", |_| {
        format!("output {out}, gpio.pwm(8): {v}")
    });
    t.eval("gpio.pwm(8, 50, 0.075)");
    let out = t.ask("pwm=8:50:75");
    t.check(out, "changed to 50 Hz at 7.5 % (a servo's middle)", |t| {
        format!("gpio.pwm(8): {}", t.eval("table.concat({ gpio.pwm(8) }, ' ')"))
    });

    for bad in [
        "gpio.pwm(8, 1000, 1.5)",
        "gpio.pwm(8, 1000, -0.1)",
        "gpio.pwm(8, 100000, 0.5)",
        "gpio.pwm(8, 9, 0.5)",
        "gpio.pwm(12, 1000, 0.5)",
        "gpio.pwm(8, 1000)",
    ] {
        let v = t.eval(&format!("tostring((pcall(function() return {bad} end)))"));
        t.check(v == "false", &format!("{bad} is refused"), |_| format!("{bad}: {v}"));
    }
    let still = t.ask("pwm=8:50:75");
    t.check(still, "a refused call changes nothing", |_| "the output changed".into());

    // Four frequencies at a time (the chip's timers); pins at the same one share.
    t.eval("gpio.pwm(9, 100, 0.5) gpio.pwm(1, 200, 0.5) gpio.pwm(2, 300, 0.5)");
    let v = t.eval("select(2, pcall(gpio.pwm, 17, 400, 0.5))");
    let shared = t.eval("tostring((pcall(gpio.pwm, 17, 300, 1)))");
    let out = t.ask("pwm=17:300:1000");
    t.check(
        v.contains("frequencies") && shared == "true" && out,
        "a fifth frequency is refused, a shared one isn't",
        |_| format!("fifth: {v}; shared: {shared}, output {out}"),
    );

    // Asleep on battery: PWM holds off light sleep (the chip would stop its clock).
    t.eval("disp.sleep(true)");
    t.advance(1100);
    let held = !t.is("light");

    t.eval("gpio.pwm(9, 0)");
    let v = t.eval("tostring(gpio.pwm(9))");
    let off = t.ask("pwm=9:off");
    let idle = t.ask("pin=9:idle");
    t.check(v == "nil" && off && idle, "gpio.pwm(pin, 0) stops it, and the pin is as it was at boot", |_| {
        format!("gpio.pwm(9): {v}, off {off}, as at boot {idle}")
    });
    // Full on and full off are reachable at any frequency, and changing only
    // the duty keeps pulsing.
    t.eval("gpio.pwm(9, 10, 1)");
    let full = t.ask("pwm=9:10:1000");
    t.eval("gpio.pwm(9, 10, 0)");
    let none = t.ask("pwm=9:10:0");
    let v = t.eval("adc.read(9) return tostring(gpio.pwm(9))");
    let off = t.ask("pwm=9:off");
    t.check(
        full && none && v == "nil" && off,
        "duty 1 and 0 at the lowest frequency; adc.read on the pin stops it",
        |_| format!("full {full}, none {none}, after adc.read: {v}, off {off}"),
    );
    t.eval("gpio.write(8, 1) gpio.mode(1, gpio.IN) gpio.toggle(2) gpio.pwm(17, 0)");
    let off = t.ask("pwm=8:off") && t.ask("pwm=1:off") && t.ask("pwm=2:off");
    let v = t.eval("gpio.read(8)");
    t.check(off && v == "1", "using a pin as plain gpio stops its PWM", |_| format!("off {off}, gpio.read(8): {v}"));
    t.advance(100);
    let light = t.is("light");
    t.eval("disp.sleep(false)");
    t.check(held && light, "PWM holds off light sleep; stopped, it's allowed", |_| {
        format!("held off {held}, light after {light}")
    });

    // A script that leaves a pin pulsing: it stops when the script ends.
    t.write("pulse/main.lua", "gpio.pwm(gpio.PORTB_OUT, 500, 0.5)\nfunction loop() end\n");
    t.e(&["put", &t.path("pulse/main.lua"), "--app", "pulse", "--run", "main.lua"]);
    t.until(2.0, |t| t.e(&["info"]).contains("running: pulse/main.lua"));
    let on = t.ask("pwm=8:500:500");
    t.e(&["stop"]);
    let off = t.ask("pwm=8:off") && t.ask("pin=8:idle");
    t.check(on && off, "PWM stops when the script ends, the pin as it was at boot", |_| {
        format!("on {on}, off after {off}")
    });
    t.stop();
}
