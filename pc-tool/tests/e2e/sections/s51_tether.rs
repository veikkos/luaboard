//! The USB tether: turning it on sets the clock, it stays on while the
//! computer is heard from, the computer's frames get no reply, and an app
//! without Wi-Fi reaches servers through the computer.

use std::io::{Read, Write};
use std::net::TcpListener;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use crate::fakes::{http, tls};
use crate::harness::{free_port, E2E_TIME, T};
use luaboard::tether::{Frame, Relay};
use luaboard::{kv_field, Device, Msg};

pub fn run(t: &mut T) {
    t.say("== USB tether");
    t.wipe();
    t.boot_with(&["--set", "wifi=0", "--set", "rtc=lost"]); // no clock at all
    let mut dev = t.dev();
    let mut evts = Vec::new();
    let usb_time = E2E_TIME + 1000;
    let on = dev.tun_on(usb_time, &mut |m| {
        if let Msg::Evt(e) = m {
            evts.push(e)
        }
    });
    let time = dev.time(&mut |_| {}).unwrap_or_default();
    let utc: u64 = kv_field(&time, "utc").and_then(|s| s.parse().ok()).unwrap_or(0);
    let ok = on.as_ref().is_ok_and(|started| *started)
        && kv_field(&time, "source") == Some("usb")
        && (usb_time..usb_time + 60).contains(&utc);
    t.check(ok, "TUN ON starts the tether and sets the clock from the computer's", |_| format!("{on:?}: {time}"));
    let told = evts.iter().any(|e| e.starts_with("time usb ")); // (before the reply)
    t.check(told, "and says so (EVT time usb)", |_| format!("events: {evts:?}"));

    // on while the computer is heard from: TUN ON or any other line
    let on = |dev: &mut Device| dev.tun_on(usb_time, &mut |_| {}).ok();
    let again = on(&mut dev);
    t.advance(3000);
    let _ = dev.ping(&mut |_| {});
    t.advance(3000);
    let still = on(&mut dev); // (6 s after TUN ON, 3 s after the PING)
    t.advance(5100);
    let ended = on(&mut dev);
    let ok = again == Some(false) && still == Some(false) && ended == Some(true);
    t.check(ok, "it stays on while the computer says anything, and ends 5 s after", |_| {
        format!("again {again:?}, after a PING {still:?}, after quiet {ended:?}")
    });

    // a frame for a connection the device doesn't have: no reply (the next
    // command's is its own), but a CLOSE, for the computer to drop it too
    let mut back = Vec::new();
    let sent = dev.tun_send(&[Frame::Data { id: 99, bytes: b"hi".to_vec() }]);
    let ping = dev.ping(&mut |m| {
        if let Msg::Tun(f) = m {
            back.push(f)
        }
    });
    let ok = sent.is_ok() && ping.is_ok() && back == ["CLOSE 99"];
    t.check(ok, "frames get no reply, and one for no connection a CLOSE", |_| format!("{sent:?} {ping:?} {back:?}"));

    let usage = dev.command("TUN ON", &mut |_| {}).map(|r| r.0);
    let ok = usage.as_ref().is_err_and(|e| e.to_string().contains("usage"));
    t.check(ok, "TUN ON needs the time", |_| format!("{usage:?}"));
    let off = dev.tun_off(&mut |_| {});
    t.check(off.is_ok(), "TUN OFF", |_| format!("{off:?}"));
    drop(dev);

    // NTP's time wins over the computer's
    t.crash();
    t.boot();
    let _ = t.dev().wifi_set("Home Net", "pw", &mut |_| {});
    // (the RTC chip has the computer's time now, so wait for the sync itself)
    let source = |t: &T| t.dev().time(&mut |_| {}).ok().and_then(|r| kv_field(&r, "source").map(String::from));
    t.until(3.0, |t| source(t).as_deref() == Some("ntp"));
    let mut dev = t.dev();
    let _ = dev.tun_on(E2E_TIME + 100_000, &mut |_| {});
    let time = dev.time(&mut |_| {}).unwrap_or_default();
    t.check(kv_field(&time, "source") == Some("ntp"), "but not over NTP's", |_| time.clone());
    drop(dev);
    t.crash();

    through(t);
}

/// Start `script` with the relay on.
fn start(dev: &mut Device, relay: &mut Relay, script: &str) {
    let _ = relay.pump(dev, Duration::ZERO); // (on before the script starts)
    let _ = dev.run(Some(script), &mut |m| {
        if let Msg::Tun(f) = m {
            relay.take(&f)
        }
    });
}

