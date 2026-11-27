//! The Wi-Fi link (a simulated access point, TCP on this computer): pairing,
//! encryption, clients, the clock (NTP) and time zones.

use std::fs;
use std::io::{Read, Write};
use std::net::{Shutdown, TcpListener, TcpStream};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use crate::harness::{grep, E2E_TIME, WINDOWS};

use crate::harness::{ui, T};
use crate::sections::ota::Raw;

pub fn run(t: &mut T) {
    t.say("== Wi-Fi link (simulated AP, TCP on this Mac)");
    t.wipe();
    t.boot();
    let ok = t.eval("sys.time()") == "nil" && t.e(&["time"]).contains("not set yet");
    t.check(ok, "no time before Wi-Fi on a fresh device", |t| format!("time before Wi-Fi: {}", t.eval("sys.time()")));
    let ok = t.eval("net.wifi()") == "off";
    t.check(ok, "net.wifi(): off before a network is set", |t| format!("net.wifi(): {}", t.eval("net.wifi()")));
    let ok = t.e(&["wifi", "set", "Home Net", "pass word"]).contains("Home Net");
    t.check(ok, "credentials with spaces stored", |_| "wifi set".into());
    let wport = t.wifi_port(); // the port the simulator's Wi-Fi server took
    let w = format!("tcp://127.0.0.1:{wport}");
    let w = w.as_str();
    t.wifi_up();
    let ok = t.until_sim(3.0, |t| wi(t, w, &["ping"]).contains("not paired"));
    t.check(ok, "device reachable over Wi-Fi", |_| "no TCP connection".into());
    // The menu's Wi-Fi icon shows the signal: its arcs (inner, middle, outer)
    // along its middle, 17 px wide at the header's right margin (no battery
    // icon on the SE), lit or dim; the header redraws every 5 s.
    let cx = ui::W - ui::PAD - 17 + 7;
    let arcs = |t: &mut T, dbm: i32| {
        t.ctl(&[&format!("wifi_rssi={dbm}"), "advance=5200"]);
        t.eval(&format!(
            "local function lit(y) return (disp.get({cx}, y) >> 16) > 200 end return lit(16), lit(13), lit(10)"
        ))
    };
    let (strong, fair, weak) = (arcs(t, -50), arcs(t, -75), arcs(t, -90));
    let ok = strong == "true\ttrue\ttrue" && fair == "true\tfalse\tfalse" && weak == "false\tfalse\tfalse";
    t.check(ok, "the Wi-Fi icon's bars follow the signal", |_| {
        format!("arcs at -50/-75/-90 dBm: {strong} | {fair} | {weak}")
    });
    t.ctl(&["wifi_rssi=-52"]);
    // The access point gone: once a try to join has failed, a red cross on the icon
    let cross = format!("local c = disp.get({}, 16) return (c >> 16) > 200 and (c >> 8) & 255 < 120", cx + 4);
    t.ctl(&["wifi=0", "advance=8000"]);
    let away = t.eval(&cross);
    t.ctl(&["wifi=1"]);
    let ok = away == "true" && t.until_sim(40.0, |t| t.eval("net.wifi()").starts_with("connected"));
    t.ctl(&["advance=5200"]);
    let back = t.eval(&cross);
    t.check(ok && back == "false", "a network that can't be joined shows a cross, gone once it's back", |_| {
        format!("cross away/back: {away}/{back}")
    });
    let ok = wi(t, w, &["eval", "6 * 7"]).contains("not paired");
    t.check(ok, "not paired: the Wi-Fi link takes no commands", |t| {
        format!("Wi-Fi without pairing: {}", wi(t, w, &["eval", "6 * 7"]))
    });
    let ok = t.e(&["wifi"]).contains("luaboard pair");
    t.check(ok, "wifi status suggests pairing", |t| format!("no pairing hint: {}", t.e(&["wifi"])));
    let ok = grep("^paired: key [0-9a-f]{16}", &t.e(&["pair"]))
        && grep("^[0-9a-f]{16} [0-9a-f]{64}$", &fs::read_to_string(&t.keys).unwrap_or_default())
        && (WINDOWS || owner_only(t));
    t.check(ok, "pair over USB: the key is saved, readable only by you", |t| {
        format!("pair: {} / {}", t.e(&["pair"]), describe(t))
    });
    t.write("main.lua", "print(\"hi over wifi\")\nfunction loop() end\n");
    let out = wi(t, w, &["put", &t.path("main.lua"), "--run", "main.lua"]);
    t.check(out.contains("hi over wifi"), "upload + run + print over Wi-Fi", |_| format!("upload over Wi-Fi: {out}"));
    let ok = wi(t, w, &["eval", "6 * 7"]) == "42";
    t.check(ok, "eval over the encrypted Wi-Fi link", |_| "eval over Wi-Fi".into());
    let ok = grep("^connected\t[0-9.]*$", &t.eval("net.wifi()"));
    t.check(ok, "net.wifi(): connected, with the address", |t| format!("net.wifi(): {}", t.eval("net.wifi()")));
    // a runtime error is reported to the Wi-Fi listener, and the link survives it
    let _mon = monitor(t, w, "mon.txt");
    t.until(2.0, |t| grep("^monitoring", &t.read("mon.txt"))); // connected
    t.eval("timer.after(10, function() error(\"boom\") end)");
    t.e(&["run", "main.lua"]);
    t.eval("loop = function() error(\"kaboom\") end");
    let ok = t.until(3.0, |t| grep("error.*kaboom", &t.read("mon.txt")));
    t.check(ok, "script error reported to Wi-Fi listener", |_| "error not seen over Wi-Fi".into());
    let ok = wi(t, w, &["eval", "1 + 1"]) == "2";
    t.check(ok, "Wi-Fi link alive after script error", |_| "link died".into());

    // A secret in pieces over USB while the Wi-Fi link stores another one.
    let mut usb = Raw::start(t);
    usb.send(b"SECRET PART t key 6161\n");
    t.until(2.0, |_| usb.has("OK SECRET PART t key 2"));
    wi(t, w, &["secrets", "set", "other", "x", "y"]);
    usb.send(b"SECRET SET t key 6262\n");
    t.until(2.0, |_| usb.has("OK SECRET SET t key"));
    let _ = usb.finish();
    let list = t.e(&["secrets"]);
    t.check(grep("^t  *key .* 4 B", &list), "another link's secret doesn't cut one sent in pieces", |_| {
        format!("secrets: {list}")
    });

    // An upload that stalls over Wi-Fi ends, the client stays.
    let mut raw = Raw::start_on(t, w);
    raw.send(b"PING\n");
    t.until(2.0, |_| raw.pongs() == 1);
    t.advance(61000); // connected longer than a login may take
    raw.send(b"PUT t/x.lua 10 0\n");
    t.until(2.0, |_| raw.has("OK READY"));
    t.advance(3050);
    t.until(2.0, |_| raw.has("ERR upload timeout"));
    t.advance(600); // the quiet after it
    raw.send(b"PING\n");
    let ok = t.until(2.0, |_| raw.pongs() == 2);
    let out = raw.finish();
    t.check(ok && !out.contains("bye"), "after an upload timeout the Wi-Fi client is still logged in", |_| {
        format!("raw: {out}")
    });

    t.say("== Wi-Fi clients: only a login replaces a logged-in client");
    let _mon2 = monitor(t, w, "mon2.txt");
    t.until(2.0, |t| grep("^monitoring", &t.read("mon2.txt")));
    // strangers without the key, more of them than the device has room for
    let mut strangers = Vec::new();
    for _ in 0..4 {
        strangers.push(TcpStream::connect(("127.0.0.1", wport)));
        t.advance(100);
    }
    t.eval("print(\"still mine\")");
    let ok = t.until(2.0, |t| t.read("mon2.txt").contains("still mine")) && !t.read("mon2.txt").contains("bye");
    t.check(ok, "connections without a login don't kick the logged-in client", |t| {
        format!("logged-in client kicked: {}", t.read("mon2.txt"))
    });
    // two slots for strangers: the third pushed out the first, the fourth the
    // second (the oldest), not the third in the first one's slot
    let byes: Vec<bool> = strangers
        .iter_mut()
        .map(|s| s.as_mut().map(|s| read_line(s, 0.05).contains("another client connected")).unwrap_or(false))
        .collect();
    t.check(byes == [true, true, false, false], "a new connection pushes out the oldest stranger", |_| {
        format!("pushed out: {byes:?}")
    });
    let ok = wi(t, w, &["eval", "2 + 2"]) == "4";
    t.check(ok, "a new client still gets in (pushes out a stranger)", |_| "new client locked out".into());
    let ok = t.until(2.0, |t| t.read("mon2.txt").contains("bye another client logged in"));
    t.check(ok, "a newer login replaces the old logged-in client", |t| {
        format!("old client not replaced: {}", t.read("mon2.txt"))
    });
    drop(strangers);
    let mut idle = TcpStream::connect(("127.0.0.1", wport)).ok();
    t.advance(200);
    t.advance(61000);
    let line = idle.as_mut().map(|s| read_line(s, 2.0)).unwrap_or_default();
    t.check(line.contains("no login in time"), "a client that never logs in is dropped after 60 s", |_| {
        format!("idle stranger kept: '{line}'")
    });
    drop(idle);

    // a script on the device must not drive the device through its own Wi-Fi port
    t.eval(&format!("S = net.connect('127.0.0.1', {wport})"));
    t.until(2.0, |t| t.eval("S:status()") == "connected");
    t.eval("S:send(\"PING\\n\")");
    let ok = t.until_sim(2.0, |t| t.eval("select(2, S:recv())") == "closed")
        && !t.eval("R = S:recv() return R").contains("PONG");
    t.check(ok, "a script can't connect to the device's own control port", |t| {
        format!("self-connection: {}", t.eval("S:recv()"))
    });
    // ... not even to send a line and close before the device looks (3333 is the
    // control port as the firmware sees it; the simulator maps it to wport)
    t.eval("S = net.connect('127.0.0.1', 3333)");
    let ok = t.eval("select(2, S:status())").contains("control port");
    t.check(ok, "connecting to the control port fails at once", |t| format!("connect 3333: {}", t.eval("S:status()")));
    // ... nor to the simulator's own control channel
    t.eval(&format!("S = net.connect('127.0.0.1', {})", t.ctl_port()));
    let ok = t.until(2.0, |t| t.eval("S:status()").starts_with("closed"));
    t.check(ok, "a script can't reach the simulator's control channel", |t| {
        format!("connect to the control channel: {}", t.eval("S:status()"))
    });

    t.say("== clock (NTP) and time zone");
    t.wait_lua(2.0, "sys.time() ~= nil");
    // The world's clock started at E2E_TIME when the device did, and runs with it.
    let dt = t
        .eval(&format!("sys.time() - sys.millis() // 1000 - {E2E_TIME}"))
        .parse::<i64>()
        .map(|d| d.abs())
        .unwrap_or(99);
    t.check(dt <= 2, "sys.time() from NTP once Wi-Fi is up", |_| format!("sys.time() off by {dt} s"));
    const FI_TZ: &str = "EET-2EEST,M3.5.0/3,M10.5.0/4";
    let ok = t.eval("sys.timezone()") == FI_TZ && t.eval("sys.date().hour") == "15";
    t.check(ok, "default time zone: Finland (13:30 UTC is 15:30)", |t| {
        format!("default tz: {} hour {}", t.eval("sys.timezone()"), t.eval("sys.date().hour"))
    });
    let ok = t.e(&["tz", "stockholm"]).contains("Europe/Stockholm") && t.eval("sys.date().zone") == "CET";
    t.check(ok, "tz by city name", |t| format!("tz stockholm: {}", t.eval("sys.date().zone")));
    let ok = t.eval("sys.date(1783000000).hour") == "15";
    t.check(ok, "summer time rules (CEST in July)", |t| format!("CEST: {}", t.eval("sys.date(1783000000).hour")));
    let ok = t.e(&["tz", "X1"]).contains("not a POSIX TZ") && t.eval("sys.timezone()") == "CET-1CEST,M3.5.0,M10.5.0/3";
    t.check(ok, "device refuses a broken TZ string", |_| "bad tz accepted".into());
    let ok = t.eval(r#"sys.date(0, "UTC").year"#) == "1970" && t.e(&["info"]).contains("time: 20");
    t.check(ok, "sys.date(t, \"UTC\"); INFO has the time", |_| "date utc / info time".into());
    // Into seconds: a date string, a table in the device's zone (CET now) or another one.
    const TIMES: &str = r#"table.concat({ sys.time("2026-11-15T12:30:00+02:00"),
      sys.time({ year = 2026, month = 11, day = 15, hour = 11, min = 30 }),
      sys.time({ year = 2026, month = 11, day = 15, hour = 5, min = 30 }, "EST5EDT"),
      sys.time("Sun, 15 Nov 2026 10:30:00 GMT"), sys.time("2026-11-15 11:30"),
      sys.date(1794738600, "EST5EDT").hour, sys.date(1794738600, "EST5EDT").zone,
      select(2, sys.time("soon")), select(2, pcall(sys.date, 0, true)),
      select(2, pcall(sys.date, 0, "Mars/Base")) }, " ")"#;
    let out = t.eval(TIMES);
    let ok = out.starts_with("1794738600 1794738600 1794738600 1794738600 1794738600 5 EST not a date and time: soon")
        && out.contains("string expected")
        && out.contains("not a time zone: Mars/Base");
    t.check(ok, "sys.time from a string or a table, sys.date in any zone", |_| format!("times: {out}"));
    let ok = grep("^synced", &t.e(&["time", "--sync"]));
    t.check(ok, "time --sync asks the time server now", |t| format!("time --sync: {}", t.e(&["time", "--sync"])));
    t.e(&["tz", "helsinki"]);

    t.say("== Wi-Fi pairing and encryption");
    let line = match TcpStream::connect(("127.0.0.1", wport)) {
        Ok(mut s) => {
            let _ = s.write_all(b"PING\n");
            read_line(&mut s, 2.0)
        }
        Err(_) => String::new(),
    };
    t.check(line.starts_with("ERR this link is encrypted"), "clear-text commands are refused", |_| {
        format!("clear text: '{line}'")
    });
    // a client that hasn't logged in must not see script output
    let mut s = TcpStream::connect(("127.0.0.1", wport)).ok();
    t.advance(100);
    t.eval("print(\"secret log line\")");
    t.advance(100); // what the device sends for it, it has sent by now
    let out = s.as_mut().map(|s| read_line(s, 0.05)).unwrap_or_default();
    t.check(out.is_empty(), "no logs before login", |_| format!("logs leaked before login: {out}"));
    drop(s);
    // nothing readable on the wire: record a session through a relay
    let relay = Relay::start(wport);
    t.wait_port(relay.port);
    let out =
        t.cli(&["-p", &format!("tcp://127.0.0.1:{}", relay.port), "--linger", "0", "eval", r#""top" .. "secret""#]);
    let wire = relay.wire();
    let text = String::from_utf8_lossy(&wire);
    let ok = out == "topsecret"
        && !["topsecret", "PONG", "EVAL"].iter().any(|p| text.contains(p))
        && grep("^HELLO 1 ", &text);
    t.check(ok, "the session is encrypted (only HELLO in clear text)", |_| {
        format!("wire: {}", strings(&wire[..wire.len().min(300)], 5))
    });
    drop(relay);
    // a fake device that knows the key id but not the key
    let id = fs::read_to_string(&t.keys).unwrap_or_default().split(' ').next().unwrap_or("").to_string();
    let fake = FakeDevice::start(id);
    t.wait_port(fake.port);
    let out = t.cli(&["-p", &format!("tcp://127.0.0.1:{}", fake.port), "--linger", "0", "ping"]);
    t.check(out.contains("didn't decrypt"), "a fake device is caught (its replies don't decrypt)", |_| {
        format!("fake device: {out}")
    });
    drop(fake);
    // a client with the wrong key for the id
    t.write("wrong-keys", wrong_keys(&fs::read_to_string(&t.keys).unwrap_or_default()));
    let wrong = t.path("wrong-keys");
    let ok = wi_env(t, &wrong, w, &["eval", "6 * 7"]) != "42";
    t.check(ok, "a wrong key gets nowhere", |_| "wrong key accepted".into());
    let ok = wi(t, w, &["eval", "1 + 1"]) == "2";
    t.check(ok, "the right key still works", |_| "right key after a wrong one".into());
    let ok = wi(t, w, &["pair", "off"]).contains("only works over USB");
    t.check(ok, "pairing can't be changed over Wi-Fi", |t| format!("pair over Wi-Fi: {}", wi(t, w, &["pair", "off"])));
    let ok = t.eval("1 + 1") == "2" && t.e(&["pair"]).contains("this computer has the key");
    t.check(ok, "USB needs no key; status over USB", |t| format!("pair status: {}", t.e(&["pair"])));
    let old = t.path("old-keys");
    let _ = fs::copy(&t.keys, &old);
    let ok = grep("^paired: key", &t.e(&["pair", "--new"]))
        && wi_env(t, &old, w, &["eval", "1"]).contains("isn't paired")
        && wi(t, w, &["eval", "3"]) == "3";
    t.check(ok, "a new key unpairs other computers", |t| format!("pair --new: {}", wi_env(t, &old, w, &["eval", "1"])));
    let id2 = regex::Regex::new("key ([0-9a-f]*)")
        .unwrap()
        .captures(&t.e(&["pair"]))
        .map(|c| c[1].to_string())
        .unwrap_or_default();
    let ok = grep("^unpaired", &t.e(&["pair", "off"]))
        && wi(t, w, &["eval", "3"]).contains("not paired")
        && !grep(&format!("^{id2} "), &fs::read_to_string(&t.keys).unwrap_or_default());
    t.check(ok, "unpair: the Wi-Fi link takes no commands, the key is forgotten", |t| {
        format!("pair off: {}", wi(t, w, &["eval", "3"]))
    });
    let ok = t.e(&["wifi", "off"]).contains("state: off");
    t.check(ok, "Wi-Fi off", |_| "wifi off".into());
    t.stop();
}

/// The CLI over Wi-Fi (luaboard -p tcp://... --linger 0 ARGS).
fn wi(t: &T, w: &str, args: &[&str]) -> String {
    let mut all = vec!["-p", w, "--linger", "0"];
    all.extend_from_slice(args);
    t.cli(&all)
}

/// wi with another file of pairing keys.
fn wi_env(t: &T, keys: &str, w: &str, args: &[&str]) -> String {
    let mut all = vec!["-p", w, "--linger", "0"];
    all.extend_from_slice(args);
    t.cli_env(&[("LUABOARD_KEYS", keys)], &all)
}

/// The keys file is a plain file readable and writable only by its owner.
#[cfg(unix)]
fn owner_only(t: &T) -> bool {
    use std::os::unix::fs::PermissionsExt;
    fs::metadata(&t.keys).is_ok_and(|m| m.is_file() && m.permissions().mode() & 0o7777 == 0o600)
}
#[cfg(not(unix))]
fn owner_only(_: &T) -> bool {
    true
}

/// The keys file's mode, for a failure message (what `ls -l` showed).
fn describe(t: &T) -> String {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        if let Ok(m) = fs::metadata(&t.keys) {
            return format!("mode {:o} {}", m.permissions().mode(), t.keys.display());
        }
    }
    format!("{} missing?", t.keys.display())
}

