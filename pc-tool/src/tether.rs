//! The USB tether (`docs/PROTOCOL.md`, "USB tether"): the device's TCP
//! connections, made by this computer and carried as `TUN` lines. `Frame`
//! is one line, `Relay` lends the device this computer's network.

use std::collections::BTreeMap;
use std::io::{self, Read, Write};
use std::net::{IpAddr, SocketAddr, TcpStream, ToSocketAddrs};
use std::sync::mpsc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use anyhow::Result;

use crate::{is_link_error, Device, ErrReply, Msg};

/// Bytes of `DATA` one side may have that the other hasn't acknowledged.
pub const WINDOW: usize = 8192;
/// Bytes in one `DATA` frame at most.
pub const DATA_MAX: usize = 1500;
/// How often to send `TUN ON` at least: the device ends the tether when it
/// hasn't heard from the computer for 5 s.
pub const KEEP_EVERY: Duration = Duration::from_secs(2);

/// One frame, either way, without the `TUN ` in front.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Frame {
    /// The device asks for a connection.
    Open {
        id: u32,
        host: String,
        port: u16,
    },
    /// The computer has connected.
    Opened {
        id: u32,
    },
    Data {
        id: u32,
        bytes: Vec<u8>,
    },
    Ack {
        id: u32,
        n: usize,
    },
    /// `why`: None if it ended normally.
    Close {
        id: u32,
        why: Option<String>,
    },
}

impl Frame {
    /// A frame from the words after `TUN`. None for what isn't one (the
    /// device ignores those too).
    pub fn parse(text: &str) -> Option<Frame> {
        let mut words = text.splitn(3, ' ');
        let kind = words.next()?.to_ascii_uppercase();
        let id: u32 = words.next()?.parse().ok()?;
        let rest = words.next().unwrap_or("").trim();
        match kind.as_str() {
            "OPEN" => {
                let (host, port) = rest.split_once(' ')?;
                Some(Frame::Open { id, host: host.to_string(), port: port.parse().ok()? })
            }
            "OPENED" => Some(Frame::Opened { id }),
            "DATA" => {
                let bytes = unbase64(rest)?;
                (!bytes.is_empty() && bytes.len() <= DATA_MAX).then_some(Frame::Data { id, bytes })
            }
            "ACK" => Some(Frame::Ack { id, n: rest.parse().ok()? }),
            "CLOSE" => Some(Frame::Close { id, why: (!rest.is_empty()).then(|| rest.to_string()) }),
            _ => None,
        }
    }

    /// The protocol line, without its newline.
    pub fn line(&self) -> String {
        match self {
            Frame::Open { id, host, port } => format!("TUN OPEN {id} {host} {port}"),
            Frame::Opened { id } => format!("TUN OPENED {id}"),
            Frame::Data { id, bytes } => format!("TUN DATA {id} {}", base64(bytes)),
            Frame::Ack { id, n } => format!("TUN ACK {id} {n}"),
            Frame::Close { id, why: None } => format!("TUN CLOSE {id}"),
            // (one line: a reason can't end it early)
            Frame::Close { id, why: Some(why) } => format!("TUN CLOSE {id} {}", why.replace(['\r', '\n'], " ")),
        }
    }
}

/// How long the computer tries to connect for the device.
const CONNECT_TIMEOUT: Duration = Duration::from_secs(10);
/// How long a connection the device has closed may take to hand the server
/// its last bytes.
const LINGER: Duration = Duration::from_secs(10);
/// How long the relay counts as busy after bytes last moved.
const BUSY_FOR: Duration = Duration::from_millis(250);

enum Conn {
    /// The name lookup and the connect, on a thread of their own.
    Connecting(mpsc::Receiver<Result<TcpStream, String>>),
    Open {
        stream: TcpStream,
        /// The device's bytes that the server hasn't taken yet.
        to_server: Vec<u8>,
        /// Bytes sent to the device that it hasn't acknowledged.
        unacked: usize,
        /// The device has closed it: by when `to_server` is to be written.
        closing: Option<Instant>,
    },
}

/// Lends the device this computer's network while `pump` is called, often:
/// every few milliseconds while `busy`, else every few tens, and at least
/// every `keep_every`.
pub struct Relay {
    /// Let the device connect to this computer's own addresses (127.0.0.1,
    /// ::1). Off: a script could reach services that only listen there.
    pub allow_local: bool,
    /// The time `TUN ON` tells the device, UTC seconds (the clock's).
    pub now: fn() -> u64,
    /// How often `TUN ON` goes out (KEEP_EVERY).
    pub keep_every: Duration,
    conns: BTreeMap<u32, Conn>,
    /// When `TUN ON` last went out on this link (None: not yet).
    last_on: Option<Instant>,
    /// The link it lends the network over (`Device::serial`): the
    /// connections are that link's.
    link: Option<u64>,
    /// The device turned the tether down (firmware without it): not asked
    /// again on this link.
    refused: bool,
    /// When bytes last moved.
    moved: Option<Instant>,
}

