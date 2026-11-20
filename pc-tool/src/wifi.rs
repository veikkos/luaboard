//! Wi-Fi: the encrypted TCP link to the device, mDNS discovery, and the WIFI
//! command.
//!
//! On Wi-Fi the device serves the same line protocol as over USB on a TCP
//! port (3333) and announces itself as `_luaboard._tcp` via mDNS. A port
//! string of the form `tcp://host:port` selects this transport. The link
//! starts with HELLO and then carries AES-256-GCM frames under keys derived
//! from the pairing key (see `keys` and docs/PROTOCOL.md, "Wi-Fi link").

use crate::keys;
use aes_gcm::aead::{Aead, KeyInit, Payload};
use aes_gcm::{Aes256Gcm, Nonce};
use anyhow::{anyhow, bail, Context, Result};
use std::io::{self, Read, Write};
use std::net::{SocketAddr, TcpStream, ToSocketAddrs};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

pub const SERVICE: &str = "_luaboard._tcp.local.";
pub const DEFAULT_PORT: u16 = 3333;

/// `tcp://host:port`, or `host:port` without any `/` -> (host, port).
pub fn parse_tcp_port(port: &str) -> Option<(String, u16)> {
    let rest = match port.strip_prefix("tcp://") {
        Some(r) => r,
        None if !port.contains('/') && port.contains(':') => port,
        None => return None,
    };
    let (host, p) = rest.rsplit_once(':').unwrap_or((rest, ""));
    let p = if p.is_empty() { DEFAULT_PORT } else { p.parse().ok()? };
    (!host.is_empty()).then(|| (host.to_string(), p))
}

/// Most plaintext in one frame (the firmware's WL_MAX_PLAIN).
const MAX_PLAIN: usize = 4096;
const TAG_LEN: usize = 16;

/// The encrypted link to a device over TCP, with the same semantics as the
/// serial links: reads time out after 20 ms with `TimedOut`, and a closed
/// connection is `BrokenPipe`.
pub struct TcpLink {
    stream: TcpStream,
    rx: Aes256Gcm,
    tx: Aes256Gcm,
    nrx: u64,
    ntx: u64,
    raw: Vec<u8>,
    plain: Vec<u8>,
    plain_off: usize,
}

/// This connection's keys (c2s, s2c) from the pairing key and both nonces.
/// Must match the firmware's auth_session_keys().
pub fn session_keys(key: &[u8], id: &[u8], cn: &[u8], sn: &[u8]) -> ([u8; 32], [u8; 32]) {
    let mut msg = b"luaboard wifi 1".to_vec();
    msg.extend_from_slice(id);
    msg.extend_from_slice(cn);
    msg.extend_from_slice(sn);
    let prk = hmac_sha256(key, &msg);
    (hmac_sha256(&prk, b"c2s"), hmac_sha256(&prk, b"s2c"))
}

fn nonce_of(count: u64) -> [u8; 12] {
    let mut n = [0u8; 12];
    n[4..].copy_from_slice(&count.to_be_bytes());
    n
}

/// Encrypt one frame (plaintext of 1..=MAX_PLAIN bytes).
pub fn seal(cipher: &Aes256Gcm, count: u64, plain: &[u8]) -> Vec<u8> {
    let len = ((plain.len() + TAG_LEN) as u16).to_be_bytes();
    let ct =
        cipher.encrypt(&Nonce::from(nonce_of(count)), Payload { msg: plain, aad: &len }).expect("AES-GCM encryption");
    let mut out = len.to_vec();
    out.extend_from_slice(&ct);
    out
}

/// Not paired with this computer, or the device isn't paired at all.
pub fn not_paired(e: &anyhow::Error) -> bool {
    let s = format!("{e:#}");
    s.contains("isn't paired") || s.contains("not paired")
}

impl TcpLink {
    /// `wait`: how long a read waits for data before it gives up (TimedOut).
    pub fn open(host: &str, port: u16, wait: Duration) -> Result<Self> {
        let addrs: Vec<SocketAddr> =
            (host, port).to_socket_addrs().with_context(|| format!("cannot resolve {host}"))?.collect();
        let mut last = None;
        for a in addrs {
            match TcpStream::connect_timeout(&a, Duration::from_secs(3)) {
                Ok(stream) => {
                    stream.set_read_timeout(Some(wait.max(Duration::from_millis(1))))?;
                    stream.set_write_timeout(Some(Duration::from_secs(5)))?;
                    stream.set_nodelay(true)?;
                    return Self::hello(stream, host);
                }
                Err(e) => last = Some(e),
            }
        }
        match last {
            Some(e) => Err(e).with_context(|| format!("cannot connect to {host}:{port}")),
            None => bail!("{host} has no addresses"),
        }
    }