/// The same key id with a wrong key: an "f" in front, cut to 64 digits.
fn wrong_keys(keys: &str) -> String {
    keys.lines()
        .map(|l| match l.split_once(' ') {
            Some((id, key)) => format!("{id} {}\n", format!("f{key}").chars().take(64).collect::<String>()),
            None => format!("{l}\n"),
        })
        .collect()
}

/// `luaboard -p W monitor --secs 3` in the background, its output to `out`.
pub(super) fn monitor(t: &T, w: &str, out: &str) -> Bg {
    let f = fs::File::create(t.work.join(out)).unwrap();
    let child = Command::new(&t.cli)
        .args(["-p", w, "monitor", "--secs", "3"])
        .env("LUABOARD_KEYS", &t.keys)
        .stdin(Stdio::null())
        .stdout(f.try_clone().unwrap())
        .stderr(f)
        .spawn()
        .unwrap();
    Bg(child)
}

/// A background process, stopped when dropped.
pub(super) struct Bg(Child);

impl Drop for Bg {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}

/// A line from a TCP connection (without its newline), waiting `secs` at most.
fn read_line(s: &mut TcpStream, secs: f64) -> String {
    let deadline = Instant::now() + Duration::from_secs_f64(secs);
    let mut line = Vec::new();
    let mut b = [0u8; 1];
    while Instant::now() < deadline {
        let _ =
            s.set_read_timeout(Some(deadline.saturating_duration_since(Instant::now()).max(Duration::from_millis(1))));
        match s.read(&mut b) {
            Ok(1) if b[0] == b'\n' => break,
            Ok(1) => line.push(b[0]),
            _ => break,
        }
    }
    String::from_utf8_lossy(&line).into_owned()
}

