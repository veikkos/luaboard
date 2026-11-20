//! What the firmware update sections (s25_ota_*) share. The simulator's app
//! image is a simulator executable: an update reboots into it for real, so the
//! confirmation to the bootloader and the rollback run as on the device.

use std::fs;
use std::io::{ErrorKind, Read, Write};
use std::path::Path;
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc, Arc, Mutex};
use std::time::{Duration, Instant};

use sha2::{Digest, Sha256};

use crate::harness::T;

/// What `luaboard update` prints once the image is in the device: from here
/// on it waits for the device, whose clock can run ahead (T::e_ahead).
pub const REBOOTS: &str = "the device reboots";

/// A field of the device's INFO (info(t, "slot") = "ota_0").
pub fn info(t: &T, key: &str) -> String {
    t.info(key)
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

pub fn sha256(data: &[u8]) -> String {
    hex(&Sha256::digest(data))
}

/// A link to the device held open: what is sent goes out step by step, what
/// the device says is collected as it comes. The serial link is opened here
/// (when `send` returns the device has the bytes, so an advance right after
/// it finds them read); over Wi-Fi it is `luaboard raw`, which has the key.
pub struct Raw {
    cli: Option<(Child, Option<ChildStdin>)>,
    link: Option<(mpsc::Sender<Vec<u8>>, mpsc::Receiver<()>)>,
    out: Arc<Mutex<Vec<u8>>>,
    stop: Arc<AtomicBool>,
    reader: Option<std::thread::JoinHandle<()>>,
}

impl Raw {
    pub fn start(t: &T) -> Raw {
        // "In use" for a moment is this program's own doing: opening tries
        // the port as a real one first, with a lock, and a process another
        // section starts just then has that file (and lock) until its exec.
        let end = Instant::now() + Duration::from_secs(2);
        let mut link = loop {
            match luaboard::open_link_poll(&t.tty, 115200, Duration::from_millis(1)) {
                Ok(link) => break link,
                Err(e) if format!("{e:#}").contains("in use") && Instant::now() < end => {
                    std::thread::sleep(Duration::from_millis(1));
                }
                Err(e) => panic!("the serial link: {e:#}"),
            }
        };
        let out = Arc::new(Mutex::new(Vec::new()));
        let stop = Arc::new(AtomicBool::new(false));
        let (tx, rx) = mpsc::channel::<Vec<u8>>();
        let (sent_tx, sent_rx) = mpsc::channel();
        let (o, st) = (out.clone(), stop.clone());
        let reader = std::thread::spawn(move || {
            let mut buf = [0u8; 8192];
            while !st.load(Ordering::SeqCst) {
                while let Ok(data) = rx.try_recv() {
                    let mut got = |b: &[u8]| o.lock().unwrap().extend_from_slice(b);
                    let _ = luaboard::write_all_reading(&mut *link, &data, Duration::from_secs(5), &mut got);
                    let _ = sent_tx.send(());
                }
                match link.read(&mut buf) {
                    Ok(n) => o.lock().unwrap().extend_from_slice(&buf[..n]),
                    Err(e) if matches!(e.kind(), ErrorKind::TimedOut | ErrorKind::WouldBlock) => {}
                    Err(_) => std::thread::sleep(Duration::from_millis(1)), // the simulator is gone
                }
            }
        });
        Raw { cli: None, link: Some((tx, sent_rx)), out, stop, reader: Some(reader) }
    }
    /// On another link than USB, e.g. tcp://127.0.0.1:N.
    pub fn start_on(t: &T, port: &str) -> Raw {
        let mut child = Command::new(&t.cli)
            .args(["-p", port, "--linger", "0.3", "raw"])
            .env("LUABOARD_KEYS", &t.keys)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .unwrap();
        let stdin = child.stdin.take();
        let mut stdout = child.stdout.take().unwrap();
        let out = Arc::new(Mutex::new(Vec::new()));
        let o = out.clone();
        std::thread::spawn(move || {
            let mut buf = [0u8; 4096];
            while let Ok(n) = stdout.read(&mut buf) {
                if n == 0 {
                    break;
                }
                o.lock().unwrap().extend_from_slice(&buf[..n]);
            }
        });
        Raw { cli: Some((child, stdin)), link: None, out, stop: Arc::new(AtomicBool::new(false)), reader: None }
    }
    pub fn send(&mut self, data: &[u8]) {
        if let Some((tx, sent)) = &self.link {
            if tx.send(data.to_vec()).is_ok() {
                let _ = sent.recv_timeout(Duration::from_secs(5));
            }
        }
        if let Some((_, Some(s))) = self.cli.as_mut() {
            let _ = s.write_all(data).and_then(|_| s.flush());
        }
    }
    pub fn text(&self) -> String {
        String::from_utf8_lossy(&self.out.lock().unwrap()).into_owned()
    }
    pub fn has(&self, s: &str) -> bool {
        self.text().contains(s)
    }
    /// Lines with PONG in them (grep -c).
    pub fn pongs(&self) -> usize {
        self.text().lines().filter(|l| l.contains("PONG")).count()
    }
    /// Everything the device has said, also to what was sent last: a PING
    /// goes after it, and its answer (left out of the text) ends the wait.
    /// A device that takes no commands right then (in the middle of an
    /// upload) is given 0.3 s instead.
    pub fn finish(mut self) -> String {
        let before = self.pongs();
        self.send(b"\nPING\n");
        let end = Instant::now() + Duration::from_millis(300);
        while self.pongs() == before && Instant::now() < end {
            std::thread::sleep(Duration::from_millis(1));
        }
        let text = self.text();
        if self.pongs() == before {
            return text;
        }
        match text.trim_end().rfind('\n') {
            Some(i) if text[i..].contains("PONG") => text[..i + 1].to_string(),
            None if text.contains("PONG") => String::new(),
            _ => text,
        }
    }
}

impl Drop for Raw {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::SeqCst);
        if let Some(r) = self.reader.take() {
            let _ = r.join(); // the link is free again when this returns
        }
        if let Some((child, _)) = self.cli.as_mut() {
            let _ = child.kill();
            let _ = child.wait();
        }
    }
}

