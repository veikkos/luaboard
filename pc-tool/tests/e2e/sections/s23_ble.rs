//! Bluetooth LE from Lua against simulated devices described in JSON
//! (platform/sim/models/ble_device.h).

use std::process::{Command, Stdio};

use crate::harness::T;

const ADDR: &str = "c0:ff:ee:00:00:01";
const SVC: &str = "12345678-1234-5678-1234-56789abcdef0";
const NOTE: &str = "12345678-1234-5678-1234-56789abcdef1"; // notifies; sends ff then its value when subscribed
const RW: &str = "12345678-1234-5678-1234-56789abcdef2"; // read and write
const PAIRED: &str = "12345678-1234-5678-1234-56789abcdef3"; // read, needs pairing

/// Lua with the names above filled in.
fn lua(s: &str) -> String {
    s.replace("$ADDR", ADDR).replace("$SVC", SVC).replace("$NOTE", NOTE).replace("$RW", RW).replace("$PAIRED", PAIRED)
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
  local ok, err = c:wait_connected()
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
        lua("task(function() result = { assert(ble.connect(\"$ADDR\")):wait_connected(30000) } end)\n"),
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
  assert(c:wait_connected())
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
  result = { c:wait_connected() }
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
}