/// The printable runs of 4 characters or more in `data`, the first `max` (as `strings | head`).
fn strings(data: &[u8], max: usize) -> String {
    let mut runs = Vec::new();
    for run in data.split(|b| !(b.is_ascii_graphic() || *b == b' ' || *b == b'\t')) {
        if run.len() >= 4 {
            runs.push(String::from_utf8_lossy(run).into_owned());
        }
    }
    runs.truncate(max);
    runs.join("\n")
}

/// Wait for connections on a port of its own, polling a stop flag; `serve`
/// gets each one on the accepting thread.
fn accept_loop(
    stop: Arc<AtomicBool>,
    mut serve: impl FnMut(TcpStream) + Send + 'static,
) -> (u16, thread::JoinHandle<()>) {
    let srv = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = srv.local_addr().unwrap().port();
    srv.set_nonblocking(true).unwrap();
    let h = thread::spawn(move || {
        while !stop.load(Ordering::SeqCst) {
            match srv.accept() {
                Ok((c, _)) => {
                    let _ = c.set_nonblocking(false);
                    serve(c);
                }
                Err(_) => thread::sleep(Duration::from_millis(10)),
            }
        }
    });
    (port, h)
}

/// A TCP relay to the device's Wi-Fi port that records everything on the wire.
struct Relay {
    port: u16,
    wire: Arc<Mutex<Vec<u8>>>,
    stop: Arc<AtomicBool>,
    thread: Option<thread::JoinHandle<()>>,
}

