//! MQTT: the offline queue while the broker is gone, reconnecting, a broker
//! that hangs (keepalive), and the last will.

use std::io::{Read, Write};
use std::net::TcpListener;

use crate::fakes::mqtt::{self, Broker};
use crate::fakes::Log;
use crate::harness::{port_open, T};

/// The numbers published to dev1/n in these logs, sorted, each once, as
/// "1 2 3 " (as `sort -n | uniq | tr '\n' ' '`).
fn delivered(logs: &[&Log]) -> String {
    let mut n: Vec<i64> = logs.iter().flat_map(|l| numbers(l)).collect();
    n.sort();
    n.dedup();
    n.iter().map(|v| format!("{v} ")).collect()
}

/// The numbers published to dev1/n in one log, in the order received.
fn numbers(log: &Log) -> Vec<i64> {
    log.lines()
        .iter()
        .filter_map(|l| {
            let f: Vec<&str> = l.split_whitespace().collect();
            (f.get(2) == Some(&"dev1/n")).then(|| f.get(3).and_then(|v| v.parse().ok())).flatten()
        })
        .collect()
}

pub fn run(t: &mut T) {
    t.say("== MQTT: offline queue, reconnect, last will");
    let b1 = Broker::start(mqtt::Opts::default());
    let bp = b1.port;
    let log1 = b1.log.clone();
    t.wipe();
    t.boot();
    t.write(
        "q.lua",
        format!(
            r#"local mqtt = require("mqtt")
task(function()
  local c = assert(mqtt.connect("mqtt://127.0.0.1:{bp}", {{ client_id = "dev1", keepalive = 2, timeout = 1500,
    will = {{ topic = "dev1/status", payload = "offline" }} }}))
  c.on_close = function(e) print("on_close: " .. tostring(e)) end
  for i = 1, 12 do c:publish("dev1/n", i, {{ qos = 1 }}) sleep(300) end
end)
"#
        ),
    );
    t.e(&["put", &t.path("q.lua"), "--run", "q.lua"]);
    t.e_bg("q.mon", &["monitor", "--secs", "16"]);
    t.until_net(3.0, |_| log1.grep("dev1/n 3$"));
    b1.stop(); // broker gone: messages queue up
    drop(b1);
    t.advance(2000);
    // the same port again, once the old listener has let it go
    t.until(5.0, |_| !port_open(bp));
    let b2 = Broker::start(mqtt::Opts { port: bp, ..Default::default() });
    let log2 = b2.log.clone();
    let want = "1 2 3 4 5 6 7 8 9 10 11 12 ";
    let ok = t.until_net(15.0, |_| delivered(&[&log1, &log2]) == want);
    t.check(ok, "offline queue: all messages delivered after reconnect", |_| {
        format!("queue: got [{}]", delivered(&[&log1, &log2]))
    });
    let order = numbers(&log2);
    let mut sorted = order.clone();
    sorted.sort();
    let shown: String = order.iter().map(|v| format!("{v} ")).collect();
    t.check(sorted == order, "queued messages kept their order", |_| format!("order: {shown}"));
    b2.set_silent(true); // half-dead broker: TCP open, silent
    let ok = t.until_net(6.0, |t| t.read("q.mon").contains("keepalive timeout"));
    t.check(ok, "silent broker detected (keepalive)", |t| {
        format!("silent broker not detected; the monitor read: {} / device: {}", t.read("q.mon"), t.read("sim.err"))
    });
    b2.set_silent(false);
    t.crash();
    let ok = t.until(3.0, |_| log2.has("WILL dev1 dev1/status offline"));
    t.check(ok, "last will published when the device dies", |_| format!("no will: {}", log2.tail(3)));
    t.boot(); // clean close: no will
    t.write(
        "q2.lua",
        format!(
            r#"local mqtt = require("mqtt")
task(function()
  local c = assert(mqtt.connect("mqtt://127.0.0.1:{bp}", {{ client_id = "dev2", will = {{ topic = "dev2/status", payload = "offline" }} }}))
  c:publish("dev2/hello", "hi")
  c:close()
  R = true
end)
"#
        ),
    );
    t.e(&["put", &t.path("q2.lua"), "--run", "q2.lua"]);
    t.wait_lua(3.0, "R");
    t.until(3.0, |_| log2.has("CLOSE dev2"));
    t.stop();
    let ok = log2.has("PUB dev2 dev2/hello hi") && !log2.has("WILL dev2");
    t.check(ok, "no will after a clean close()", |_| "will on clean close".into());

    // A broker that sends a PUBLISH too short for its topic length.
    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    let rp = l.local_addr().unwrap().port();
    std::thread::spawn(move || {
        let Ok((mut c, _)) = l.accept() else { return };
        let _ = c.read(&mut [0u8; 256]); // CONNECT
        let _ = c.write_all(&[0x20, 2, 0, 0, 0x30, 1, 0]); // CONNACK, PUBLISH
        std::thread::sleep(std::time::Duration::from_secs(5));
    });
    t.boot();
    t.write(
        "q3.lua",
        format!(
            r#"local mqtt = require("mqtt")
task(function()
  local c = assert(mqtt.connect("mqtt://127.0.0.1:{rp}", {{ reconnect = false }}))
  c.on_close = function(e) R = e end
end)
function loop() end
"#
        ),
    );
    t.e(&["put", &t.path("q3.lua"), "--run", "q3.lua"]);
    t.wait_lua(3.0, "R ~= nil");
    let r = t.eval("R");
    let ok = r == "bad PUBLISH from the broker" && !t.e(&["info"]).contains("running: -");
    t.check(ok, "a malformed packet closes the connection, the app goes on", |t| {
        format!("R: {r}; info: {}", t.e(&["info"]))
    });
    t.stop();
}
