//! The protocol client: `Device` sends commands over a `Link` and reads the
//! replies and the device's own messages (`Msg`).

use anyhow::{anyhow, bail, Context, Result};
use std::io::Read;
use std::time::{Duration, Instant};

use crate::{
    crash, crc32, is_asset, kept_secrets_note, keys, login, open_link_poll, tether, wifi, write_all_reading, Link,
    LINK_WAIT,
};

/// Bytes per write of an upload: one full frame on the Wi-Fi link.
const WRITE_CHUNK: usize = 4096;

/// A snapshot of the device's display framebuffer (from the `FB` command).
#[derive(Debug, Clone)]
pub struct Frame {
    pub width: usize,
    pub height: usize,
    /// RGB565 pixels, row-major.
    pub pixels: Vec<u16>,
    /// CRC-32 of the pixel bytes, as the device sent it (for `FB <crc>`).
    pub crc: u32,
}

impl Frame {
    /// Pixel as 8-bit (r, g, b).
    pub fn rgb(&self, x: usize, y: usize) -> (u8, u8, u8) {
        let v = self.pixels[y * self.width + x];
        let (r, g, b) = ((v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F);
        (((r * 527 + 23) >> 6) as u8, ((g * 259 + 33) >> 6) as u8, ((b * 527 + 23) >> 6) as u8)
    }
}

/// A device line made safe to print: control characters (escape sequences
/// that could rewrite the terminal, set its clipboard, fake output) become
/// U+FFFD. Tabs stay.
pub fn printable(s: &str) -> String {
    s.chars().map(|c| if c.is_control() && c != '\t' { char::REPLACEMENT_CHARACTER } else { c }).collect()
}

/// One line received from the device.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Msg {
    Ok(String),
    Err(String),
    Log(String),
    Evt(String),
    File(String),
    /// A `SECRET <name> <bytes>` line of a SECRETS listing.
    Secret(String),
    /// An `APP <name> <files> <bytes> <state>` line of an APPS listing.
    App(String),
    /// A `FRAME <call>` line of a CRASH reply.
    Frame(String),
    /// A `TUN <frame>` line: a connection of the USB tether (`tether::Frame`).
    Tun(String),
    /// Anything else (e.g. ROM bootloader chatter on real hardware).
    Other(String),
}

impl Msg {
    pub fn parse(line: &str) -> Msg {
        let (head, rest) = match line.split_once(' ') {
            Some((h, r)) => (h, r.to_string()),
            None => (line, String::new()),
        };
        match head {
            "OK" => Msg::Ok(rest),
            "ERR" => Msg::Err(rest),
            "LOG" => Msg::Log(rest),
            "EVT" => Msg::Evt(rest),
            "FILE" => Msg::File(rest),
            "SECRET" => Msg::Secret(rest),
            "APP" => Msg::App(rest),
            "FRAME" => Msg::Frame(rest),
            "TUN" => Msg::Tun(rest),
            _ => Msg::Other(line.to_string()),
        }
    }

    /// True for messages the device sends on its own (not part of a reply).
    pub fn is_async(&self) -> bool {
        matches!(self, Msg::Log(_) | Msg::Evt(_) | Msg::Tun(_) | Msg::Other(_))
    }
}

/// A field of a `k=v k=v` reply (INFO, TIME).
pub fn kv_field<'a>(reply: &'a str, key: &str) -> Option<&'a str> {
    reply.split_whitespace().find_map(|kv| kv.strip_prefix(key).and_then(|v| v.strip_prefix('=')))
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FileInfo {
    /// `<app>/<file>`
    pub name: String,
    pub size: usize,
    pub crc: u32,
}

/// Where an app on the device lives.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AppState {
    /// In flash: survives a restart, shows in the menu.
    Installed,
    /// Uploaded to RAM only: gone after a restart.
    Ram,
    /// Installed, with newer uploads in RAM.
    Modified,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AppInfo {
    pub name: String,
    pub files: usize,
    pub bytes: usize,
    pub state: AppState,
}

/// A `SECRET` line of the `SECRETS` listing: "<app> <name> <bytes>".
pub fn parse_secret_line(line: &str) -> Option<(String, String, usize)> {
    let mut w = line.split_whitespace();
    let (app, name, size) = (w.next()?, w.next()?, w.next()?.parse().ok()?);
    w.next().is_none().then(|| (app.to_string(), name.to_string(), size))
}

/// The reply to `PAIR`: `paired=on id=<id>` or `paired=off`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PairState {
    /// The device has a pairing key (without one the Wi-Fi link takes no commands).
    pub on: bool,
    /// The key's id.
    pub id: Option<String>,
    /// This computer has that key.
    pub here: bool,
}