    /// HELLO: swap nonces in clear text, then derive this connection's keys.
    fn hello(mut stream: TcpStream, host: &str) -> Result<Self> {
        let mut cn = [0u8; 16];
        getrandom::fill(&mut cn).map_err(|e| anyhow!("no random numbers: {e}"))?;
        stream.write_all(format!("HELLO 1 {}\n", keys::hex(&cn)).as_bytes())?;
        // The answer, one byte at a time: frames may follow right behind it.
        let mut line = Vec::new();
        let end = Instant::now() + Duration::from_secs(3);
        let mut b = [0u8; 1];
        loop {
            match stream.read(&mut b) {
                Ok(0) => bail!("{host} closed the connection"),
                Ok(_) if b[0] == b'\n' => break,
                Ok(_) if line.len() < 512 => line.push(b[0]),
                Ok(_) => bail!("{host} doesn't answer like a luaboard"),
                Err(e) if matches!(e.kind(), io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut) => {
                    if Instant::now() > end {
                        bail!("{host} doesn't answer (is it a luaboard with current firmware?)");
                    }
                }
                Err(e) => return Err(e).context("reading the device's HELLO"),
            }
        }
        let line = String::from_utf8_lossy(&line).trim_end_matches('\r').to_string();
        if let Some(err) = line.strip_prefix("ERR ") {
            bail!("device: {}", crate::printable(err));
        }
        let mut it = line.split_whitespace();
        let bad = || anyhow!("{host} doesn't answer like a luaboard: {}", crate::printable(&line));
        if (it.next(), it.next(), it.next()) != (Some("OK"), Some("HELLO"), Some("1")) {
            return Err(bad());
        }
        let id = it.next().filter(|i| i.len() == 16).ok_or_else(bad)?.to_string();
        let sn = it.next().and_then(keys::unhex).filter(|s| s.len() == 16).ok_or_else(bad)?;
        let id_bytes = keys::unhex(&id).ok_or_else(bad)?;
        let key = keys::get(&id).ok_or_else(|| {
            anyhow!(
                "this computer isn't paired with the device at {host} (key {id}): \
                 connect the device by USB and run `luaboard pair`"
            )
        })?;
        let (c2s, s2c) = session_keys(&key, &id_bytes, &cn, &sn);
        Ok(TcpLink {
            stream,
            rx: Aes256Gcm::new_from_slice(&s2c).expect("32-byte key"),
            tx: Aes256Gcm::new_from_slice(&c2s).expect("32-byte key"),
            nrx: 0,
            ntx: 0,
            raw: Vec::new(),
            plain: Vec::new(),
            plain_off: 0,
        })
    }

    /// Decrypt the next frame if all of it is here.
    fn open_frame(&mut self) -> io::Result<bool> {
        if self.raw.len() < 2 {
            return Ok(false);
        }
        let clen = u16::from_be_bytes([self.raw[0], self.raw[1]]) as usize;
        if clen <= TAG_LEN || clen > MAX_PLAIN + TAG_LEN {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "bad frame from the device"));
        }
        if self.raw.len() < 2 + clen {
            return Ok(false);
        }
        let plain = self
            .rx
            .decrypt(&Nonce::from(nonce_of(self.nrx)), Payload { msg: &self.raw[2..2 + clen], aad: &self.raw[..2] })
            .map_err(|_| {
                io::Error::new(
                    io::ErrorKind::InvalidData,
                    "the device's data didn't decrypt: it has a different key now (pair again over USB), \
                     or it isn't the real device",
                )
            })?;
        self.nrx += 1;
        self.raw.drain(..2 + clen);
        self.plain = plain;
        self.plain_off = 0;
        Ok(true)
    }
}

impl crate::Link for TcpLink {
    fn set_wait(&mut self, wait: Duration) {
        let _ = self.stream.set_read_timeout(Some(wait.max(Duration::from_millis(1))));
    }
}

impl Read for TcpLink {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        loop {
            if self.plain_off < self.plain.len() {
                let n = buf.len().min(self.plain.len() - self.plain_off);
                buf[..n].copy_from_slice(&self.plain[self.plain_off..self.plain_off + n]);
                self.plain_off += n;
                return Ok(n);
            }
            if self.open_frame()? {
                continue;
            }
            let mut tmp = [0u8; 8192];
            match self.stream.read(&mut tmp) {
                Ok(0) => return Err(io::Error::new(io::ErrorKind::BrokenPipe, "device closed the connection")),
                Ok(n) => self.raw.extend_from_slice(&tmp[..n]),
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => return Err(io::ErrorKind::TimedOut.into()),
                Err(e) => return Err(e),
            }
        }
    }
}

