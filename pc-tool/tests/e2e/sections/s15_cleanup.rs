//! Cleanup between scripts: pins released, a clean screen for the next one.

use std::path::Path;

use crate::harness::{Ppm, T};

pub fn run(t: &mut T) {
    t.say("== cleanup between scripts");
    t.wipe();
    t.boot();
    t.write("first.lua", "gpio.mode(gpio.PORTB_OUT, gpio.OUT)\ngpio.write(gpio.PORTB_OUT, 1)\ndisp.clear(disp.RED)\n");
    t.write("second.lua", "print(\"second\")\n");
    t.e(&["put", &t.path("first.lua"), &t.path("second.lua"), "--run", "first.lua"]);
    let ok = t.eval("gpio.read(gpio.PORTB_OUT)") == "1";
    t.check(ok, "script drives a pin", |_| "pin not driven".into());
    t.e(&["run", "second.lua"]);
    let ok = t.eval("gpio.read(gpio.PORTB_OUT)") == "0";
    t.check(ok, "pin released when another script starts", |_| "pin still driven after switch".into());
    t.e(&["run", "first.lua"]);
    t.e(&["stop"]);
    let ok = t.eval("gpio.read(gpio.PORTB_OUT)") == "0";
    t.check(ok, "pin released on STOP", |_| "pin still driven after STOP".into());
    t.write(
        "failing.lua",
        "gpio.mode(gpio.PORTB_OUT, gpio.OUT)\ngpio.write(gpio.PORTB_OUT, 1)\nfunction setup() error(\"boom\") end\n",
    );
    t.e(&["put", &t.path("failing.lua")]);
    t.e(&["run", "failing.lua"]);
    let ok = t.eval("gpio.read(gpio.PORTB_OUT)") == "0";
    t.check(ok, "pin released when setup() fails", |_| "pin still driven after a failed start".into());
    let ok = t.eval("adc.read(3)").contains("reserved");
    t.check(ok, "adc can't take the LCD chip select", |t| format!("adc.read(3): {}", t.eval("adc.read(3)")));
    let v = t.eval(
        "local r = {} for _, p in ipairs({ 21, 43, 44, 22 }) do r[#r + 1] = select(2, pcall(gpio.mode, p, gpio.OUT)) end \
         return table.concat(r, '|')",
    );
    let ok = v.matches("reserved").count() == 3 && v.contains("invalid GPIO number");
    t.check(ok, "gpio can't take the IO expander's interrupt or the log's pins, nor a pin the chip lacks", |_| {
        format!("gpio.mode on 21, 43, 44, 22: {v}")
    });
    let ok = t.eval("adc.PORTB_IN == gpio.PORTB_IN and math.abs(adc.read(9) - adc.read(adc.PORTB_IN)) < 50") == "true";
    t.check(ok, "adc takes GPIO numbers", |t| format!("adc pins: {}", t.eval("adc.PORTB_IN")));
    t.e(&["run", "second.lua"]);
    t.stop();
    let red = Ppm::load(Path::new(&t.path("screen.ppm"))).red_pixels();
    t.check(red < 100, "new script starts on a clean screen", |_| format!("old picture left ({red} red px)"));
}