fn utc_now() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

impl Relay {
    pub fn new(allow_local: bool) -> Relay {
        Relay {
            allow_local,
            now: utc_now,
            keep_every: KEEP_EVERY,
            conns: BTreeMap::new(),
            last_on: None,
            link: None,
            refused: false,
            moved: None,
        }
    }

    /// The device's connections through this computer, open or opening.
    pub fn connections(&self) -> usize {
        self.conns.len()
    }

    /// Whether to come back at once: a connection is being made, or has
    /// bytes to hand on or moved some a moment ago.
    pub fn busy(&self) -> bool {
        self.moved.is_some_and(|t| t.elapsed() < BUSY_FOR)
            || self.conns.values().any(|c| match c {
                Conn::Connecting(_) => true,
                Conn::Open { to_server, .. } => !to_server.is_empty(),
            })
    }

    /// Keep the tether on and move the connections along, waiting up to
    /// `wait` for the device's lines. Its other messages (LOG, EVT) are
    /// returned. Frames that come with the replies of other commands go to
    /// `take`. An error is a broken link, or the device turning the tether
    /// down (then it's not asked again on this link: `ErrReply`).
    pub fn pump(&mut self, dev: &mut Device, wait: Duration) -> Result<Vec<Msg>> {
        if self.link != Some(dev.serial()) {
            // Another link (a reconnect): the connections were the old one's.
            self.conns.clear();
            self.last_on = None;
            self.refused = false;
            self.link = Some(dev.serial());
        }
        let mut msgs = Vec::new();
        if !self.refused && self.last_on.is_none_or(|t| t.elapsed() >= self.keep_every) {
            let first = self.last_on.is_none();
            self.last_on = Some(Instant::now()); // (after a failure too: again in keep_every)
            match self.turn_on(dev, first, &mut msgs) {
                // The tether had ended (or the device restarted): the
                // device's ends of these connections are gone.
                Ok(true) => self.conns.clear(),
                Ok(false) => {}
                Err(e) if e.downcast_ref::<ErrReply>().is_some() => {
                    self.refused = true;
                    return Err(e);
                }
                Err(e) if is_link_error(&e) => return Err(e),
                Err(_) => {} // no answer in time (the device is busy): next time
            }
        }
        msgs.extend(dev.poll(wait)?);
        let mut other = Vec::new();
        for m in msgs {
            match m {
                Msg::Tun(frame) => self.take(&frame),
                m => other.push(m),
            }
        }
        let mut out = Vec::new();
        self.conns.retain(|&id, conn| step(id, conn, &mut out));
        if !out.is_empty() {
            self.moved = Some(Instant::now());
            dev.tun_send(&out)?;
        }
        Ok(other)
    }

    /// TUN ON: true if the tether starts now.
    fn turn_on(&self, dev: &mut Device, first: bool, msgs: &mut Vec<Msg>) -> Result<bool> {
        // New on this link: what an earlier program left on the device is
        // over (its sockets went with it).
        if first {
            dev.tun_off(&mut |m| msgs.push(m))?;
        }
        dev.tun_on((self.now)(), &mut |m| msgs.push(m))
    }

    /// A frame from the device (`Msg::Tun`'s text).
    pub fn take(&mut self, frame: &str) {
        let frame = Frame::parse(frame);
        if frame.is_some() {
            self.moved = Some(Instant::now());
        }
        match frame {
            Some(Frame::Open { id, host, port }) => {
                let (tx, rx) = mpsc::channel();
                let allow_local = self.allow_local;
                std::thread::spawn(move || tx.send(connect(&host, port, allow_local)));
                self.conns.insert(id, Conn::Connecting(rx));
            }
            Some(Frame::Data { id, bytes }) => {
                if let Some(Conn::Open { to_server, .. }) = self.conns.get_mut(&id) {
                    to_server.extend(bytes);
                }
            }
            Some(Frame::Ack { id, n }) => {
                if let Some(Conn::Open { unacked, .. }) = self.conns.get_mut(&id) {
                    *unacked = unacked.saturating_sub(n);
                }
            }
            Some(Frame::Close { id, why }) => match self.conns.get_mut(&id) {
                // Its last bytes still go to the server.
                Some(Conn::Open { to_server, closing, .. }) if why.is_none() && !to_server.is_empty() => {
                    closing.get_or_insert(Instant::now() + LINGER);
                }
                _ => {
                    self.conns.remove(&id);
                }
            },
            Some(Frame::Opened { .. }) | None => {}
        }
    }