impl PairState {
    pub fn parse(reply: &str) -> PairState {
        let mut st = PairState { on: false, id: None, here: false };
        for kv in reply.split_whitespace() {
            match kv.split_once('=') {
                Some(("paired", v)) => st.on = v == "on",
                Some(("id", v)) if !v.is_empty() => st.id = Some(v.to_string()),
                _ => {}
            }
        }
        st.here = st.id.as_deref().is_some_and(|id| keys::get(id).is_some());
        st
    }
}

/// True if an error means the serial link itself is gone (device unplugged,
/// simulator closed), as opposed to the device answering `ERR` or being slow.
pub fn is_link_error(e: &anyhow::Error) -> bool {
    e.chain().any(|c| c.downcast_ref::<std::io::Error>().is_some())
}

/// How long a command that runs Lua (EVAL, RUN, STOP) may take: the
/// device stops any call at 10 s (its hard watchdog), plus a margin.
pub const LUA_CALL_TIMEOUT: Duration = Duration::from_secs(12);

/// How long a command that writes or deletes many files may take (INSTALL,
/// SAVE, UNINSTALL): flash writes run at tens of KB/s, and each may also
/// stop or restart the app on the screen.
pub const FLASH_TIMEOUT: Duration = Duration::from_secs(60);

/// How long FORMAT may take: it erases the whole apps partition, which the
/// device allows 5 minutes (WATCHDOG_FORMAT_MS), plus a margin.
pub const FORMAT_TIMEOUT: Duration = Duration::from_secs(5 * 60 + 15);

/// The device's ERR reply to a command (as text: "device: <why>").
#[derive(Debug)]
pub struct ErrReply(pub String);

impl std::fmt::Display for ErrReply {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "device: {}", self.0)
    }
}

impl std::error::Error for ErrReply {}

pub struct Device {
    port: Box<dyn Link>,
    rx: Vec<u8>,
    pub timeout: Duration,
    serial: u64,
}

impl Device {
    pub fn open(path: &str, baud: u32) -> Result<Self> {
        let mut dev = Device::on(open_link_poll(path, baud, LINK_WAIT)?);
        dev.sync()?;
        Ok(dev)
    }

    pub(crate) fn on(port: Box<dyn Link>) -> Device {
        static OPENED: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let serial = OPENED.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        Device { port, rx: Vec::new(), timeout: Duration::from_secs(3), serial }
    }

    /// Which open this is: a reconnect, to the same port too, is another.
    pub fn serial(&self) -> u64 {
        self.serial
    }

    /// Get the line protocol into a known state: terminate any partial line,
    /// then PING until the device answers. Everything that arrives before the
    /// PONG (stale output, an error for the partial line) is dropped, as the
    /// link keeps order, so there is no need to wait for the line to go quiet.
    /// But for the device's goodbye (`EVT bye <why>`): when it then hangs up,
    /// that is why, and the error says so.
    fn sync(&mut self) -> Result<()> {
        self.write_all(b"\n")?;
        self.rx.clear();
        let mut bye: Option<String> = None;
        for _ in 0..3 {
            self.send_line("PING")?;
            let deadline = Instant::now() + Duration::from_millis(800);
            while Instant::now() < deadline {
                let line = match self.read_line(Duration::from_millis(50)) {
                    Ok(line) => line,
                    Err(e) => {
                        return Err(match bye {
                            Some(why) => e.context(format!("the device turned this connection away: {why}")),
                            None => e,
                        })
                    }
                };
                match line.as_deref().map(Msg::parse) {
                    Some(Msg::Ok(s)) if s == "PONG" => return Ok(()),
                    Some(Msg::Evt(s)) => {
                        if let Some(why) = s.strip_prefix("bye ") {
                            bye = Some(why.to_string());
                        }
                    }
                    _ => {}
                }
            }
        }
        bail!("no answer to PING - is the firmware running and the baud rate right?")
    }

    /// Read whatever comes within `wait` (at least a millisecond's).
    fn fill(&mut self, wait: Duration) -> Result<usize> {
        let mut tmp = [0u8; 1024];
        self.port.set_wait(wait.max(Duration::from_millis(1)));
        match self.port.read(&mut tmp) {
            Ok(n) => {
                self.rx.extend_from_slice(&tmp[..n]);
                Ok(n)
            }
            Err(e) if e.kind() == std::io::ErrorKind::TimedOut => Ok(0),
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => Ok(0),
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => Ok(0),
            Err(e) => Err(e).context("reading from the device failed"),
        }
    }

    fn send_line(&mut self, line: &str) -> Result<()> {
        if line.contains('\n') {
            bail!("command must be a single line");
        }
        // One write: over Wi-Fi each write is a frame of its own.
        self.write_all(format!("{line}\n").as_bytes())
    }

    /// Write everything, keeping what comes meanwhile (`write_all_reading`).
    fn write_all(&mut self, data: &[u8]) -> Result<()> {
        self.port.set_wait(LINK_WAIT);
        let rx = &mut self.rx;
        write_all_reading(&mut *self.port, data, self.timeout, &mut |b| rx.extend_from_slice(b))
            .context("writing to the device failed")
    }