/// Keep the relay going until `done` (or 10 s), the script's lines into
/// `logs` (and its events, "[evt] ..."). The most connections the relay
/// had at once.
fn relay_until(
    dev: &mut Device,
    relay: &mut Relay,
    logs: &mut Vec<String>,
    mut done: impl FnMut(&[String], &Relay) -> bool,
) -> usize {
    let (end, mut most) = (Instant::now() + Duration::from_secs(10), 0);
    while Instant::now() < end && !done(logs, relay) {
        for m in relay.pump(dev, Duration::from_millis(2)).unwrap_or_default() {
            match m {
                Msg::Log(s) => logs.push(s),
                Msg::Evt(e) => logs.push(format!("[evt] {e}")),
                _ => {}
            }
        }
        most = most.max(relay.connections());
    }
    most
}

fn printed(logs: &[String], line: &str) -> bool {
    logs.iter().any(|l| l == line)
}

/// A server that does `serve` with each connection.
fn server(serve: fn(std::net::TcpStream)) -> u16 {
    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = l.local_addr().unwrap().port();
    std::thread::spawn(move || {
        for c in l.incoming() {
            let Ok(c) = c else { return };
            std::thread::spawn(move || serve(c));
        }
    });
    port
}

/// Sends back what it gets.
fn echo(mut c: std::net::TcpStream) {
    let mut b = [0u8; 4096];
    while let Ok(n @ 1..) = c.read(&mut b) {
        if c.write_all(&b[..n]).is_err() {
            return;
        }
    }
}

/// Says bye and ends the connection.
fn bye(mut c: std::net::TcpStream) {
    let _ = c.write_all(b"bye");
}

/// How many bytes the sink server got, once the connection ended.
static SUNK: AtomicUsize = AtomicUsize::new(0);

fn sink(mut c: std::net::TcpStream) {
    let mut all = Vec::new();
    let _ = c.read_to_end(&mut all);
    SUNK.store(all.len(), Ordering::SeqCst);
}

