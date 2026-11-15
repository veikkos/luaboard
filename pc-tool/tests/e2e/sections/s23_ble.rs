//! Bluetooth LE from Lua against simulated devices described in JSON
//! (platform/sim/models/ble_device.h), and the di2 example on one.

use std::fs;
use std::process::{Command, Stdio};

use crate::harness::{Ppm, T};

const ADDR: &str = "c0:ff:ee:00:00:01";
const SVC: &str = "12345678-1234-5678-1234-56789abcdef0";
const NOTE: &str = "12345678-1234-5678-1234-56789abcdef1"; // notifies; sends ff then its value when subscribed
const RW: &str = "12345678-1234-5678-1234-56789abcdef2"; // read and write
const PAIRED: &str = "12345678-1234-5678-1234-56789abcdef3"; // read, needs pairing
const GEAR: &str = "00002ac1-5348-494d-414e-4f5f424c4500";

/// Lua with the names above filled in.
fn lua(s: &str) -> String {
    s.replace("$ADDR", ADDR).replace("$SVC", SVC).replace("$NOTE", NOTE).replace("$RW", RW).replace("$PAIRED", PAIRED)
}

fn gear(front: u8, rear: u8) -> String {
    format!("ble_value=di2:{GEAR}:00 06 03 0{front} 02 {rear:02x} 0c 02 ee 12 fd ee 12 05 00 12 00")
}

/// The gear in use is drawn amber (255,196,40 -> RGB565 ~ 255,195,66).
fn amber(t: &T) -> usize {
    let p = Ppm::load(&t.work.join("screen.ppm"));
    p.px.as_chunks::<3>().0.iter().filter(|c| c[0] > 230 && c[1] > 170 && c[1] < 220 && c[2] < 100).count()
}

fn same(t: &T, a: &str, b: &str) -> bool {
    match (fs::read(t.work.join(a)), fs::read(t.work.join(b))) {
        (Ok(x), Ok(y)) => x == y,
        _ => false,
    }
}