    /// Next complete line, or None if none arrived within `timeout`.
    pub fn read_line(&mut self, timeout: Duration) -> Result<Option<String>> {
        let end = Instant::now() + timeout;
        loop {
            if let Some(pos) = self.rx.iter().position(|&b| b == b'\n') {
                let raw: Vec<u8> = self.rx.drain(..=pos).collect();
                let s = String::from_utf8_lossy(&raw[..raw.len() - 1]);
                return Ok(Some(printable(s.trim_end_matches('\r'))));
            }
            let left = end.saturating_duration_since(Instant::now());
            if left.is_zero() {
                return Ok(None);
            }
            self.fill(left)?;
        }
    }

    fn read_bytes(&mut self, n: usize) -> Result<Vec<u8>> {
        let end = Instant::now() + self.timeout;
        while self.rx.len() < n {
            let left = end.saturating_duration_since(Instant::now());
            if left.is_zero() {
                bail!("timeout reading {n} bytes (got {})", self.rx.len());
            }
            self.fill(left)?;
        }
        Ok(self.rx.drain(..n).collect())
    }

    /// Collect asynchronous messages for up to `timeout`.
    pub fn poll(&mut self, timeout: Duration) -> Result<Vec<Msg>> {
        let mut out = Vec::new();
        let end = Instant::now() + timeout;
        loop {
            let line = match self.read_line(end.saturating_duration_since(Instant::now())) {
                Ok(Some(line)) => line,
                Ok(None) => break,
                // The link went away: hand over what came before it (e.g. the
                // device's "EVT bye ..." explaining why); the next poll reports the error.
                Err(_) if !out.is_empty() => break,
                Err(e) => return Err(e),
            };
            if !line.is_empty() {
                out.push(Msg::parse(&line));
            }
            if Instant::now() >= end {
                break;
            }
        }
        Ok(out)
    }

    /// Send a command and wait for its final OK/ERR.
    /// FILE lines are returned; async lines are passed to `on_async`.
    pub fn command(&mut self, line: &str, on_async: &mut dyn FnMut(Msg)) -> Result<(String, Vec<String>)> {
        self.command_within(line, self.timeout, on_async)
    }

    /// `command` with its own reply timeout.
    pub fn command_within(
        &mut self,
        line: &str,
        timeout: Duration,
        on_async: &mut dyn FnMut(Msg),
    ) -> Result<(String, Vec<String>)> {
        self.send_line(line)?;
        self.wait_reply(line, timeout, on_async)
    }

    /// After a timeout the late reply is still on its way: PING and drop
    /// everything up to the PONG, so the next command doesn't read it as its
    /// own. A device that doesn't answer even then is a dead link (an I/O
    /// error, so the studio reconnects).
    fn resync(&mut self, on_async: &mut dyn FnMut(Msg)) -> Result<()> {
        self.send_line("PING")?;
        let end = Instant::now() + LUA_CALL_TIMEOUT;
        while Instant::now() < end {
            let Some(line) = self.read_line(Duration::from_millis(100))? else { continue };
            match Msg::parse(&line) {
                Msg::Ok(s) if s == "PONG" => return Ok(()),
                m if m.is_async() => on_async(m),
                _ => {}
            }
        }
        Err(std::io::Error::new(std::io::ErrorKind::TimedOut, "the device stopped answering").into())
    }

    fn wait_reply(
        &mut self,
        what: &str,
        timeout: Duration,
        on_async: &mut dyn FnMut(Msg),
    ) -> Result<(String, Vec<String>)> {
        let mut data = Vec::new();
        let end = Instant::now() + timeout;
        loop {
            let left = end.saturating_duration_since(Instant::now());
            if left.is_zero() {
                self.resync(on_async)?;
                bail!("timeout waiting for reply to '{what}'");
            }
            let Some(line) = self.read_line(left)? else { continue };
            match Msg::parse(&line) {
                Msg::Ok(s) => return Ok((s, data)),
                Msg::Err(s) => return Err(ErrReply(s).into()),
                Msg::File(s) | Msg::Secret(s) | Msg::App(s) | Msg::Frame(s) => data.push(s),
                m => on_async(m),
            }
        }
    }

    pub fn ping(&mut self, a: &mut dyn FnMut(Msg)) -> Result<()> {
        self.command("PING", a).map(|_| ())
    }

    pub fn info(&mut self, a: &mut dyn FnMut(Msg)) -> Result<String> {
        self.command("INFO", a).map(|r| r.0)
    }

    /// TIME: "synced=1 utc=... local=... tz=..." as key/value pairs.
    pub fn time(&mut self, a: &mut dyn FnMut(Msg)) -> Result<String> {
        Ok(self.command("TIME", a)?.0)
    }