impl Relay {
    fn start(dport: u16) -> Relay {
        let wire = Arc::new(Mutex::new(Vec::new()));
        let stop = Arc::new(AtomicBool::new(false));
        let rec = wire.clone();
        // (wait_port's probe connects too)
        let (port, h) = accept_loop(stop.clone(), move |c| {
            let Ok(d) = TcpStream::connect(("127.0.0.1", dport)) else { return };
            for (a, b) in [(d.try_clone(), c.try_clone()), (c.try_clone(), d.try_clone())] {
                let (Ok(a), Ok(b)) = (a, b) else { continue };
                let rec = rec.clone();
                thread::spawn(move || pump(a, b, rec));
            }
        });
        Relay { port, wire, stop, thread: Some(h) }
    }
    fn wire(&self) -> Vec<u8> {
        self.wire.lock().unwrap().clone()
    }
}

impl Drop for Relay {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::SeqCst);
        if let Some(h) = self.thread.take() {
            let _ = h.join();
        }
    }
}

/// Copy from `a` to `b`, recording it, until `a` ends; then close `b`.
fn pump(mut a: TcpStream, mut b: TcpStream, rec: Arc<Mutex<Vec<u8>>>) {
    let mut buf = [0u8; 65536];
    loop {
        match a.read(&mut buf) {
            Ok(0) | Err(_) => break,
            Ok(n) => {
                rec.lock().unwrap().extend_from_slice(&buf[..n]);
                if b.write_all(&buf[..n]).is_err() {
                    break;
                }
            }
        }
    }
    let _ = b.shutdown(Shutdown::Both);
}