impl Write for TcpLink {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        let mut frames = Vec::new();
        for chunk in buf.chunks(MAX_PLAIN) {
            frames.extend_from_slice(&seal(&self.tx, self.ntx, chunk));
            self.ntx += 1;
        }
        self.stream.write_all(&frames)?;
        Ok(buf.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        self.stream.flush()
    }
}

/// A device announcing itself on the network.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Found {
    /// mDNS instance name, e.g. "luaboard".
    pub name: String,
    /// Port string to connect with, e.g. "tcp://192.168.1.23:3333".
    pub port: String,
    pub board: Option<String>,
    pub fw: Option<String>,
}

fn found_from(info: &mdns_sd::ResolvedService) -> Option<Found> {
    let mut v4: Vec<_> = info.get_addresses_v4().into_iter().collect();
    // Prefer a routable address over loopback / link-local.
    v4.sort_by_key(|a| (a.is_loopback(), a.is_link_local(), *a));
    let ip = v4.first()?;
    let name = info.get_fullname().split('.').next().unwrap_or("luaboard").to_string();
    Some(Found {
        name,
        port: format!("tcp://{ip}:{}", info.get_port()),
        board: info.get_property_val_str("board").map(str::to_string),
        fw: info.get_property_val_str("fw").map(str::to_string),
    })
}

/// Browse for devices for `timeout` (blocking). Used by the CLI.
pub fn discover(timeout: Duration) -> Vec<Found> {
    let Ok(d) = mdns_sd::ServiceDaemon::new() else { return Vec::new() };
    let Ok(rx) = d.browse(SERVICE) else { return Vec::new() };
    let end = Instant::now() + timeout;
    let mut out: Vec<Found> = Vec::new();
    while let Some(left) = end.checked_duration_since(Instant::now()) {
        if let Ok(mdns_sd::ServiceEvent::ServiceResolved(info)) = rx.recv_timeout(left) {
            if let Some(f) = found_from(&info) {
                if !out.iter().any(|o| o.port == f.port) {
                    out.push(f);
                }
            }
        }
    }
    let _ = d.shutdown();
    out
}

/// Continuous background discovery (for the GUI): the list stays current
/// as devices appear and disappear.
pub struct Discovery {
    devices: Arc<Mutex<Vec<Found>>>,
    _daemon: Option<mdns_sd::ServiceDaemon>,
}

impl Discovery {
    pub fn start() -> Self {
        let devices: Arc<Mutex<Vec<Found>>> = Arc::new(Mutex::new(Vec::new()));
        let daemon = mdns_sd::ServiceDaemon::new().ok();
        if let Some(rx) = daemon.as_ref().and_then(|d| d.browse(SERVICE).ok()) {
            let list = devices.clone();
            std::thread::spawn(move || {
                while let Ok(ev) = rx.recv() {
                    let mut l = list.lock().unwrap();
                    match ev {
                        mdns_sd::ServiceEvent::ServiceResolved(info) => {
                            if let Some(f) = found_from(&info) {
                                l.retain(|o| o.name != f.name);
                                l.push(f);
                            }
                        }
                        mdns_sd::ServiceEvent::ServiceRemoved(_, full) => {
                            let name = full.split('.').next().unwrap_or_default().to_string();
                            l.retain(|o| o.name != name);
                        }
                        _ => {}
                    }
                }
            });
        }
        Discovery { devices, _daemon: daemon }
    }

    pub fn devices(&self) -> Vec<Found> {
        self.devices.lock().unwrap().clone()
    }
}

/// Parsed reply of the WIFI command.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Status {
    pub state: String,
    pub ssid: Option<String>,
    pub ip: Option<String>,
    pub rssi: i32,
    pub port: u16,
    pub client: bool,
}

/// `WIFI SET` command line for the given credentials (hex keeps spaces safe).
pub fn set_command(ssid: &str, password: &str) -> Result<String> {
    if ssid.is_empty() || ssid.len() > 32 {
        bail!("network name must be 1..32 bytes");
    }
    if password.len() > 64 {
        bail!("password must be at most 64 bytes");
    }
    Ok(format!("WIFI SET {} {}", keys::hex(ssid.as_bytes()), keys::hex(password.as_bytes())).trim_end().to_string())
}