    /// Ask the device to get the time from its time server now (TIME SYNC).
    pub fn time_sync(&mut self, a: &mut dyn FnMut(Msg)) -> Result<()> {
        self.command("TIME SYNC", a).map(|_| ())
    }

    /// Get (None) or set the device's time zone, a POSIX TZ string. Returns it.
    pub fn timezone(&mut self, tz: Option<&str>, a: &mut dyn FnMut(Msg)) -> Result<String> {
        let cmd = match tz {
            Some(t) => format!("TZ {t}"),
            None => "TZ".to_string(),
        };
        let reply = self.command(&cmd, a)?.0;
        Ok(reply.strip_prefix("tz=").unwrap_or(&reply).to_string())
    }

    /// The record of the last crash, if the last reset was one (`CRASH`).
    pub fn crash(&mut self, a: &mut dyn FnMut(Msg)) -> Result<Option<crash::Crash>> {
        let (reply, lines) = self.command("CRASH", a)?;
        Ok(crash::parse(&reply, &lines))
    }

    pub fn list(&mut self, a: &mut dyn FnMut(Msg)) -> Result<Vec<FileInfo>> {
        let (_, lines) = self.command("LS", a)?;
        lines
            .iter()
            .map(|l| {
                let mut it = l.split_whitespace();
                let name = it.next().ok_or_else(|| anyhow!("bad FILE line: {l}"))?;
                let size = it.next().and_then(|s| s.parse().ok()).ok_or_else(|| anyhow!("bad FILE line: {l}"))?;
                let crc = it.next().and_then(|s| u32::from_str_radix(s, 16).ok()).unwrap_or(0);
                Ok(FileInfo { name: name.to_string(), size, crc })
            })
            .collect()
    }

    /// Upload a file: `PUT name size crc` -> `OK READY` -> raw bytes -> `OK STORED`.
    pub fn put(&mut self, name: &str, data: &[u8], a: &mut dyn FnMut(Msg)) -> Result<()> {
        let cmd = format!("PUT {} {} {:08x}", name, data.len(), crc32(data));
        let (ready, _) = self.command(&cmd, a)?;
        if ready != "READY" {
            bail!("unexpected reply to PUT: {ready}");
        }
        self.send_payload(data, &mut |_, _| {})?;
        self.wait_reply("PUT data", self.timeout, a).map(|_| ())
    }

    /// The raw bytes after an `OK READY`; `progress(done, total)` follows them.
    fn send_payload(&mut self, data: &[u8], progress: &mut dyn FnMut(usize, usize)) -> Result<()> {
        let mut done = 0;
        for chunk in data.chunks(WRITE_CHUNK) {
            self.write_all(chunk)?;
            done += chunk.len();
            progress(done, data.len());
        }
        Ok(())
    }

    /// The `size` raw bytes after a reply line, checked against `crc`. On a
    /// broken link what came is dropped and the link brought back in step.
    fn read_payload(&mut self, size: usize, crc: u32, what: &str, a: &mut dyn FnMut(Msg)) -> Result<Vec<u8>> {
        let data = match self.read_bytes(size) {
            Ok(d) => d,
            Err(e) => {
                self.rx.clear(); // part of the data: drop it, then whatever still comes
                self.resync(a)?;
                return Err(e);
            }
        };
        if crc32(&data) != crc {
            bail!("CRC mismatch in {what}");
        }
        Ok(data)
    }

    /// Firmware update: `OTA size sha256` -> `OK READY` -> the image ->
    /// `OK UPDATED <version>`; the device then reboots (see [`ota::update`]).
    /// `progress(done, total)` follows the bytes sent.
    pub fn ota(
        &mut self,
        data: &[u8],
        progress: &mut dyn FnMut(usize, usize),
        a: &mut dyn FnMut(Msg),
    ) -> Result<String> {
        use sha2::{Digest, Sha256};
        let sha = keys::hex(&Sha256::digest(data));
        let (ready, _) = self.command(&format!("OTA {} {sha}", data.len()), a)?;
        if ready != "READY" {
            bail!("unexpected reply to OTA: {ready}");
        }
        self.send_payload(data, progress)?;
        // The device checks the whole image (hash, image format) before it answers.
        let (reply, _) = self.wait_reply("OTA data", Duration::from_secs(60), a)?;
        Ok(reply.strip_prefix("UPDATED").unwrap_or(&reply).trim().to_string())
    }

    /// Commands answered with `OK <size> <crc>` followed by `size` raw bytes.
    fn read_blob(&mut self, cmd: &str, a: &mut dyn FnMut(Msg)) -> Result<Vec<u8>> {
        let (hdr, _) = self.command(cmd, a)?;
        let mut it = hdr.split_whitespace();
        let size: usize = it.next().and_then(|s| s.parse().ok()).ok_or_else(|| anyhow!("bad reply to {cmd}: {hdr}"))?;
        let crc = it.next().and_then(|s| u32::from_str_radix(s, 16).ok()).unwrap_or(0);
        self.read_payload(size, crc, &format!("reply to {cmd}"), a)
    }

