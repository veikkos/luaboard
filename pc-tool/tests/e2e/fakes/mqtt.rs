//! A minimal MQTT 3.1.1 broker. Every PUBLISH it receives is logged as
//! "PUB <client-id> <topic> <payload>". Supports CONNECT (with an optional
//! user/password check), SUBSCRIBE with + and # wildcards, PUBLISH QoS 0/1
//! (forwarded to subscribers as QoS 0), retained messages, PINGREQ,
//! DISCONNECT and the last will: if a client goes away without DISCONNECT,
//! its will is published and logged as "WILL <client-id> <topic> <payload>".
//! Also logged: CONNECT, AUTHFAIL, SUB and CLOSE <client-id>.
//! While silent, it reads and answers nothing: TCP stays open, as with a
//! broker that hangs.

use std::io::{self, Read, Write};
use std::net::TcpStream;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::mpsc::{self, Receiver, Sender};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use rustls::{ServerConfig, ServerConnection, StreamOwned};

use super::Log;

const CONNECT: u8 = 1;
const CONNACK: u8 = 2;
const PUBLISH: u8 = 3;
const PUBACK: u8 = 4;
const SUBSCRIBE: u8 = 8;
const SUBACK: u8 = 9;
const PINGREQ: u8 = 12;
const PINGRESP: u8 = 13;
const DISCONNECT: u8 = 14;

#[derive(Default, Clone)]
pub struct Opts {
    pub user: Option<String>,
    pub password: Option<String>,
    /// MQTT over TLS (as on port 8883).
    pub tls: Option<Arc<ServerConfig>>,
    /// The port (0: any free one).
    pub port: u16,
    /// Decides each CONNECT instead of user/password: (client id, user name,
    /// password) -> the CONNACK return code (0 accepts; 4 bad user name or
    /// password, 5 not authorized).
    pub auth: Option<Arc<Auth>>,
}

pub type Auth = dyn Fn(&str, &str, &str) -> u8 + Send + Sync;

struct Sub {
    client: usize,
    filter: String,
    out: Sender<Vec<u8>>,
}

#[derive(Default)]
struct State {
    subs: Vec<Sub>,
    /// topic -> payload, in the order first retained
    retained: Vec<(String, Vec<u8>)>,
}

pub struct Broker {
    pub port: u16,
    pub log: Log,
    state: Arc<Mutex<State>>,
    silent: Arc<AtomicBool>,
    stopping: Arc<AtomicBool>,
    conns: super::http::Conns,
}

impl Broker {
    pub fn start(opts: Opts) -> Broker {
        let (l, port, stopping, conns) = super::http::listen(opts.port);
        let log = Log::default();
        let silent = Arc::new(AtomicBool::new(false));
        let state = Arc::new(Mutex::new(State::default()));
        let ids = Arc::new(AtomicUsize::new(0));
        let (log2, silent2, stop2, conns2) = (log.clone(), silent.clone(), stopping.clone(), conns.clone());
        let state2 = state.clone();
        std::thread::spawn(move || {
            for s in l.incoming() {
                if stop2.load(Ordering::SeqCst) {
                    break;
                }
                let Ok(s) = s else { continue };
                let open = conns2.add(&s);
                let c = Client {
                    id: ids.fetch_add(1, Ordering::SeqCst),
                    opts: opts.clone(),
                    log: log2.clone(),
                    silent: silent2.clone(),
                    state: state2.clone(),
                    stopping: stop2.clone(),
                };
                std::thread::spawn(move || {
                    let _open = open;
                    c.run(s)
                });
            }
        });
        Broker { port, log, state, silent, stopping, conns }
    }

    /// Drop every client's connection and keep listening, as a broker that
    /// restarts (no wills: as for a killed broker process).
    pub fn kick(&self) {
        self.log.line("KICK");
        self.conns.close_all();
    }

    /// Deliver a message to the matching subscribers straight from the
    /// broker (as BMW's stream sends the car's values). Unlike publish(), it
    /// works with TLS too.
    pub fn push(&self, topic: &str, payload: &str) {
        self.log.line(format!("PUSH {topic}"));
        forward(&self.state.lock().unwrap(), topic, payload.as_bytes());
    }