    /// End the tether now: the device's connections through it close.
    pub fn stop(&mut self, dev: &mut Device) -> Result<()> {
        self.conns.clear();
        self.last_on = None;
        dev.tun_off(&mut |_| {})
    }
}

/// Move one connection along, its frames to `out`. False once it's over.
fn step(id: u32, conn: &mut Conn, out: &mut Vec<Frame>) -> bool {
    let fail = |out: &mut Vec<Frame>, why: String| {
        out.push(Frame::Close { id, why: Some(why) });
        false
    };
    if let Conn::Connecting(rx) = conn {
        match rx.try_recv() {
            Err(mpsc::TryRecvError::Empty) => return true,
            Err(mpsc::TryRecvError::Disconnected) => return fail(out, "connect failed".into()),
            Ok(Err(why)) => return fail(out, why),
            Ok(Ok(stream)) => {
                out.push(Frame::Opened { id });
                *conn = Conn::Open { stream, to_server: Vec::new(), unacked: 0, closing: None };
            }
        }
    }
    let Conn::Open { stream, to_server, unacked, closing } = conn else { return true };
    // The device's bytes to the server, acknowledged once it has taken them.
    let mut taken = 0;
    while taken < to_server.len() {
        match stream.write(&to_server[taken..]) {
            Ok(0) => return fail(out, "connection closed".into()),
            Ok(n) => taken += n,
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => break,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
            Err(e) => return fail(out, why(&e)),
        }
    }
    to_server.drain(..taken);
    if let Some(by) = closing {
        return !to_server.is_empty() && Instant::now() < *by; // (the device isn't told any more)
    }
    if taken > 0 {
        out.push(Frame::Ack { id, n: taken });
    }
    // The server's bytes to the device, as many as its window takes.
    let mut buf = [0u8; DATA_MAX];
    while *unacked < WINDOW {
        let room = (WINDOW - *unacked).min(DATA_MAX);
        match stream.read(&mut buf[..room]) {
            Ok(0) => {
                out.push(Frame::Close { id, why: None });
                return false;
            }
            Ok(n) => {
                out.push(Frame::Data { id, bytes: buf[..n].to_vec() });
                *unacked += n;
            }
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => break,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
            Err(e) => return fail(out, why(&e)),
        }
    }
    true
}

/// Look the name up and connect (blocking: on a thread of its own).
fn connect(host: &str, port: u16, allow_local: bool) -> Result<TcpStream, String> {
    let unknown = || format!("cannot resolve {host}");
    let addrs: Vec<SocketAddr> = (host, port).to_socket_addrs().map_err(|_| unknown())?.collect();
    let allowed: Vec<&SocketAddr> = addrs.iter().filter(|a| allow_local || !on_this_computer(a.ip())).collect();
    if allowed.is_empty() {
        return Err(if addrs.is_empty() { unknown() } else { "not allowed: an address on this computer".into() });
    }
    let mut last = String::new();
    for addr in allowed {
        match TcpStream::connect_timeout(addr, CONNECT_TIMEOUT) {
            Ok(stream) => {
                stream.set_nonblocking(true).map_err(|e| why(&e))?;
                let _ = stream.set_nodelay(true);
                return Ok(stream);
            }
            Err(e) => last = why(&e),
        }
    }
    Err(last)
}

fn on_this_computer(ip: IpAddr) -> bool {
    let ip = ip.to_canonical();
    ip.is_loopback() || ip.is_unspecified()
}

/// An error as the device's own connections word it.
fn why(e: &io::Error) -> String {
    match e.kind() {
        io::ErrorKind::ConnectionRefused => "connection refused".into(),
        io::ErrorKind::ConnectionReset => "connection reset".into(),
        io::ErrorKind::TimedOut => "timed out".into(),
        _ => e.to_string(),
    }
}

const B64: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// Standard base64 with padding.
pub fn base64(data: &[u8]) -> String {
    let mut out = String::with_capacity(data.len().div_ceil(3) * 4);
    for chunk in data.chunks(3) {
        let v =
            (chunk[0] as u32) << 16 | (*chunk.get(1).unwrap_or(&0) as u32) << 8 | *chunk.get(2).unwrap_or(&0) as u32;
        for (i, shift) in [18, 12, 6, 0].into_iter().enumerate() {
            out.push(if i <= chunk.len() { B64[(v >> shift) as usize & 63] as char } else { '=' });
        }
    }
    out
}

