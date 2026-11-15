//! i2c on Port A against the simulated ENV IV unit.

use crate::harness::T;

const SHT_LUA: &str = r#"local function crc(s)
  local c = 0xFF
  for i = 1, #s do
    c = c ~ s:byte(i)
    for _ = 1, 8 do c = (c & 0x80 ~= 0) and ((c << 1) ~ 0x31) & 0xFF or (c << 1) & 0xFF end
  end
  return c
end
assert(i2c.write(0x44, "\xFD"))
local d = assert(i2c.read(0x44, 6))
assert(crc(d:sub(1, 2)) == d:byte(3) and crc(d:sub(4, 5)) == d:byte(6), "crc")
local t = -45 + 175 * string.unpack(">I2", d, 1) / 65535
local h = -6 + 125 * string.unpack(">I2", d, 4) / 65535
return string.format("%.1f %.1f", t, h)
"#;

pub fn run(t: &mut T) {
    t.say("== i2c on Port A (simulated ENV IV unit: SHT40 at 0x44)");
    t.wipe();
    t.boot_with(&["--set", "porta=env", "--set", "env_temp=23.4", "--set", "env_hum=56.7"]);
    t.write("main.lua", "function loop() end\n");
    t.e(&["put", &t.path("main.lua"), "--app", "env", "--run", "main.lua"]);
    let v = t.eval(r#"table.concat(i2c.scan(), ",")"#);
    t.check(v == "68", "scan finds the unit (0x44), not the internal chips", |_| format!("scan: {v}"));
    t.write("sht.lua", SHT_LUA);
    t.e(&["put", &t.path("sht.lua"), "--app", "env"]);
    let v = t.eval(r#"require("sht")"#);
    t.check(v == "23.4 56.7", "write a command, read the measurement back (CRC checked)", |_| format!("measure: {v}"));
    let v = t.eval(r#"i2c.write(0x50, "x")"#);
    t.check(v.contains("no answer from 0x50"), "a missing device is an error value", |_| format!("missing: {v}"));
    let v = t.eval("i2c.read(0x34, 1)");
    t.check(v.contains("no answer from 0x34"), "the internal bus (power chip at 0x34) is out of reach", |_| {
        format!("internal: {v}")
    });
    let v = t.eval("gpio.mode(gpio.PORTA_SDA, gpio.OUT)");
    let ok = v.contains("in use by i2c") && t.eval("adc.read(adc.PORTA_SCL)").contains("in use by i2c");
    t.check(ok, "gpio and adc can't take the pins while i2c has them", |_| format!("pins: {v}"));
    let v = t.eval("i2c.read(0x03, 1)");
    t.check(v.contains("0x08..0x77"), "reserved addresses are refused", |_| format!("addr: {v}"));
    let v = t.eval("sys.port_power()");
    t.check(v == "true", "the ports have 5V from boot", |_| format!("port power at boot: {v}"));
    let off = t.eval("sys.port_power(false) return #i2c.scan()");
    let ok = off == "0" && t.eval("sys.port_power(true) return #i2c.scan()") == "1";
    t.check(ok, "sys.port_power(false) cuts the unit's power, true brings it back", |_| format!("port power: {off}"));
    t.eval("sys.port_power(false)");
    t.e(&["run", "env"]); // a new run: the bus is free again
    let v = t.eval("sys.port_power()");
    t.check(v == "true", "the power is back on for the next app", |_| format!("after run: {v}"));
    t.eval("gpio.mode(gpio.PORTA_SDA, gpio.IN)");
    let v = t.eval("i2c.scan()");
    t.check(v.contains("in use by gpio"), "and i2c can't take pins gpio has", |_| format!("i2c after gpio: {v}"));
    t.stop();
    t.boot();
    t.e(&["put", &t.path("main.lua"), "--app", "env", "--run", "main.lua"]);
    let v = t.eval("#i2c.scan()");
    t.check(v == "0", "nothing plugged in: empty scan", |_| format!("empty: {v}"));
    t.stop();
}