    pub fn get(&mut self, name: &str, a: &mut dyn FnMut(Msg)) -> Result<Vec<u8>> {
        self.read_blob(&format!("GET {name}"), a)
    }

    /// Display framebuffer: `OK <w> <h> rgb565 <bytes> <crc>` + raw little-endian pixels.
    pub fn framebuffer(&mut self, a: &mut dyn FnMut(Msg)) -> Result<Frame> {
        self.framebuffer_if_changed(None, a)?.ok_or_else(|| anyhow!("FB: no frame"))
    }

    /// The framebuffer, or None if it still has the CRC of `last` (`FB <crc>`:
    /// `OK SAME`, no pixels sent).
    pub fn framebuffer_if_changed(&mut self, last: Option<u32>, a: &mut dyn FnMut(Msg)) -> Result<Option<Frame>> {
        let cmd = match last {
            Some(crc) => format!("FB {crc:08x}"),
            None => "FB".to_string(),
        };
        let (hdr, _) = self.command(&cmd, a)?;
        if hdr.trim() == "SAME" && last.is_some() {
            return Ok(None);
        }
        let f: Vec<&str> = hdr.split_whitespace().collect();
        let (width, height): (usize, usize) =
            match (f.first().and_then(|s| s.parse().ok()), f.get(1).and_then(|s| s.parse().ok())) {
                (Some(w), Some(h)) if f.get(2) == Some(&"rgb565") => (w, h),
                _ => bail!("bad FB reply: {hdr}"),
            };
        let size: usize = f.get(3).and_then(|s| s.parse().ok()).ok_or_else(|| anyhow!("bad FB reply: {hdr}"))?;
        let crc = f.get(4).and_then(|s| u32::from_str_radix(s, 16).ok()).unwrap_or(0);
        if width.checked_mul(height).and_then(|n| n.checked_mul(2)) != Some(size) {
            bail!("FB size mismatch: {size} bytes for {width}x{height}");
        }
        let data = self.read_payload(size, crc, "framebuffer", a)?;
        let pixels = data.as_chunks::<2>().0.iter().map(|c| u16::from_le_bytes([c[0], c[1]])).collect();
        Ok(Some(Frame { width, height, pixels, crc }))
    }

    /// A finger from this computer: down (or moved) at `at` in the touch area
    /// (x 0..319, y 0..279; y >= 240 is the A/B/C strip), or let go (None).
    /// Repeat it while held: the device lets go after 1 s without one.
    pub fn touch(&mut self, at: Option<(u32, u32)>, a: &mut dyn FnMut(Msg)) -> Result<()> {
        let cmd = match at {
            Some((x, y)) => format!("TOUCH {x} {y}"),
            None => "TOUCH UP".to_string(),
        };
        self.command(&cmd, a).map(|_| ())
    }

    /// A short press of the power key (home).
    pub fn key_power(&mut self, a: &mut dyn FnMut(Msg)) -> Result<()> {
        self.command("KEY POWER", a).map(|_| ())
    }

    pub fn remove(&mut self, name: &str, a: &mut dyn FnMut(Msg)) -> Result<()> {
        self.command(&format!("RM {name}"), a).map(|_| ())
    }

    pub fn run(&mut self, name: Option<&str>, a: &mut dyn FnMut(Msg)) -> Result<String> {
        let cmd = match name {
            Some(n) => format!("RUN {n}"),
            None => "RUN".to_string(),
        };
        self.command_within(&cmd, LUA_CALL_TIMEOUT, a).map(|r| r.0)
    }

    pub fn stop(&mut self, a: &mut dyn FnMut(Msg)) -> Result<()> {
        self.command_within("STOP", LUA_CALL_TIMEOUT, a).map(|_| ())
    }

    pub fn save(&mut self, a: &mut dyn FnMut(Msg)) -> Result<String> {
        self.command_within("SAVE", FLASH_TIMEOUT, a).map(|r| r.0)
    }

    pub fn format(&mut self, a: &mut dyn FnMut(Msg)) -> Result<()> {
        self.command_within("FORMAT", FORMAT_TIMEOUT, a).map(|_| ())
    }

    /// The app started at boot instead of the menu: `Some("off")` or
    /// `Some(app)` sets it. Returns `AUTORUN <app>|off`.
    pub fn autorun(&mut self, app: Option<&str>, a: &mut dyn FnMut(Msg)) -> Result<String> {
        let cmd = match app {
            Some(v) => format!("AUTORUN {v}"),
            None => "AUTORUN".to_string(),
        };
        self.command(&cmd, a).map(|r| r.0)
    }

    /// Keep the device awake (screen on, not turning itself off) for a
    /// time: `Some(minutes)` or `Some("off")` sets it. Returns `AWAKE
    /// <minutes left>|off`.
    pub fn awake(&mut self, minutes: Option<&str>, a: &mut dyn FnMut(Msg)) -> Result<String> {
        let cmd = match minutes {
            Some(v) => format!("AWAKE {v}"),
            None => "AWAKE".to_string(),
        };
        self.command(&cmd, a).map(|r| r.0)
    }