/// An app on a device without Wi-Fi reaches servers through the computer:
/// plain TCP both ways, more than a window of it, and HTTPS, the device
/// checking the certificate. Errors read as the device's own.
fn through(t: &mut T) {
    t.say("   an app's connections go through the computer");
    let cert = tls::self_signed();
    let body = |_: &http::Request| http::Response::new(200, "text/plain", "hello over usb");
    let web = http::serve(http::Opts { tls: Some(cert.config.clone()), ..Default::default() }, Arc::new(body));
    let (echo, bye, sink) = (server(echo), server(bye), server(sink));
    let nobody = free_port();
    let lua = format!(
        r#"local http = require("http")
local CA = [[{ca}]]
task(function()
  local s = assert(net.connect("127.0.0.1", {echo}))
  assert(s:wait_connected())
  local data = string.rep("0123456789abcdef", 1280) -- 20 KB: windows full both ways
  assert(s:write_all(data))
  local back = s:reader():read(#data, 5000)
  print("echo " .. tostring(back == data))
  s:close()
  local k = assert(net.connect("127.0.0.1", {sink}))
  assert(k:wait_connected())
  assert(k:write_all(string.rep("x", 3000)))
  k:close() -- (right after: its CLOSE comes with the data)
  local st, body = http.get("https://127.0.0.1:{web}/", nil, {{ ca = CA }})
  print("https " .. tostring(st) .. " " .. tostring(body))
  print("untrusted " .. tostring(select(2, http.get("https://127.0.0.1:{web}/"))))
  print("refused " .. tostring(select(2, net.connect("127.0.0.1", {nobody}):wait_connected())))
  print("done")
end)
function loop() end
"#,
        ca = cert.pem,
        web = web.port,
    );
    let late = format!(
        r#"task(function()
  local s = assert(net.connect("127.0.0.1", {bye}))
  assert(s:wait_connected())
  repeat sleep(1) until go -- (the test says when the server's end has come)
  print("late " .. tostring(s:send("thanks")) .. " " .. tostring(s:reader():read(3, 2000)))
end)
function loop() end
"#
    );
    let held = format!(
        r#"task(function()
  local s = assert(net.connect("127.0.0.1", {echo}))
  assert(s:wait_connected())
  print("held")
  print("ended " .. tostring(select(2, s:reader():chunk(60000))))
end)
function loop() end
"#
    );
    t.wipe();
    t.boot_with(&["--set", "wifi=0"]);
    t.write("main.lua", &lua);
    t.write("late.lua", &late);
    t.write("held.lua", &held);
    t.e(&["put", &t.path("main.lua"), &t.path("late.lua"), &t.path("held.lua"), "--app", "tt"]);
    let mut dev = t.dev();
    let mut relay = Relay::new(true); // (the servers are on this computer)
    relay.now = || E2E_TIME;
    let mut logs = Vec::new();
    start(&mut dev, &mut relay, "tt");
    let most = relay_until(&mut dev, &mut relay, &mut logs, |l, _| printed(l, "done"));
    t.check(most > 0 && printed(&logs, "echo true"), "20 KB to a server and back, through the computer", |_| {
        format!("connections {most}, logs: {logs:?}")
    });
    relay_until(&mut dev, &mut relay, &mut logs, |_, _| SUNK.load(Ordering::SeqCst) > 0);
    let sunk = SUNK.load(Ordering::SeqCst);
    t.check(sunk == 3000, "what an app sends just before it closes reaches the server", |_| format!("{sunk} bytes"));
    t.check(printed(&logs, "https 200 hello over usb"), "HTTPS through it, TLS on the device", |_| format!("{logs:?}"));
    let says = |start: &str, end: &str| logs.iter().any(|l| l.starts_with(start) && l.contains(end));
    let ok = says("untrusted cannot connect to ", ": TLS: certificate not trusted");
    t.check(ok, "the device checks the certificate itself", |_| format!("{logs:?}"));
    let ok = says("refused cannot connect to ", ": connection refused");
    t.check(ok, "a refused connection says so", |_| format!("{logs:?}"));

    // the server ends it while the app still writes: what it sent before
    // can still be read (once the device has the CLOSE, the relay holds
    // nothing; the EVAL comes after it)
    let mut logs = Vec::new();
    start(&mut dev, &mut relay, "tt/late.lua");
    let mut had = false;
    relay_until(&mut dev, &mut relay, &mut logs, |_, r| {
        had |= r.connections() > 0;
        had && r.connections() == 0
    });
    let _ = dev.eval("go = true", &mut |m| {
        if let Msg::Tun(f) = m {
            relay.take(&f)
        }
    });
    relay_until(&mut dev, &mut relay, &mut logs, |l, _| l.iter().any(|x| x.starts_with("late ")));
    t.check(printed(&logs, "late 6 bye"), "a server's last bytes are read after it has ended", |_| format!("{logs:?}"));

    // another relay (the computer's program restarted): the device's
    // connections through the earlier one end
    let mut logs = Vec::new();
    start(&mut dev, &mut relay, "tt/held.lua");
    relay_until(&mut dev, &mut relay, &mut logs, |l, _| printed(l, "held"));
    let mut relay = Relay::new(true);
    relay.now = || E2E_TIME;
    relay_until(&mut dev, &mut relay, &mut logs, |l, _| l.iter().any(|x| x.starts_with("ended ")));
    let ok = printed(&logs, "ended the computer's network went away");
    t.check(ok, "a new relay ends the connections an earlier one had", |_| format!("{logs:?}"));

    // the computer goes quiet: 5 s later the tether ends, and the relay
    // drops what it had when it next turns the tether on
    let mut logs = Vec::new();
    start(&mut dev, &mut relay, "tt/held.lua");
    relay_until(&mut dev, &mut relay, &mut logs, |l, _| printed(l, "held"));
    let had = relay.connections();
    t.advance(5100);
    t.until(1.0, |_| {
        logs.extend(dev.poll(Duration::from_millis(5)).unwrap_or_default().into_iter().filter_map(|m| match m {
            Msg::Log(s) => Some(s),
            _ => None,
        }));
        logs.iter().any(|l| l.starts_with("ended "))
    });
    relay.keep_every = Duration::ZERO; // (its next turn turns it on again)
    let _ = relay.pump(&mut dev, Duration::ZERO);
    let ok = printed(&logs, "ended the computer's network went away") && had == 1 && relay.connections() == 0;
    t.check(ok, "and an open connection ends with a reason, at both ends", |_| {
        format!("{logs:?}, relay had {had}, has {}", relay.connections())
    });
    drop(dev);
    t.crash();
}
