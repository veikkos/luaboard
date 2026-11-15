//! The `uart` module: a serial port on Port C, here with its TX wired to its
//! RX (`--set portc=loopback`).

use crate::harness::T;

pub fn run(t: &mut T) {
    t.say("== uart: a serial port on Port C");
    t.wipe();
    t.boot_with(&["--model", "cores3", "--set", "usb=0", "--set", "portc=loopback"]);

    let v = t.eval("select(2, pcall(uart.read))");
    t.check(v.contains("uart isn't open"), "reading before uart.open is an error", |_| format!("read: {v}"));
    let bad = [
        "uart.open(10)",
        "uart.open(9600, { tx = 12 })",
        "uart.open(9600, { rx = 22 })",
        "uart.open(9600, { tx = 43 })",
        "uart.open(9600, { parity = 'x' })",
        "uart.open()",
    ];
    for bad in bad {
        let v = t.eval(&format!("tostring((pcall(function() return {bad} end)))"));
        t.check(v == "false", &format!("{bad} is refused"), |_| format!("{bad}: {v}"));
    }

    let v = t.eval("tostring(uart.open(9600)) .. ' ' .. uart.write('hello\\nwor')");
    let held = t.ask("pin=17:out") && !t.ask("pin=18:idle");
    t.check(v == "true 9" && held, "open at 9600, write 9 bytes; the port has its pins", |_| {
        format!("open, write: {v}, pins taken {held}")
    });
    t.advance(50);
    let v = t.eval("tostring(uart.line()) .. '|' .. tostring(uart.line())");
    t.check(v == "hello|nil", "a whole line comes back, half a line doesn't", |_| format!("lines: {v}"));
    t.eval("uart.write('ld\\r\\n')");
    t.advance(50);
    let v = t.eval("tostring(uart.line()) .. '|' .. #uart.read()");
    t.check(v == "world|0", "the rest of the line, without its CR LF; then nothing", |_| format!("line, read: {v}"));
    t.eval("uart.write('abc')");
    t.advance(50);
    let v = t.eval("uart.read(2) .. '|' .. uart.read()");
    t.check(v == "ab|c", "read(max) takes that many bytes", |_| format!("reads: {v}"));

    let v = t.eval("select(2, pcall(gpio.mode, gpio.PORTC_RX, gpio.IN))");
    t.check(v.contains("uart"), "the port's pins aren't gpio's while it's open", |_| format!("gpio.mode: {v}"));
    // ... nor i2c's, when the port is on Port A's pins; and a refused
    // re-open leaves the open port as it was.
    t.eval("uart.open(9600, { rx = 1, tx = 2 })");
    let v = t.eval("select(2, pcall(i2c.scan))");
    let again = t.eval("tostring((pcall(uart.open, 10))) .. ' ' .. #uart.read()");
    t.check(
        v.contains("uart") && again == "false 0",
        "i2c can't take the port's pins; a refused re-open keeps it open",
        |_| format!("i2c.scan: {v}; re-open, read: {again}"),
    );
    t.eval("uart.open(9600)");
    t.eval("i2c.scan()"); // the bus takes Port A
    let v = t.eval("select(2, pcall(uart.open, 9600, { rx = 1, tx = 2 }))");
    t.check(v.contains("in use by i2c"), "nor can the port take pins i2c has", |_| format!("uart.open on Port A: {v}"));

    // Asleep on battery: no light sleep while the port is open (bytes would be lost).
    t.eval("disp.sleep(true)");
    t.advance(1100);
    let held = !t.is("light");
    t.eval("uart.close()");
    t.advance(100);
    let ok = held && t.is("light");
    t.eval("disp.sleep(false)");
    t.check(ok, "an open port holds off light sleep; closed, it's allowed", |_| format!("held off {held}"));
    let free = t.ask("pin=18:idle") && t.ask("pin=17:idle");
    let v = t.eval("tostring((pcall(gpio.mode, gpio.PORTC_RX, gpio.IN)))");
    t.check(v == "true" && free, "closed: the pins are free again, as they were at boot", |_| {
        format!("gpio.mode: {v}, as at boot {free}")
    });

    // A script that leaves the port open: it's closed when the script ends.
    t.write("ser/main.lua", "uart.open(115200)\nfunction loop() end\n");
    t.e(&["put", &t.path("ser/main.lua"), "--app", "ser", "--run", "main.lua"]);
    t.until(2.0, |t| t.e(&["info"]).contains("running: ser/main.lua"));
    let open = t.eval("#uart.read()") == "0";
    t.e(&["stop"]);
    let v = t.eval("select(2, pcall(uart.read))");
    t.check(open && v.contains("uart isn't open"), "the port closes when the script ends", |_| {
        format!("open {open}, after: {v}")
    });
    t.stop();
}