    pub fn apps(&mut self, a: &mut dyn FnMut(Msg)) -> Result<Vec<AppInfo>> {
        let (_, lines) = self.command("APPS", a)?;
        lines
            .iter()
            .map(|l| {
                let f: Vec<&str> = l.split_whitespace().collect();
                let bad = || anyhow!("bad APP line: {l}");
                Ok(AppInfo {
                    name: f.first().ok_or_else(bad)?.to_string(),
                    files: f.get(1).and_then(|s| s.parse().ok()).ok_or_else(bad)?,
                    bytes: f.get(2).and_then(|s| s.parse().ok()).ok_or_else(bad)?,
                    state: match f.get(3).copied() {
                        Some("installed") => AppState::Installed,
                        Some("ram") => AppState::Ram,
                        Some("modified") => AppState::Modified,
                        _ => return Err(bad()),
                    },
                })
            })
            .collect()
    }

    /// Write an app's uploaded files to flash.
    pub fn install(&mut self, app: &str, a: &mut dyn FnMut(Msg)) -> Result<String> {
        self.command_within(&format!("INSTALL {app}"), FLASH_TIMEOUT, a).map(|r| r.0)
    }

    pub fn uninstall(&mut self, app: &str, a: &mut dyn FnMut(Msg)) -> Result<String> {
        self.command_within(&format!("UNINSTALL {app}"), FLASH_TIMEOUT, a).map(|r| r.0)
    }

    /// Upload `files` (name, contents) of the app `app`. Assets the device
    /// already has as they are (the same size and CRC) aren't sent again, so
    /// a save in the studio doesn't send the pictures and sounds each time.
    /// Returns the names sent.
    pub fn put_project(
        &mut self,
        app: &str,
        files: &[(String, Vec<u8>)],
        a: &mut dyn FnMut(Msg),
    ) -> Result<Vec<String>> {
        let have = if files.iter().any(|(n, _)| is_asset(n)) { self.list(a)? } else { vec![] };
        let mut sent = vec![];
        for (name, data) in files {
            let path = format!("{app}/{name}");
            let same =
                is_asset(name) && have.iter().any(|f| f.name == path && f.size == data.len() && f.crc == crc32(data));
            if !same {
                self.put(&path, data, a)?;
                sent.push(name.clone());
            }
        }
        Ok(sent)
    }

    /// Upload `files` (name, contents) as the app `app` and install it:
    /// device files of that app that aren't in `files` are removed, so the
    /// device ends up with exactly this project.
    pub fn install_project(
        &mut self,
        app: &str,
        files: &[(String, Vec<u8>)],
        a: &mut dyn FnMut(Msg),
    ) -> Result<String> {
        self.put_project(app, files, a)?;
        let prefix = format!("{app}/");
        for f in self.list(a)? {
            if let Some(rest) = f.name.strip_prefix(&prefix) {
                if !files.iter().any(|(n, _)| n == rest) {
                    self.remove(&f.name, a)?;
                }
            }
        }
        self.install(app, a)
    }

    /// Wi-Fi pairing status (and whether this computer has the key).
    pub fn pair_status(&mut self, a: &mut dyn FnMut(Msg)) -> Result<PairState> {
        Ok(PairState::parse(&self.command("PAIR", a)?.0))
    }

    /// Pair this computer for Wi-Fi (over USB): take the device's key, or
    /// make one if it has none (or `new`: a new one, which unpairs every other
    /// computer). The key is saved in [`keys::path`]. Returns the key id.
    pub fn pair(&mut self, new: bool, a: &mut dyn FnMut(Msg)) -> Result<String> {
        let cmd = if new || !self.pair_status(a)?.on { "PAIR NEW" } else { "PAIR KEY" };
        let (reply, _) = self.command(cmd, a)?;
        let mut it = reply.split_whitespace();
        let (Some("PAIR"), Some(id), Some(key)) = (it.next(), it.next(), it.next()) else {
            bail!("unexpected PAIR reply");
        };
        let key: [u8; keys::KEY_LEN] =
            keys::unhex(key).and_then(|k| k.try_into().ok()).ok_or_else(|| anyhow!("bad key from the device"))?;
        keys::put(id, &key)?;
        Ok(id.to_string())
    }

    /// Unpair (over USB): the device forgets its key, so the Wi-Fi link takes
    /// no commands; this computer forgets it too.
    pub fn unpair(&mut self, a: &mut dyn FnMut(Msg)) -> Result<()> {
        if let Some(id) = self.pair_status(a)?.id {
            keys::remove(&id)?;
        }
        self.command("PAIR OFF", a).map(|_| ())
    }