/// Standard base64; padding optional. None for any other character.
pub fn unbase64(text: &str) -> Option<Vec<u8>> {
    let mut out = Vec::with_capacity(text.len() / 4 * 3);
    let (mut acc, mut bits) = (0u32, 0);
    for c in text.bytes().filter(|&c| c != b'=') {
        acc = acc << 6 | B64.iter().position(|&b| b == c)? as u32;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push((acc >> bits) as u8);
        }
    }
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn base64_both_ways() {
        for (raw, text) in [("", ""), ("f", "Zg=="), ("fo", "Zm8="), ("foo", "Zm9v"), ("foobar", "Zm9vYmFy")] {
            assert_eq!(base64(raw.as_bytes()), text);
            assert_eq!(unbase64(text).unwrap(), raw.as_bytes());
        }
        let all: Vec<u8> = (0..=255).collect();
        assert_eq!(unbase64(&base64(&all)).unwrap(), all);
        assert_eq!(unbase64("Zm9v*"), None);
    }

    #[test]
    fn frames_parse_and_print() {
        let frames = [
            ("OPEN 1 example.com 443", Frame::Open { id: 1, host: "example.com".into(), port: 443 }),
            ("OPENED 7", Frame::Opened { id: 7 }),
            ("DATA 2 aGVsbG8=", Frame::Data { id: 2, bytes: b"hello".to_vec() }),
            ("ACK 2 517", Frame::Ack { id: 2, n: 517 }),
            ("CLOSE 3", Frame::Close { id: 3, why: None }),
            ("CLOSE 3 connection refused", Frame::Close { id: 3, why: Some("connection refused".into()) }),
        ];
        for (text, frame) in frames {
            assert_eq!(Frame::parse(text).as_ref(), Some(&frame), "{text}");
            assert_eq!(frame.line(), format!("TUN {text}"));
        }
        assert_eq!(Frame::parse("open 1 a 1"), Some(Frame::Open { id: 1, host: "a".into(), port: 1 }));
        for bad in ["", "OPEN", "OPEN x a 1", "OPEN 1 a", "OPEN 1 a 70000", "DATA 1", "DATA 1 *", "ACK 1 x", "WHAT 1"] {
            assert_eq!(Frame::parse(bad), None, "{bad}");
        }
        let full = Frame::Data { id: 1, bytes: vec![7; DATA_MAX] };
        assert_eq!(Frame::parse(&full.line()[4..]), Some(full.clone()));
        assert!(full.line().len() < 2047, "fits the device's line");
        assert_eq!(Frame::parse(&format!("DATA 1 {}", base64(&[7; DATA_MAX + 1]))), None);
        let why = Frame::Close { id: 1, why: Some("a\nTUN OFF".into()) };
        assert_eq!(why.line(), "TUN CLOSE 1 a TUN OFF");
    }

    /// A device whose firmware has no tether, counting the TUN lines.
    struct Older {
        rx: Vec<u8>,
        line: Vec<u8>,
        tuns: std::sync::Arc<std::sync::atomic::AtomicUsize>,
    }

    impl Read for Older {
        fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
            let n = buf.len().min(self.rx.len());
            if n == 0 {
                return Err(io::ErrorKind::TimedOut.into());
            }
            buf[..n].copy_from_slice(&self.rx[..n]);
            self.rx.drain(..n);
            Ok(n)
        }
    }

    impl Write for Older {
        fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
            for &b in buf {
                if b != b'\n' {
                    self.line.push(b);
                    continue;
                }
                if self.line.starts_with(b"TUN") {
                    self.tuns.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
                    self.rx.extend_from_slice(b"ERR unknown command 'TUN' (try HELP)\n");
                }
                self.line.clear();
            }
            Ok(buf.len())
        }
        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }

    impl crate::Link for Older {
        fn set_wait(&mut self, _: Duration) {}
    }

    #[test]
    fn a_device_without_the_tether_is_asked_once() {
        let tuns = std::sync::Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let mut dev = Device::on(Box::new(Older { rx: Vec::new(), line: Vec::new(), tuns: tuns.clone() }));
        let mut relay = Relay::new(false);
        relay.keep_every = Duration::ZERO;
        let e = relay.pump(&mut dev, Duration::ZERO).unwrap_err();
        assert!(e.downcast_ref::<ErrReply>().is_some() && !is_link_error(&e), "{e:#}");
        assert!(relay.pump(&mut dev, Duration::ZERO).is_ok()); // (the link is fine)
        assert_eq!(tuns.load(std::sync::atomic::Ordering::SeqCst), 1);
    }
}