    /// While silent, the broker reads and answers nothing.
    pub fn set_silent(&self, on: bool) {
        self.silent.store(on, Ordering::SeqCst);
    }

    /// Close the listener and every connection (wills are published as for
    /// clients that went away).
    pub fn stop(&self) {
        if self.stopping.swap(true, Ordering::SeqCst) {
            return;
        }
        let _ = TcpStream::connect(("127.0.0.1", self.port));
        self.conns.close_all();
    }
}

impl Drop for Broker {
    fn drop(&mut self) {
        self.stop();
    }
}

enum Conn {
    Plain(TcpStream),
    Tls(Box<StreamOwned<ServerConnection, TcpStream>>),
}

impl Conn {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        match self {
            Conn::Plain(s) => s.read(buf),
            Conn::Tls(s) => s.read(buf),
        }
    }
    fn send(&mut self, data: &[u8]) -> io::Result<()> {
        match self {
            Conn::Plain(s) => s.write_all(data),
            Conn::Tls(s) => {
                s.write_all(data)?;
                s.flush()
            }
        }
    }
}

fn enc_len(mut n: usize) -> Vec<u8> {
    let mut out = Vec::new();
    loop {
        let b = (n % 128) as u8;
        n /= 128;
        out.push(b | if n > 0 { 0x80 } else { 0 });
        if n == 0 {
            return out;
        }
    }
}

fn packet(first: u8, body: &[u8]) -> Vec<u8> {
    let mut p = vec![first];
    p.extend(enc_len(body.len()));
    p.extend_from_slice(body);
    p
}

fn mstr(s: &[u8]) -> Vec<u8> {
    let mut v = (s.len() as u16).to_be_bytes().to_vec();
    v.extend_from_slice(s);
    v
}

/// A whole packet from the front of `buf`, if there is one: (first byte, body).
fn take_packet(buf: &mut Vec<u8>) -> Option<(u8, Vec<u8>)> {
    let (mut len, mut mult, mut i) = (0usize, 1usize, 1usize);
    loop {
        let b = *buf.get(i)?;
        len += (b & 0x7f) as usize * mult;
        mult *= 128;
        i += 1;
        if b & 0x80 == 0 {
            break;
        }
    }
    if buf.len() < i + len {
        return None;
    }
    let first = buf[0];
    let body = buf[i..i + len].to_vec();
    buf.drain(..i + len);
    Some((first, body))
}

pub fn matches(filter: &str, topic: &str) -> bool {
    let f: Vec<&str> = filter.split('/').collect();
    let t: Vec<&str> = topic.split('/').collect();
    for (i, part) in f.iter().enumerate() {
        if *part == "#" {
            return true;
        }
        if i >= t.len() || (*part != "+" && *part != t[i]) {
            return false;
        }
    }
    f.len() == t.len()
}

/// A length-prefixed string at `pos` (moving past it).
fn take_str(body: &[u8], pos: &mut usize) -> Option<Vec<u8>> {
    let n = u16::from_be_bytes([*body.get(*pos)?, *body.get(*pos + 1)?]) as usize;
    let s = body.get(*pos + 2..*pos + 2 + n)?.to_vec();
    *pos += 2 + n;
    Some(s)
}

fn text(b: &[u8]) -> String {
    String::from_utf8_lossy(b).into_owned()
}

struct Client {
    id: usize,
    opts: Opts,
    log: Log,
    silent: Arc<AtomicBool>,
    state: Arc<Mutex<State>>,
    stopping: Arc<AtomicBool>,
}