/// A fake device that knows the key id but not the key: it answers HELLO and
/// then sends a frame of random bytes.
struct FakeDevice {
    port: u16,
    stop: Arc<AtomicBool>,
    thread: Option<thread::JoinHandle<()>>,
}

impl FakeDevice {
    fn start(id: String) -> FakeDevice {
        let stop = Arc::new(AtomicBool::new(false));
        // (wait_port's probe connects too)
        let (port, h) = accept_loop(stop.clone(), move |mut c| {
            let _ = c.set_read_timeout(Some(Duration::from_secs(5)));
            let mut buf = [0u8; 1000];
            match c.read(&mut buf[..100]) {
                Ok(0) | Err(_) => return,
                Ok(_) => {}
            }
            let mut nonce = [0u8; 16];
            getrandom::fill(&mut nonce).unwrap();
            let hex: String = nonce.iter().map(|b| format!("{b:02x}")).collect();
            if c.write_all(format!("OK HELLO 1 {id} {hex}\n").as_bytes()).is_err() {
                return;
            }
            let _ = c.read(&mut buf);
            let mut frame = vec![0u8; 2 + 0x15];
            frame[1] = 0x15;
            getrandom::fill(&mut frame[2..]).unwrap();
            let _ = c.write_all(&frame);
            let _ = c.read(&mut buf);
        });
        FakeDevice { port, stop, thread: Some(h) }
    }
}

impl Drop for FakeDevice {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::SeqCst);
        if let Some(h) = self.thread.take() {
            let _ = h.join();
        }
    }
}