    /// The pending browser-login request of the running script, if any.
    pub fn browser_pending(&mut self, a: &mut dyn FnMut(Msg)) -> Result<Option<login::Request>> {
        let (r, _) = self.command("BROWSER", a)?;
        let r = r.strip_prefix("BROWSER ").unwrap_or(&r);
        Ok(if r == "none" { None } else { login::Request::parse(r) })
    }

    /// Answer a browser-login request with the redirect's query string.
    pub fn browser_answer(&mut self, id: u32, query: &str, a: &mut dyn FnMut(Msg)) -> Result<()> {
        let q = if query.is_empty() { "?" } else { query };
        if q.contains(char::is_whitespace) {
            bail!("the login answer contains spaces");
        }
        self.command(&format!("BROWSER {id} {q}"), a).map(|_| ())
    }

    /// Decline a browser-login request.
    pub fn browser_cancel(&mut self, id: u32, a: &mut dyn FnMut(Msg)) -> Result<()> {
        self.command(&format!("BROWSER {id} CANCEL"), a).map(|_| ())
    }

    /// The secrets stored on the device: (app, name, size). Values never leave it.
    pub fn secrets(&mut self, a: &mut dyn FnMut(Msg)) -> Result<Vec<(String, String, usize)>> {
        let (_, lines) = self.command("SECRETS", a)?;
        Ok(lines.iter().filter_map(|l| parse_secret_line(l)).collect())
    }

    /// Secrets the app `app` already has on the device, other than `from_project`
    /// (those a project's secrets file sets anyway): tokens the app stored itself,
    /// e.g. a login. Installing code into that app hands them to the new code.
    pub fn kept_secrets(&mut self, app: &str, from_project: &[String], a: &mut dyn FnMut(Msg)) -> Result<Vec<String>> {
        Ok(self
            .secrets(a)?
            .into_iter()
            .filter(|(owner, name, _)| owner == app && !from_project.contains(name))
            .map(|(_, name, _)| name)
            .collect())
    }

    /// Store a secret of the app `app` (only that app's scripts can read it).
    pub fn secret_set(&mut self, app: &str, name: &str, value: &[u8], a: &mut dyn FnMut(Msg)) -> Result<()> {
        // A device line holds 2047 characters, a 2 KB value takes 4 KB of hex:
        // all but the last piece go as SECRET PART.
        const PIECE: usize = 768;
        let pieces: Vec<&[u8]> = if value.is_empty() { vec![value] } else { value.chunks(PIECE).collect() };
        for p in &pieces[..pieces.len() - 1] {
            self.command(&format!("SECRET PART {app} {name} {}", keys::hex(p)), a)?;
        }
        self.command(&format!("SECRET SET {app} {name} {}", keys::hex(pieces[pieces.len() - 1])), a).map(|_| ())
    }

    /// Before a project's code goes into the app `app`: store its secrets
    /// (its secrets file's `secrets`), and say what else the app keeps on the
    /// device, which the new code can read (kept_secrets_note; None: nothing).
    pub fn push_secrets(
        &mut self,
        app: &str,
        secrets: &[(String, String)],
        a: &mut dyn FnMut(Msg),
    ) -> Result<Option<String>> {
        let own: Vec<String> = secrets.iter().map(|(n, _)| n.clone()).collect();
        let note = kept_secrets_note(app, &self.kept_secrets(app, &own, a)?);
        self.set_secrets(app, secrets, a)?;
        Ok(note)
    }

    /// Store `secrets` (name, value) for the app `app`, e.g. a project's secrets file.
    pub fn set_secrets(&mut self, app: &str, secrets: &[(String, String)], a: &mut dyn FnMut(Msg)) -> Result<()> {
        for (name, value) in secrets {
            self.secret_set(app, name, value.as_bytes(), a)?;
        }
        Ok(())
    }

    pub fn secret_rm(&mut self, app: &str, name: &str, a: &mut dyn FnMut(Msg)) -> Result<()> {
        self.command(&format!("SECRET RM {app} {name}"), a).map(|_| ())
    }

    /// Delete all of an app's secrets (its logins, keys and saved choices).
    pub fn secrets_clear(&mut self, app: &str, a: &mut dyn FnMut(Msg)) -> Result<()> {
        self.command(&format!("SECRET CLEAR {app}"), a).map(|_| ())
    }

    /// Wi-Fi status (`WIFI`).
    pub fn wifi_status(&mut self, a: &mut dyn FnMut(Msg)) -> Result<wifi::Status> {
        Ok(wifi::parse_status(&self.command("WIFI", a)?.0))
    }

    /// Store Wi-Fi credentials on the device and (re)connect.
    pub fn wifi_set(&mut self, ssid: &str, password: &str, a: &mut dyn FnMut(Msg)) -> Result<wifi::Status> {
        let cmd = wifi::set_command(ssid, password)?;
        Ok(wifi::parse_status(&self.command(&cmd, a)?.0))
    }