impl Client {
    fn run(self, s: TcpStream) {
        // Short reads, so messages for this client go out in between.
        let _ = s.set_read_timeout(Some(Duration::from_millis(20)));
        let conn = match &self.opts.tls {
            Some(cfg) => match ServerConnection::new(cfg.clone()) {
                Ok(c) => Conn::Tls(Box::new(StreamOwned::new(c, s))),
                Err(_) => return,
            },
            None => Conn::Plain(s),
        };
        let (tx, rx) = mpsc::channel();
        let mut name = "?".to_string();
        let mut will: Option<(String, Vec<u8>, bool)> = None;
        let graceful = self.serve(conn, &tx, &rx, &mut name, &mut will).unwrap_or(false);
        if self.stopping.load(Ordering::SeqCst) {
            return; // as a broker process that was killed: no wills
        }
        let mut st = self.state.lock().unwrap();
        st.subs.retain(|s| s.client != self.id);
        if let (Some((topic, payload, retain)), false) = (will, graceful) {
            if retain {
                retain_msg(&mut st, &topic, &payload);
            }
            self.log.line(format!("WILL {name} {topic} {}", text(&payload)));
            forward(&st, &topic, &payload);
        }
        drop(st);
        self.log.line(format!("CLOSE {name}"));
    }

    /// The connection until it ends; Ok(true) after a DISCONNECT.
    fn serve(
        &self,
        mut conn: Conn,
        tx: &Sender<Vec<u8>>,
        rx: &Receiver<Vec<u8>>,
        name: &mut String,
        will: &mut Option<(String, Vec<u8>, bool)>,
    ) -> io::Result<bool> {
        let mut buf = Vec::new();
        let mut connected = false;
        let mut chunk = [0u8; 4096];
        loop {
            if connected && self.silent.load(Ordering::SeqCst) {
                std::thread::sleep(Duration::from_millis(20));
                continue;
            }
            while let Ok(p) = rx.try_recv() {
                conn.send(&p)?;
            }
            while let Some((first, body)) = take_packet(&mut buf) {
                let kind = first >> 4;
                if !connected {
                    if kind != CONNECT {
                        return Ok(false);
                    }
                    let mut pos = 0;
                    take_str(&body, &mut pos); // protocol name
                    pos += 1; // level
                    let flags = *body.get(pos).unwrap_or(&0);
                    pos += 3; // flags + keepalive
                    *name = text(&take_str(&body, &mut pos).unwrap_or_default());
                    if flags & 0x04 != 0 {
                        let t = text(&take_str(&body, &mut pos).unwrap_or_default());
                        let p = take_str(&body, &mut pos).unwrap_or_default();
                        *will = Some((t, p, flags & 0x20 != 0));
                    }
                    let user = (flags & 0x80 != 0).then(|| text(&take_str(&body, &mut pos).unwrap_or_default()));
                    let password = (flags & 0x40 != 0).then(|| text(&take_str(&body, &mut pos).unwrap_or_default()));
                    if let Some(auth) = &self.opts.auth {
                        let code = auth(name, user.as_deref().unwrap_or(""), password.as_deref().unwrap_or(""));
                        if code != 0 {
                            conn.send(&packet(CONNACK << 4, &[0, code]))?;
                            self.log.line(format!("AUTHFAIL {name} {code}"));
                            return Ok(false);
                        }
                    } else if self.opts.user.is_some()
                        && (user, password) != (self.opts.user.clone(), self.opts.password.clone())
                    {
                        conn.send(&packet(CONNACK << 4, &[0, 4]))?; // bad user name or password
                        self.log.line(format!("AUTHFAIL {name}"));
                        return Ok(false);
                    }
                    conn.send(&packet(CONNACK << 4, &[0, 0]))?;
                    self.log.line(format!("CONNECT {name}"));
                    connected = true;
                    continue;
                }
                match kind {
                    PUBLISH => {
                        let qos = (first >> 1) & 3;
                        let mut pos = 0;
                        let topic = text(&take_str(&body, &mut pos).unwrap_or_default());
                        if qos > 0 {
                            let pid = body.get(pos..pos + 2).unwrap_or(&[0, 0]).to_vec();
                            pos += 2;
                            conn.send(&packet(PUBACK << 4, &pid))?;
                        }
                        let payload = body.get(pos..).unwrap_or_default().to_vec();
                        let mut st = self.state.lock().unwrap();
                        if first & 1 != 0 {
                            if payload.is_empty() {
                                st.retained.retain(|(t, _)| *t != topic);
                            } else {
                                retain_msg(&mut st, &topic, &payload);
                            }
                        }
                        let shown =
                            if payload.len() < 200 { text(&payload) } else { format!("<{} bytes>", payload.len()) };
                        self.log.line(format!("PUB {name} {topic} {shown}"));
                        forward(&st, &topic, &payload);
                    }
                    SUBSCRIBE => {
                        let pid = body.get(..2).unwrap_or(&[0, 0]).to_vec();
                        let mut pos = 2;
                        let mut granted = Vec::new();
                        let mut st = self.state.lock().unwrap();
                        while pos < body.len() {
                            let Some(f) = take_str(&body, &mut pos) else { break };
                            let qos = *body.get(pos).unwrap_or(&0);
                            pos += 1;
                            let filter = text(&f);
                            self.log.line(format!("SUB {name} {filter}"));
                            st.subs.push(Sub { client: self.id, filter, out: tx.clone() });
                            granted.push(qos.min(1));
                        }
                        let mut ack = pid;
                        ack.extend(granted);
                        let mut out = packet(SUBACK << 4, &ack);
                        for (t, p) in &st.retained {
                            if st.subs.iter().any(|s| s.client == self.id && matches(&s.filter, t)) {
                                let mut b = mstr(t.as_bytes());
                                b.extend_from_slice(p);
                                out.extend(packet(PUBLISH << 4 | 1, &b));
                            }
                        }
                        drop(st);
                        conn.send(&out)?;
                    }
                    PINGREQ => conn.send(&packet(PINGRESP << 4, &[]))?,
                    DISCONNECT => return Ok(true),
                    _ => {}
                }
            }
            match conn.read(&mut chunk) {
                Ok(0) => return Ok(false),
                Ok(n) => buf.extend_from_slice(&chunk[..n]),
                Err(e) if matches!(e.kind(), io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut) => {}
                Err(e) => return Err(e),
            }
        }
    }
}