pub fn run(t: &mut T) {
    // A simulated device described in JSON (platform/sim/models/ble_device.h)
    let dev = t.root.join("pc-tool/tests/e2e/data/ble_device.json").to_string_lossy().into_owned();

    t.say("== ble: scan, connect, read, write, subscribe (simulated device)");
    t.wipe();
    t.boot_with(&["--ble-device", &dev]);
    t.write("main.lua", "seen, log = {}, {}\nble.scan(function(a) seen[a.addr] = a end)\nfunction loop() end\n");
    t.e(&["put", &t.path("main.lua"), "--app", "bt", "--run", "main.lua"]);
    let name_expr = lua(r#"seen["$ADDR"] and seen["$ADDR"].name"#);
    let ok = t.until_sim(2.0, |t| t.eval(&name_expr) == "Test Sensor");
    t.check(ok, "scan hears the device, with its name", |t| format!("scan: {}", t.eval("next(seen)")));
    let rssi_expr = lua("seen['$ADDR'].rssi");
    let v = t.eval(&rssi_expr);
    t.check(v == "-55", "and its signal strength", |t| format!("rssi: {}", t.eval(&rssi_expr)));
    let v = t.eval(r#"ble.uuid("0x2A19") .. " " .. ble.uuid("00002a19-0000-1000-8000-00805f9b34fb") .. " " .. tostring(ble.uuid("xyz"))"#);
    t.check(v == "2a19 2a19 nil", "ble.uuid: short and long forms of the same UUID agree", |t| {
        format!("uuid: {}", t.eval(r#"ble.uuid("0x2A19")"#))
    });

    t.write(
        "conn.lua",
        lua(r#"ble.stop_scan()
task(function()
  local c = assert(ble.connect("$ADDR"))
  local ok, err = ble.wait_connected(c)
  if not ok then log[#log + 1] = "connect: " .. err return end
  conn = c
  log[#log + 1] = "svcs " .. table.concat(c:services(), ",")
  log[#log + 1] = "bat " .. assert(c:read("180f", "2A19")):byte()
  log[#log + 1] = "mfr " .. assert(c:read("0000180a-0000-1000-8000-00805f9b34fb", "2a29"))
  log[#log + 1] = "rd " .. tostring(select(2, c:read("$SVC", "$NOTE")))
  log[#log + 1] = "wr " .. tostring(select(2, c:write("180f", "2a19", "x")))
  log[#log + 1] = "no " .. tostring(select(2, c:read("180f", "2a00")))
  assert(c:write("$SVC", "$RW", "world"))
  log[#log + 1] = "rw " .. assert(c:read("$SVC", "$RW"))
  assert(c:subscribe("$SVC", "$NOTE", function(d) notes = (notes or "") .. string.format("%02x", d:byte(-1)) end))
  assert(c:subscribe("180f", "2a19", function(d) bat = d:byte() end))
  log[#log + 1] = "subscribed"
end)
"#),
    );
    t.e(&["put", &t.path("conn.lua"), "--app", "bt"]);
    t.eval(r#"require("conn")"#);
    t.until_sim(3.0, |t| t.eval("log[#log]") == "subscribed");
    let v = t.eval("log[1]");
    t.check(v == format!("svcs 180a,180f,{SVC}"), "connects and discovers its services", |t| {
        format!("services: {}", t.eval("log[1]"))
    });
    let v = t.eval(r#"log[2] .. "|" .. log[3]"#);
    t.check(v == "bat 70|mfr ACME", "reads by 16-bit and long UUIDs", |t| format!("read: {}", t.eval("log[2]")));
    let v = t.eval(r#"log[4] .. "|" .. log[5]"#);
    t.check(
        v == format!("rd read {NOTE}: not readable|wr write 2a19: not writable"),
        "a characteristic that can't be read or written says so",
        |t| format!("refused: {}", t.eval("log[4]")),
    );
    let v = t.eval("log[6]");
    t.check(
        v == "no read 2a00: the device has no characteristic 2a00 in service 180f",
        "a missing characteristic is an error value",
        |t| format!("missing: {}", t.eval("log[6]")),
    );
    let v = t.eval("log[7]");
    t.check(v == "rw world", "a write is read back", |t| format!("write: {}", t.eval("log[7]")));
    let v = t.eval("notes");
    t.check(v == "ff01", "subscribing brings what the device sends then, in order", |t| {
        format!("first: {}", t.eval("notes"))
    });
    t.ctl(&[&format!("ble_value=test:{NOTE}:00 07"), "ble_value=test:2a19:37", &format!("ble_notify=test:{NOTE}:09")]);
    let ne = r#"notes .. " " .. tostring(bat)"#;
    let ok = t.until_sim(1.0, |t| t.eval(ne) == "ff010709 55");
    t.check(ok, "and every change after (notifications)", |t| format!("notify: {}", t.eval(ne)));
    let c = t.eval(
        r#"local t = {} for _, ch in ipairs(conn:characteristics("180f")) do
    t[#t + 1] = ch.uuid .. (ch.read and " r" or "") .. (ch.notify and " n" or "") .. (ch.write and " w" or "") end
    return table.concat(t, ",")"#,
    );
    t.check(c == "2a19 r n", "characteristics() lists what each can do", |_| format!("characteristics: {c}"));
    let v = t.eval("tostring(conn)");
    t.check(v == format!("ble({ADDR}, connected)"), "a connection prints its address and state", |t| {
        format!("tostring: {}", t.eval("tostring(conn)"))
    });
    let v = t.eval(r#"conn:read("180f", "2a19")"#);
    t.check(v.contains("only works inside task()"), "waiting outside a task is an error", |t| {
        format!("no task: {}", t.eval(r#"conn:read("180f", "2a19")"#))
    });
    t.ctl(&["ble=test:off"]);
    let ok = t.until_sim(3.0, |t| t.eval("conn:status()") == "closed\tconnection lost");
    t.check(ok, "out of range: the connection is lost", |t| format!("lost: {}", t.eval("conn:status()")));
    let again = lua("ble.connect('$ADDR')");
    let v = t.eval(&again);
    let want = format!("ble({ADDR}, connecting)");
    t.check(v.lines().any(|l| l.starts_with(&want)), "connecting again is allowed", |t| {
        format!("again: {}", t.eval(&again))
    });
    t.write(
        "gone.lua",
        lua("task(function() result = { ble.wait_connected(assert(ble.connect(\"$ADDR\")), 30000) } end)\n"),
    );
    t.e(&["put", &t.path("gone.lua"), "--app", "bt"]);
    t.e(&["run", "bt/gone.lua"]); // a new run: the old connections are closed
    t.advance(16000);
    t.wait_lua(3.0, "result ~= nil");
    let v = t.eval("result[2]");
    t.check(v == format!("cannot connect to {ADDR}: timeout"), "a device that's not there times out", |t| {
        format!("timeout: {}", t.eval("result and result[2]"))
    });
    t.stop();

    t.say("== ble: a characteristic that wants pairing");
    t.boot_with(&["--ble-device", &dev]);
    t.write(
        "pair.lua",
        lua(r#"task(function()
  local c = assert(ble.connect("$ADDR"))
  assert(ble.wait_connected(c))
  result = { c:read("$SVC", "$PAIRED") }
end)
function loop() end
"#),
    );
    t.e(&["put", &t.path("pair.lua"), "--app", "bt", "--run", "pair.lua"]);
    t.until_sim(3.0, |t| t.eval("result ~= nil") == "true");
    let v = t.eval("result[1] and result[1]:byte()");
    t.check(v == "42", "pairs by itself and reads again", |t| format!("pairing: {}", t.eval("result and result[2]")));
    t.write(
        "other.lua",
        lua(r#"task(function()
  local c = assert(ble.connect("$ADDR"))
  result = { ble.wait_connected(c) }
end)
function loop() end
"#),
    );
    t.e(&["put", &t.path("other.lua"), "--app", "bt2"]);
    t.e(&["stop"]);
    t.advance(300); // the first app's connection is down
    t.e(&["run", "bt2/other.lua"]);
    t.until_sim(3.0, |t| t.eval("result ~= nil") == "true");
    let v = t.eval("result[2]");
    t.check(v.contains("paired with app 'bt'"), "another app can't use a device paired by an app", |t| {
        format!("other app: {}", t.eval("result[1], result[2]"))
    });
    t.e(&["uninstall", "bt"]);
    t.e(&["run", "bt2/other.lua"]);
    t.until_sim(3.0, |t| t.eval("result ~= nil") == "true");
    let v = t.eval("result[1]");
    t.check(v == "true", "uninstalling that app deletes its bond", |t| {
        format!("after uninstall: {}", t.eval("result[1], result[2]"))
    });
    t.stop();

    t.say("== simulated devices: bad descriptions are refused");
    let bad_json = t.write("bad.json", r#"{ "name": "x", "characteristics": [] }"#);
    let out = Command::new(&t.sim_bin)
        .args(["--headless", "--nvs", &t.path("nvs2.bin"), "--ble-device", &bad_json, "--run-ms", "1"])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .output()
        .unwrap();
    let err = String::from_utf8_lossy(&out.stderr).into_owned();
    if out.status.success() {
        t.bad("a description without an address was taken");
    } else {
        t.check(err.contains(r#"needs "addr""#), "a description without an address says so", |_| format!("bad: {err}"));
    }

    t.say("== example di2: gear and battery on screen (examples/di2/sim_device.json)");
    t.wipe();
    let di2_dev = t.root.join("examples/di2/sim_device.json").to_string_lossy().into_owned();
    t.boot_with(&["--ble-device", &di2_dev, "--set", "ble=di2:off"]);
    let di2 = t.root.join("examples/di2").to_string_lossy().into_owned();
    t.e(&["install", &di2, "--run"]);
    t.advance(500);
    t.shot("screen.ppm");
    let n = amber(t);
    t.check(n == 0, "no derailleur: looking for one, no gear shown", |_| format!("amber before: {n}"));
    t.ctl(&["ble=di2:on"]);
    let ok = t.until_sim(5.0, |t| {
        t.shot("screen.ppm");
        amber(t) > 500
    });
    t.check(ok, "finds it, connects and shows the gear", |t| format!("live: {}", amber(t)));
    let v = t.eval(r#"store.get("addr")"#);
    t.check(v == "c7:18:2c:51:4d:01", "remembers the derailleur", |t| {
        format!("store: {}", t.eval(r#"store.get("addr")"#))
    });
    t.shot("before.ppm");
    t.ctl(&[&gear(2, 11)]);
    let ok = t.until_sim(1.0, |t| {
        t.shot("screen.ppm");
        !same(t, "screen.ppm", "before.ppm")
    });
    t.check(ok, "a shift changes the screen", |_| "no redraw after a shift".into());
    t.shot("before.ppm");
    t.ctl(&[&format!("ble_notify=di2:{GEAR}:01 ff ff ff ff ff ff ff 00 ff ff ff ff ff ff"), "advance=300"]);
    t.shot("screen.ppm");
    let ok = same(t, "screen.ppm", "before.ppm");
    t.check(ok, "other kinds of message (01 ff ...) don't change the gear", |_| {
        "a 01 message changed the screen".into()
    });
    t.ctl(&["ble=di2:off"]);
    let ok = t.until_sim(4.0, |t| {
        t.shot("screen.ppm");
        amber(t) == 0
    });
    t.check(ok, "losing it dims the last gear", |t| format!("lost: {}", amber(t)));
    t.stop();
}
