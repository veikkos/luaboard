//! Network from Lua: net (TCP in and out, name lookups) and mqtt (plain and TLS).

use std::io::Write;
use std::net::TcpListener;
use std::time::Instant;

use crate::fakes::mqtt::{self, Broker};
use crate::fakes::tls;
use crate::harness::{free_port, tcp_read, T};

pub fn run(t: &mut T) {
    t.say("== network from Lua (net, mqtt)");
    let broker = Broker::start(mqtt::Opts { user: Some("u".into()), password: Some("p".into()), ..Default::default() });
    let (bp, lp) = (broker.port, free_port());
    let log = broker.log.clone();
    t.wipe();
    t.boot();
    t.write(
        "mq.lua",
        format!(
            r#"local mqtt = require("mqtt")
R = {{}}
task(function()
  local bad, err = mqtt.connect("mqtt://127.0.0.1:{bp}", {{ client_id = "intruder", username = "u", password = "wrong" }})
  R.bad = tostring(err)
  local _, err2 = mqtt.connect("mqtt://127.0.0.1:1", {{}})
  R.nobroker = tostring(err2)
  local c = assert(mqtt.connect("mqtt://127.0.0.1:{bp}", {{ client_id = "dev1", username = "u", password = "p" }}))
  assert(c:subscribe("dev1/cmd/#", function(topic, msg) c:publish("dev1/echo", topic .. "=" .. msg) end))
  for i = 1, 3 do assert(c:publish("dev1/temp", tostring(20 + i), {{ qos = 1 }})) end
  R.published = true
end)
local srv = assert(net.listen({lp}))
task(function()
  while true do
    local cl = srv:accept()
    if cl then net.write_all(cl, "hello from luaboard\n") cl:close() end
    yield()
  end
end)
"#
        ),
    );
    t.e(&["put", &t.path("mq.lua"), "--run", "mq.lua"]);
    t.wait_lua(5.0, "R.published");
    let v = t.eval("R.bad");
    t.check(v == "broker refused: bad user name or password", "MQTT wrong password reported", |_| {
        format!("wrong password: {v}")
    });
    let v = t.eval("R.nobroker");
    t.check(v.starts_with("cannot connect to 127.0.0.1:1"), "MQTT unreachable broker reported", |_| {
        format!("no broker: {v}")
    });
    let ok = t.until(2.0, |_| log.has("PUB dev1 dev1/temp 23"));
    t.check(ok, "MQTT connect + QoS 1 publish", |_| format!("publish: {}", log.text()));
    if let Err(e) = mqtt::publish(bp, "dev1/cmd/led", "on", Some("u"), Some("p")) {
        t.bad(&format!("publishing to the broker: {e}"));
    }
    let ok = t.until(3.0, |_| log.has("PUB dev1 dev1/echo dev1/cmd/led=on"));
    t.check(ok, "MQTT subscribe: message in, reply out", |_| format!("subscribe: {}", log.text()));
    let ok = tcp_read(lp) == "hello from luaboard";
    t.check(ok, "net.listen: inbound TCP", |_| "net.listen".into());
    t.e(&["run", "mq.lua"]); // restart: old sockets must be closed
    let ok = t.until(2.0, |_| tcp_read(lp) == "hello from luaboard");
    t.check(ok, "sockets closed on script restart", |_| "port still taken after restart".into());
    t.stop();
    drop(broker);

    t.say("== mqtt over TLS (mqtts://)");
    let cert = tls::self_signed();
    let tb = Broker::start(mqtt::Opts { tls: Some(cert.config.clone()), ..Default::default() });
    let (sp, tlog) = (tb.port, tb.log.clone());
    let cert13 = tls::self_signed_tls13();
    let tb13 = Broker::start(mqtt::Opts { tls: Some(cert13.config.clone()), ..Default::default() });
    let (sp13, tlog13) = (tb13.port, tb13.log.clone());
    t.boot();
    t.write(
        "tls.lua",
        format!(
            r#"CA = [[
{}]]
CA13 = [[
{}]]
local mqtt = require("mqtt")
R = {{}}
task(function()
  local _, err = mqtt.connect("mqtts://127.0.0.1:{sp}", {{ client_id = "untrusting", reconnect = false }})
  R.untrusted = tostring(err)
  local c, err2 = mqtt.connect("mqtts://127.0.0.1:{sp}", {{ client_id = "tls1", ca = CA }})
  R.err = tostring(err2)
  if c then assert(c:publish("tls1/hello", "secure", {{ qos = 1 }})) end
  local c13, err3 = mqtt.connect("mqtts://127.0.0.1:{sp13}", {{ client_id = "tls13", ca = CA13, reconnect = false }})
  R.err13 = tostring(err3)
  if c then R.done = true end
end)
"#,
            cert.pem, cert13.pem
        ),
    );
    t.e(&["put", &t.path("tls.lua"), "--run", "tls.lua"]);
    t.wait_lua(8.0, r#"R.done or R.err ~= nil and R.err ~= "nil""#);
    let ok = t.until(3.0, |_| tlog.has("PUB tls1 tls1/hello secure"));
    t.check(ok, "mqtts:// with the broker's CA: connect + QoS 1 publish", |t| {
        format!("mqtts: {} / {}", t.eval("R.err"), tlog.text())
    });
    let v = t.eval("R.untrusted");
    t.check(v.to_lowercase().contains("certificate not trusted"), "mqtts:// refuses a broker it can't verify", |_| {
        format!("untrusted: {v}")
    });
    let ok = tlog13.has("CONNECT tls13");
    t.check(ok, "MQTT over TLS 1.3 only", |t| format!("tls 1.3: {} / {}", t.eval("R.err13"), tlog13.text()));
    t.stop();
    drop(tb);
    drop(tb13);

    t.say("== name lookups don't block the device");
    t.boot();
    let srv = TcpListener::bind("127.0.0.1:0").unwrap();
    let ep = srv.local_addr().unwrap().port();
    std::thread::spawn(move || {
        for mut c in srv.incoming().flatten() {
            let _ = c.write_all(b"hi by name\n");
        }
    });
    t.write("main.lua", "function loop() end\n");
    t.e(&["put", &t.path("main.lua"), "--app", "dns", "--run", "main.lua"]);
    // (a name under .invalid: the simulator's own "nobody answers", 3 s of the device's time)
    let st = t.eval(r#"S = net.connect("no-such-device.invalid", 80) return S:status()"#);
    let t0 = Instant::now();
    let answered = t.eval("1 + 1") == "2";
    let secs = t0.elapsed().as_secs_f64();
    t.check(
        answered && secs < 1.5 && st == "connecting",
        &format!("a slow lookup runs in the background (device answered in {secs:.1}s, socket connecting)"),
        |_| format!("lookup blocked: {secs:.1}s, {st}"),
    );
    t.advance(3100);
    t.until_sim(2.0, |t| !t.eval("S:status()").contains("connecting"));
    let v = t.eval("S:status()");
    t.check(v.contains("cannot resolve no-such-device.invalid"), "an unknown name ends as 'cannot resolve'", |_| {
        format!("unknown name: {v}")
    });
    t.write(
        "byname.lua",
        format!(
            r#"R = nil
task(function()
  local s = assert(net.connect("localhost", {ep}))
  assert(net.wait_connected(s, 5000))
  local line = ""
  while not line:find("\n") do local d = s:recv() if not d then break end line = line .. d yield() end
  R = line
end)
"#
        ),
    );
    t.e(&["put", &t.path("byname.lua"), "--app", "dns", "--run", "byname.lua"]);
    t.wait_lua(5.0, "R ~= nil");
    let v = t.eval(r#"(R:gsub("\n$", ""))"#);
    t.check(v == "hi by name", "a host name resolves and connects", |t| format!("by name: {}", t.eval("R")));
    t.stop();
}