fn retain_msg(st: &mut State, topic: &str, payload: &[u8]) {
    match st.retained.iter_mut().find(|(t, _)| t == topic) {
        Some(e) => e.1 = payload.to_vec(),
        None => st.retained.push((topic.to_string(), payload.to_vec())),
    }
}

/// To every matching subscription (QoS 0).
fn forward(st: &State, topic: &str, payload: &[u8]) {
    let mut b = mstr(topic.as_bytes());
    b.extend_from_slice(payload);
    let p = packet(PUBLISH << 4, &b);
    for s in &st.subs {
        if matches(&s.filter, topic) {
            let _ = s.out.send(p.clone());
        }
    }
}

/// Publish one message as a client ("test-pub") and disconnect.
pub fn publish(port: u16, topic: &str, message: &str, user: Option<&str>, password: Option<&str>) -> io::Result<()> {
    let mut s = TcpStream::connect(("127.0.0.1", port))?;
    s.set_read_timeout(Some(Duration::from_secs(5)))?;
    let flags = 0x02 | if user.is_some() { 0x80 } else { 0 } | if password.is_some() { 0x40 } else { 0 };
    let mut body = mstr(b"MQTT");
    body.extend([4, flags, 0, 30]);
    body.extend(mstr(b"test-pub"));
    for v in [user, password].into_iter().flatten() {
        body.extend(mstr(v.as_bytes()));
    }
    s.write_all(&packet(CONNECT << 4, &body))?;
    let mut ack = [0u8; 4];
    s.read_exact(&mut ack)?;
    if ack[3] != 0 {
        return Err(io::Error::other(format!("broker refused: {}", ack[3])));
    }
    let mut p = mstr(topic.as_bytes());
    p.extend_from_slice(message.as_bytes());
    let mut out = packet(PUBLISH << 4, &p);
    out.extend(packet(DISCONNECT << 4, &[]));
    s.write_all(&out)?;
    Ok(())
}