    /// Forget the Wi-Fi network; Wi-Fi off.
    pub fn wifi_off(&mut self, a: &mut dyn FnMut(Msg)) -> Result<wifi::Status> {
        Ok(wifi::parse_status(&self.command("WIFI OFF", a)?.0))
    }

    pub fn eval(&mut self, code: &str, a: &mut dyn FnMut(Msg)) -> Result<String> {
        let one_line = code.replace(['\r', '\n'], " ");
        self.command_within(&format!("EVAL {one_line}"), LUA_CALL_TIMEOUT, a).map(|r| r.0)
    }

    /// Lend the device this computer's network (again within
    /// `tether::KEEP_EVERY` to keep it), telling it the time (UTC seconds).
    /// True if the tether starts now: the device has no connections through
    /// it from before.
    pub fn tun_on(&mut self, utc: u64, a: &mut dyn FnMut(Msg)) -> Result<bool> {
        self.command(&format!("TUN ON {utc}"), a).map(|r| r.0.ends_with(" started"))
    }

    pub fn tun_off(&mut self, a: &mut dyn FnMut(Msg)) -> Result<()> {
        self.command("TUN OFF", a).map(|_| ())
    }

    /// The computer's frames, in one write: no reply comes.
    pub fn tun_send(&mut self, frames: &[tether::Frame]) -> Result<()> {
        let lines: String = frames.iter().map(|f| f.line() + "\n").collect();
        self.write_all(lines.as_bytes())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    /// A device that answers each command only once it gets the next line,
    /// i.e. always too late.
    struct LateDevice {
        replies: std::collections::VecDeque<&'static str>,
        rx: Vec<u8>,
        line: Vec<u8>,
    }

    impl Read for LateDevice {
        fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
            if self.rx.is_empty() {
                std::thread::sleep(Duration::from_millis(2));
                return Err(std::io::ErrorKind::TimedOut.into());
            }
            let n = buf.len().min(self.rx.len());
            buf[..n].copy_from_slice(&self.rx[..n]);
            self.rx.drain(..n);
            Ok(n)
        }
    }

    impl Link for LateDevice {
        fn set_wait(&mut self, _: Duration) {}
    }

    impl Write for LateDevice {
        fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
            for &b in buf {
                if b != b'\n' {
                    self.line.push(b);
                    continue;
                }
                if let Some(r) = self.replies.pop_front() {
                    self.rx.extend_from_slice(r.as_bytes()); // the previous command's reply
                }
                if self.line == b"PING" {
                    self.rx.extend_from_slice(b"OK PONG\n");
                }
                self.line.clear();
            }
            Ok(buf.len())
        }
        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }

    #[test]
    fn a_late_reply_is_not_read_as_the_next_ones() {
        let late = LateDevice { replies: ["", "OK 42\n"].into(), rx: Vec::new(), line: Vec::new() };
        let mut dev = Device::on(Box::new(late));
        dev.timeout = Duration::from_millis(50);
        let mut a = |_m: Msg| {};
        assert!(dev.command("EVAL slow()", &mut a).is_err());
        // The 42 came after the resync PING and was dropped with everything
        // up to the PONG; the next command gets its own reply.
        assert_eq!(dev.command("PING", &mut a).unwrap().0, "PONG");
    }

    #[test]
    fn secret_listing_lines_parse() {
        assert_eq!(parse_secret_line("gcal gcal_calendars 42"), Some(("gcal".into(), "gcal_calendars".into(), 42)));
        assert_eq!(parse_secret_line("gcal 42"), None);
        assert_eq!(parse_secret_line("gcal x y"), None);
    }

    #[test]
    fn link_errors_are_told_apart_from_device_errors() {
        let io = std::io::Error::new(std::io::ErrorKind::BrokenPipe, "device disconnected");
        let lost: anyhow::Error = anyhow::Error::from(io).context("reading from the device failed");
        assert!(is_link_error(&lost));
        let eio: anyhow::Error = std::io::Error::from_raw_os_error(5).into();
        assert!(is_link_error(&eio));
        assert!(!is_link_error(&anyhow!("device: no such file: x.lua")));
        assert!(!is_link_error(&anyhow!("timeout waiting for reply to 'PING'")));
    }

    #[test]
    fn parse_lines() {
        assert_eq!(Msg::parse("OK PONG"), Msg::Ok("PONG".into()));
        assert_eq!(Msg::parse("OK"), Msg::Ok("".into()));
        assert_eq!(Msg::parse("LOG a b"), Msg::Log("a b".into()));
        assert_eq!(printable("a\tb\x1b]52;c;eA==\x07\r\u{9b}2K"), "a\tb\u{fffd}]52;c;eA==\u{fffd}\u{fffd}\u{fffd}2K");
        assert_eq!(Msg::parse("ets Jun  8 2016"), Msg::Other("ets Jun  8 2016".into()));
    }
}