/// The reply lines (OK/ERR) of raw output.
fn replies(out: &str) -> Vec<String> {
    out.split('\n')
        .map(|l| l.trim_end_matches('\r'))
        .filter(|l| l.starts_with("OK") || l.starts_with("ERR"))
        .map(String::from)
        .collect()
}

/// OTA by hand, for what the CLI refuses to send: SIZE SHA FILE. The image
/// goes only after OK READY (luaboard raw: the bytes as they are). The OK/ERR
/// replies, each followed by a space.
pub fn raw_ota(t: &T, size: usize, sha: &str, file: &Path) -> String {
    let mut raw = Raw::start(t);
    raw.send(format!("\nOTA {size} {sha}\n").as_bytes());
    t.until_real(2.0, |_| !replies(&raw.text()).is_empty()); // READY, or why not
    if raw.has("OK READY") {
        raw.send(&fs::read(file).unwrap());
        t.until_real(5.0, |_| replies(&raw.text()).len() >= 2);
    }
    replies(&raw.finish()).iter().map(|l| format!("{l} ")).collect()
}

/// An image made to fail: a copy of the simulator with `fail=none ` patched
/// (macOS wants it signed again).
pub fn failing(t: &T, mode: &str, dest: &Path) {
    let mut d = fs::read(&t.sim_bin).unwrap();
    let (from, to) = (b"fail=none ".to_vec(), format!("fail={mode} ").into_bytes());
    assert_eq!(from.len(), to.len());
    let mut i = 0;
    while i + from.len() <= d.len() {
        if d[i..i + from.len()] == from[..] {
            d[i..i + from.len()].copy_from_slice(&to);
            i += from.len();
        } else {
            i += 1;
        }
    }
    fs::write(dest, d).unwrap();
    copy_mode(&t.sim_bin, dest);
    if cfg!(target_os = "macos") {
        let _ = Command::new("codesign")
            .args(["-f", "-s", "-"])
            .arg(dest)
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status();
    }
}

/// cp: the file and its permissions (executable).
pub fn cp(from: &Path, to: &Path) {
    fs::copy(from, to).unwrap();
}

fn copy_mode(from: &Path, to: &Path) {
    if let Ok(m) = fs::metadata(from) {
        let _ = fs::set_permissions(to, m.permissions());
    }
}

pub fn tail(text: &str, n: usize) -> String {
    let l: Vec<&str> = text.lines().collect();
    l[l.len().saturating_sub(n)..].join("\n")
}

pub fn has_app(t: &T, name: &str) -> bool {
    t.e(&["apps"]).lines().any(|l| l.starts_with(name))
}