pub fn parse_status(s: &str) -> Status {
    let mut st = Status::default();
    for kv in s.split_whitespace() {
        let (k, v) = kv.split_once('=').unwrap_or((kv, ""));
        match k {
            "state" => st.state = v.to_string(),
            "ssid" if v != "-" => st.ssid = keys::unhex(v).map(|b| String::from_utf8_lossy(&b).into_owned()),
            "ip" if v != "-" => st.ip = Some(v.to_string()),
            "rssi" => st.rssi = v.parse().unwrap_or(0),
            "port" => st.port = v.parse().unwrap_or(DEFAULT_PORT),
            "client" => st.client = v == "1",
            _ => {}
        }
    }
    st
}

fn hmac_sha256(key: &[u8], msg: &[u8]) -> [u8; 32] {
    use sha2::{Digest, Sha256};
    let mut k = [0u8; 64];
    if key.len() > 64 {
        k[..32].copy_from_slice(&Sha256::digest(key));
    } else {
        k[..key.len()].copy_from_slice(key);
    }
    let ipad: Vec<u8> = k.iter().map(|b| b ^ 0x36).collect();
    let opad: Vec<u8> = k.iter().map(|b| b ^ 0x5c).collect();
    let inner = Sha256::new().chain_update(&ipad).chain_update(msg).finalize();
    Sha256::new().chain_update(&opad).chain_update(inner).finalize().into()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn frames_round_trip_and_fail_when_tampered() {
        let (c2s, _) = session_keys(&[7u8; 32], &[1u8; 8], &[2u8; 16], &[3u8; 16]);
        let k = Aes256Gcm::new_from_slice(&c2s).unwrap();
        let f = seal(&k, 5, b"PING\n");
        assert_eq!(f.len(), 2 + 5 + TAG_LEN);
        let open = |f: &[u8], n| k.decrypt(&Nonce::from(nonce_of(n)), Payload { msg: &f[2..], aad: &f[..2] });
        assert_eq!(open(&f, 5).unwrap(), b"PING\n");
        assert!(open(&f, 6).is_err()); // replayed or reordered
        let mut t = f.clone();
        t[3] ^= 1;
        assert!(open(&t, 5).is_err());
    }

    #[test]
    fn session_keys_depend_on_everything() {
        let base = session_keys(&[7u8; 32], &[1u8; 8], &[2u8; 16], &[3u8; 16]);
        assert_ne!(base.0, base.1);
        assert_ne!(base, session_keys(&[8u8; 32], &[1u8; 8], &[2u8; 16], &[3u8; 16]));
        assert_ne!(base, session_keys(&[7u8; 32], &[1u8; 8], &[2u8; 16], &[4u8; 16]));
    }

    #[test]
    fn port_strings() {
        assert_eq!(parse_tcp_port("tcp://192.168.1.5:3333"), Some(("192.168.1.5".into(), 3333)));
        assert_eq!(parse_tcp_port("tcp://luaboard.local"), Some(("luaboard.local".into(), 3333)));
        assert_eq!(parse_tcp_port("luaboard.local:4000"), Some(("luaboard.local".into(), 4000)));
        assert_eq!(parse_tcp_port("/dev/cu.usbmodem1101"), None);
        assert_eq!(parse_tcp_port("/tmp/esp32-sim"), None);
    }

    #[test]
    fn wifi_command_and_status() {
        assert_eq!(set_command("Home Net", "p w").unwrap(), "WIFI SET 486f6d65204e6574 702077");
        assert_eq!(set_command("open", "").unwrap(), "WIFI SET 6f70656e");
        assert!(set_command("", "x").is_err());
        let st = parse_status(
            "state=connected ssid=486f6d65204e6574 ip=10.0.0.7 rssi=-52 port=3333 host=luaboard.local client=1",
        );
        assert_eq!(st.state, "connected");
        assert_eq!(st.ssid.as_deref(), Some("Home Net"));
        assert_eq!(st.ip.as_deref(), Some("10.0.0.7"));
        assert_eq!((st.rssi, st.port, st.client), (-52, 3333, true));
        assert_eq!(parse_status("state=off ssid=- ip=- rssi=0 port=3333").ssid, None);
        assert_eq!(parse_status("state=off ssid=äa ip=- rssi=0").ssid, None, "not hex: no panic");
        assert_eq!(parse_status("state=off ssid=+f ip=- rssi=0").ssid, None, "a sign isn't a hex digit");
    }
}
