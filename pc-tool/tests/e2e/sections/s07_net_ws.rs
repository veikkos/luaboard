//! WebSocket from Lua (ws.lua): the upgrade, messages both ways, pings,
//! messages in several frames, closing, and what isn't a WebSocket server.

use crate::fakes::{tls, ws};
use crate::harness::T;

const LUA: &str = r#"local ws = require("ws")
R = {}
local U = "ws://127.0.0.1:$PORT"
task(function()
  local w = assert(ws.connect(U .. "/chat", { headers = { ["X-Test"] = "1" } }))
  assert(w:send("hello"))
  R.echo = table.concat({ tostring(w:recv(2000)) }, " ")
  w:send("\0\1\2", true)
  local d, bin = w:recv(2000)
  R.bin = #d .. " " .. tostring(bin)
  w:send("ping")
  R.ping = w:recv(2000)                         -- (the server pinged first: answered inside recv)
  w:send("pieces")
  R.pieces = w:recv(2000)
  w:send("big 70000")
  R.big = #w:recv(5000)
  w:send(string.rep("y", 65536))
  R.sent = #w:recv(5000)
  R.quiet = select(2, w:recv(100))
  -- The time runs out between a message's frames: the next call still gets it whole.
  w:send("slow")
  R.half = select(2, w:recv(300))
  w:send("more")
  R.whole = w:recv(2000)
  -- Sent outside a task (from the PC: EVAL), more than the socket takes at once.
  W = w
  OUT = 0
  while OUT < 3 do yield() end
  R.outside = {}
  for i = 1, 3 do R.outside[i] = #(w:recv(5000) or "") end
  R.outside = table.concat(R.outside, " ")
  R.toolong = select(2, pcall(w.send, w, string.rep("y", 65537)))
  w:send("bye")
  R.bye = select(2, w:recv(2000))
  R.after = select(2, w:send("x")) .. " " .. select(2, w:recv(10))
  R.badkey = select(2, ws.connect(U .. "/badkey"))
  R.plain = select(2, ws.connect(U .. "/plain"))
  R.nobody = select(2, ws.connect("ws://127.0.0.1:1/"))
  local t, err = ws.connect("wss://127.0.0.1:$TLS/", { ca = CA })
  if t then t:send("secure") R.tls = t:recv(2000) t:close() else R.tls = err end
  R.untrusted = select(2, ws.connect("wss://127.0.0.1:$TLS/"))
  R.done = true
end)
function loop() end
"#;

pub fn run(t: &mut T) {
    t.say("== WebSocket from Lua (ws)");
    let cert = tls::self_signed();
    let plain = ws::start(None);
    let secure = ws::start(Some(cert.config.clone()));
    t.wipe();
    t.boot();
    let lua = LUA.replace("$PORT", &plain.port.to_string()).replace("$TLS", &secure.port.to_string());
    // (not "ws.lua": an app's file of that name would stand in for the built-in one)
    t.write("chat.lua", format!("CA = [[\n{}]]\n{lua}", cert.pem));
    t.e(&["put", &t.path("chat.lua"), "--app", "wsapp", "--run", "chat.lua"]);
    // The script waits for three messages sent outside a task, from here.
    t.wait_lua(15.0, "W ~= nil");
    let sent = t.eval(r#"local ok = 0 for i = 1, 3 do if W:send(string.rep(string.char(64 + i), 65536)) then ok = ok + 1 end end OUT = 3 return ok, #W.out"#);
    t.wait_lua(15.0, "R.done");

    let v = t.eval("R.echo, R.bin");
    let ok = v == "hello\t3 true" && plain.log.has("OPEN /chat") && plain.log.has("HDR x-test: 1");
    t.check(ok, "ws.connect, with its headers; text and binary messages come back as sent", |_| {
        format!("echo, binary: {v} / {}", plain.log.tail(4))
    });
    let v = t.eval("R.ping, R.pieces");
    let ok = v == "ping\tone two three" && plain.log.has("PONG p1") && plain.log.has("PONG p2");
    t.check(ok, "the server's pings are answered, and a message in several frames is one", |_| {
        format!("after a ping, in pieces: {v} / {}", plain.log.text())
    });
    let v = t.eval("R.big, R.sent, R.quiet");
    let ok = v == "70000\t65536\ttimeout" && !plain.log.has("UNMASKED");
    t.check(ok, "long messages both ways, masked on the way out; nothing in time is a timeout", |_| {
        format!("70000 bytes in, 65536 out and back, a quiet 100 ms: {v} / unmasked: {}", plain.log.has("UNMASKED"))
    });
    let v = t.eval("R.half, R.whole, R.outside");
    let whole = plain.log.count("MSG 65536 bytes") >= 4;
    let ok = v == "timeout\tfirst last\t65536 65536 65536" && sent.starts_with("3\t") && whole;
    t.check(
        ok,
        "a timeout between a message's frames loses nothing; sends outside a task go out whole, in order",
        |_| format!("split by a timeout: {v} / sent from EVAL (count, left queued): {sent} / {}", plain.log.tail(6)),
    );
    let v = t.eval("R.bye, R.after");
    let long = t.eval("R.toolong");
    let ok = v == "closed\tclosed closed" && long.contains("at most 65536 bytes") && long.contains("chat.lua:");
    t.check(ok, "the server's close ends it; a message over 64 KiB is a wrong call", |_| {
        format!("closed: {v} / too long: {long}")
    });
    let v = t.eval("R.badkey");
    let p = t.eval("R.plain");
    let n = t.eval("R.nobody");
    let ok = v.contains("wrong answer to the key")
        && p.contains("not a WebSocket: HTTP/1.1 200")
        && n.contains("cannot connect");
    t.check(ok, "a server that isn't a WebSocket one, or isn't there, is an error value", |_| {
        format!("wrong key: {v} / plain http: {p} / nobody: {n}")
    });
    let v = t.eval("R.tls");
    let u = t.eval("R.untrusted");
    let ok = v == "secure" && u.contains("certificate not trusted") && secure.log.has("MSG secure");
    t.check(ok, "wss:// with the server's CA; without it the certificate isn't trusted", |_| {
        format!("wss: {v} / untrusted: {u}")
    });
    let v = t.eval(r#"select(2, pcall(require("ws").connect, "ws://127.0.0.1:1/"))"#);
    let bad = t.eval(r#"select(2, pcall(require("ws").connect, 5))"#);
    let ok = v.contains("inside task()") && bad.contains("inside task()");
    t.check(ok, "ws.connect outside a task is a wrong call", |_| format!("outside a task: {v} / {bad}"));
    t.stop();
}
