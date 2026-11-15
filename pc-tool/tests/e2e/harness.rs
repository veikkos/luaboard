//! What every section works with: a simulator, the CLI and a work folder (T),
//! and checks that are counted and printed (ok/bad/check).

#![allow(dead_code)]

use std::cell::RefCell;
use std::fs;
use std::io::{BufRead, BufReader, Read, Write};
use std::net::TcpStream;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::mpsc::{self, Receiver};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

thread_local! {
    /// The message of the last panic in this thread (see main's panic hook).
    pub static PANIC_MSG: RefCell<String> = const { RefCell::new(String::new()) };
}

/// No test depends on the real date: the simulated world (NTP, the RTC chip)
/// starts at this time in every boot, Fri 2026-11-06 13:30 UTC (15:30 in
/// Finland), and runs with the device's clock. The fake servers use it too.
pub const E2E_TIME: u64 = 1793971800;

pub const WINDOWS: bool = cfg!(windows);
pub const EXE: &str = std::env::consts::EXE_SUFFIX;

/// What a section shares with the runner: its output and counts, and the
/// processes to kill if it runs over its time.
#[derive(Default)]
pub struct Shared {
    out: Mutex<String>,
    pass: AtomicUsize,
    fail: AtomicUsize,
    pids: Mutex<Vec<u32>>,
    done: AtomicBool,
    secs: AtomicUsize,
    /// Where the time went (E2E_TIMES=1): what, how often, how long.
    prof: Mutex<Vec<(&'static str, u32, Duration)>>,
    /// When the last check was counted.
    last: Mutex<Option<Instant>>,
}

/// E2E_TIMES=1: each check with the time since the one before, and each
/// section's time by what it went to.
pub fn times() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("E2E_TIMES").is_some())
}

impl Shared {
    pub fn say(&self, line: &str) {
        let mut o = self.out.lock().unwrap();
        o.push_str(line);
        o.push('\n');
    }
    pub fn fail_line(&self, msg: &str) {
        self.say(&format!("  FAIL  {msg}{}", self.since_last()));
        self.fail.fetch_add(1, Ordering::SeqCst);
    }
    /// " (N ms)" since the check before this one, with E2E_TIMES.
    fn since_last(&self) -> String {
        if !times() {
            return String::new();
        }
        let mut last = self.last.lock().unwrap();
        let now = Instant::now();
        let ms = last.map_or(0, |t| (now - t).as_millis());
        *last = Some(now);
        format!(" ({ms} ms)")
    }
    /// Count the time since `t0` as spent on `what`.
    pub fn spent(&self, what: &'static str, t0: Instant) {
        if !times() {
            return;
        }
        let d = t0.elapsed();
        let mut p = self.prof.lock().unwrap();
        match p.iter_mut().find(|e| e.0 == what) {
            Some(e) => {
                e.1 += 1;
                e.2 += d;
            }
            None => p.push((what, 1, d)),
        }
    }
    /// "boot 3x 410 ms, cli 25x 300 ms, ..."
    pub fn spent_on(&self) -> String {
        let p = self.prof.lock().unwrap();
        p.iter().map(|(w, n, d)| format!("{w} {n}x {} ms", d.as_millis())).collect::<Vec<_>>().join(", ")
    }
    /// Forget the output and counts so far: the section runs again.
    pub fn reset(&self) {
        self.out.lock().unwrap().clear();
        self.pass.store(0, Ordering::SeqCst);
        self.fail.store(0, Ordering::SeqCst);
    }
    pub fn finish(&self, secs: u64) {
        if !self.done.swap(true, Ordering::SeqCst) {
            self.secs.store(secs as usize, Ordering::SeqCst);
        }
    }
    pub fn is_done(&self) -> bool {
        self.done.load(Ordering::SeqCst)
    }
    pub fn output(&self) -> String {
        self.out.lock().unwrap().clone()
    }
    pub fn result(&self) -> (usize, usize, u64) {
        (self.pass.load(Ordering::SeqCst), self.fail.load(Ordering::SeqCst), self.secs.load(Ordering::SeqCst) as u64)
    }
    fn add_pid(&self, pid: u32) {
        self.pids.lock().unwrap().push(pid);
    }
    fn remove_pid(&self, pid: u32) {
        self.pids.lock().unwrap().retain(|p| *p != pid);
    }
    pub fn kill_children(&self) {
        for pid in self.pids.lock().unwrap().drain(..) {
            kill_pid(pid);
        }
    }
}

#[cfg(unix)]
fn kill_pid(pid: u32) {
    unsafe { libc::kill(pid as i32, libc::SIGKILL) };
}
#[cfg(windows)]
fn kill_pid(pid: u32) {
    let _ = Command::new("taskkill")
        .args(["/F", "/T", "/PID", &pid.to_string()])
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status();
}

/// The repository's root folder.
pub fn root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap().to_path_buf()
}

/// A TCP port nobody listens on right now, for something that listens on it
/// later (the device, the CLI's login redirect); the fake servers take a
/// free port themselves (port 0). Not from the ports the system hands out for
/// port 0 and outgoing connections (32768 and up on Linux, 49152 and up on
/// macOS and Windows): one of those could take it before its listener does.
/// Never the same port twice in a run.
/// How long the firmware's Restarted screen stays up after a crash
/// (RESTART_NOTICE_MS in app_config.h); boot() after crash() runs past it.
pub const RESTART_NOTICE_S: i64 = 10;

pub fn free_port() -> u16 {
    static GIVEN: Mutex<Vec<u16>> = Mutex::new(Vec::new());
    let mut given = GIVEN.lock().unwrap();
    loop {
        let mut b = [0u8; 2];
        getrandom::fill(&mut b).unwrap();
        let port = 20000 + u16::from_le_bytes(b) % 12000;
        // (asked by connecting, not by binding it: a process another section
        // starts just then would hold the bound socket until its exec, and
        // the port with it, for whoever listens on it next)
        if !given.contains(&port) && !port_open(port) {
            given.push(port);
            return port;
        }
    }
}

pub fn port_open(port: u16) -> bool {
    TcpStream::connect_timeout(&([127, 0, 0, 1], port).into(), Duration::from_secs(1)).is_ok()
}

/// What a TCP server on this computer sends until it closes (2 s at most),
/// without trailing newlines.
pub fn tcp_read(port: u16) -> String {
    let Ok(mut s) = TcpStream::connect_timeout(&([127, 0, 0, 1], port).into(), Duration::from_secs(2)) else {
        return String::new();
    };
    let deadline = Instant::now() + Duration::from_secs(2);
    let mut out = Vec::new();
    let mut buf = [0u8; 4096];
    while Instant::now() < deadline {
        let _ =
            s.set_read_timeout(Some(deadline.saturating_duration_since(Instant::now()).max(Duration::from_millis(1))));
        match s.read(&mut buf) {
            Ok(0) | Err(_) => break,
            Ok(n) => out.extend_from_slice(&buf[..n]),
        }
    }
    chomp(String::from_utf8_lossy(&out).into_owned())
}

/// Text without its trailing newlines, as a shell's $(...) gives it.
pub fn chomp(mut s: String) -> String {
    while s.ends_with('\n') || s.ends_with('\r') {
        s.pop();
    }
    s
}

/// grep: does any line of `text` match the regular expression? (^ and $ are
/// a line's ends.)
pub fn grep(pattern: &str, text: &str) -> bool {
    regex::Regex::new(&format!("(?m){pattern}")).unwrap().is_match(text)
}

/// A screen capture (PPM, P6).
pub struct Ppm {
    pub w: usize,
    pub h: usize,
    pub px: Vec<u8>,
}

impl Ppm {
    pub fn load(path: &Path) -> Ppm {
        let d = fs::read(path).unwrap_or_default();
        // "P6\n<w> <h>\n255\n" then the pixels
        let mut fields = Vec::new();
        let mut pos = 0;
        while fields.len() < 4 && pos < d.len() {
            while pos < d.len() && d[pos].is_ascii_whitespace() {
                pos += 1;
            }
            let s = pos;
            while pos < d.len() && !d[pos].is_ascii_whitespace() {
                pos += 1;
            }
            fields.push(String::from_utf8_lossy(&d[s..pos]).into_owned());
        }
        if fields.len() < 4 || fields[0] != "P6" {
            return Ppm { w: 0, h: 0, px: Vec::new() };
        }
        let w = fields[1].parse().unwrap_or(0);
        let h = fields[2].parse().unwrap_or(0);
        Ppm { w, h, px: d.get(pos + 1..).unwrap_or_default().to_vec() }
    }
    /// The same picture: the size, and every color within what two ways of
    /// widening the LCD's 5 and 6 bits to 8 can differ by.
    pub fn same_as(&self, other: &Ppm) -> bool {
        self.w > 0 && (self.w, self.h) == (other.w, other.h) && self.px.len() == other.px.len() && {
            self.px.iter().zip(&other.px).all(|(a, b)| a.abs_diff(*b) <= 8)
        }
    }
    /// The color at (x, y).
    pub fn rgb(&self, x: usize, y: usize) -> (u8, u8, u8) {
        let i = (y * self.w + x) * 3;
        match self.px.get(i..i + 3) {
            Some(p) => (p[0], p[1], p[2]),
            None => (0, 0, 0),
        }
    }
    /// Color bytes brighter than the dark backgrounds the examples use.
    pub fn lit_pixels(&self) -> usize {
        self.px.iter().filter(|b| **b > 60).count()
    }
    /// Pixels of the red "Lua error" header (R > 180, G and B < 90).
    pub fn red_pixels(&self) -> usize {
        self.px.as_chunks::<3>().0.iter().filter(|p| p[0] > 180 && p[1] < 90 && p[2] < 90).count()
    }
    /// Pixels in x0..x1, y0..y1 for which `f(r, g, b)` holds.
    pub fn count(
        &self,
        xs: std::ops::Range<usize>,
        ys: std::ops::Range<usize>,
        f: impl Fn(u8, u8, u8) -> bool,
    ) -> usize {
        let mut n = 0;
        for y in ys {
            for x in xs.clone() {
                let (r, g, b) = self.rgb(x, y);
                n += f(r, g, b) as usize;
            }
        }
        n
    }
}

/// A running simulator and its control channel.
struct Sim {
    child: Child,
    ctl: TcpStream,
    acks: Receiver<String>,
    ctl_port: u16,
}

/// A connection to a simulator's control channel and its answers, line by line.
fn ctl_connect(port: u16) -> (TcpStream, Receiver<String>) {
    let ctl = TcpStream::connect(("127.0.0.1", port)).unwrap();
    let _ = ctl.set_nodelay(true);
    let ack_r = ctl.try_clone().unwrap();
    let (tx, rx) = mpsc::channel();
    std::thread::spawn(move || {
        for line in BufReader::new(ack_r).lines() {
            let Ok(line) = line else { break };
            if tx.send(line.trim_end_matches('\r').to_string()).is_err() {
                break;
            }
        }
    });
    (ctl, rx)
}

/// A section's world: its work folder, simulator and CLI.
pub struct T {
    pub root: PathBuf,
    pub work: PathBuf,
    /// The simulator executable (changeable: a section may boot another build).
    pub sim_bin: PathBuf,
    pub cli: PathBuf,
    /// The simulator's serial port: a link in the work folder, on Windows a named pipe.
    pub tty: String,
    /// The app of scripts put from the work folder (its name).
    pub wapp: String,
    /// This "computer"'s Wi-Fi pairing keys (LUABOARD_KEYS for the CLI).
    pub keys: PathBuf,
    /// Leave the Restarted screen of a boot after crash() up (to test it)
    /// instead of fast-forwarding past it.
    pub keep_restart_notice: bool,
    /// Environment variables for the simulator (e.g. SIM_CA_FILE), from the
    /// next boot on.
    pub sim_env: Vec<(String, String)>,
    sh: Arc<Shared>,
    sim: Option<Sim>,
    bg: Vec<Child>,
    /// The last stop was crash(): the next boot shows the Restarted screen.
    crashed: bool,
}

impl T {
    pub fn new(_name: &str, sh: Arc<Shared>) -> T {
        let root = root();
        let sim_bin = std::env::var_os("SIM")
            .map(PathBuf::from)
            .unwrap_or_else(|| root.join(format!("build/m5stack-cores3/esp32-sim{EXE}")));
        let cli = PathBuf::from(env!("CARGO_BIN_EXE_luaboard"));
        let work = make_work_dir();
        let wapp = work.file_name().unwrap().to_string_lossy().into_owned();
        let tty = tty_in(&work);
        let keys = work.join("wifi-keys");
        T {
            root,
            work,
            sim_bin,
            cli,
            tty,
            wapp,
            keys,
            keep_restart_notice: false,
            sim_env: Vec::new(),
            sh,
            sim: None,
            bg: Vec::new(),
            crashed: false,
        }
    }

    /// Another world in a folder of this one's work folder (its own simulator),
    /// counting into the same section: for parts that run side by side.
    pub fn sub(&self, name: &str) -> T {
        let work = self.work.join(name);
        fs::create_dir_all(&work).unwrap();
        T {
            root: self.root.clone(),
            tty: tty_in(&work),
            wapp: name.to_string(),
            keys: self.keys.clone(),
            sim_bin: self.sim_bin.clone(),
            cli: self.cli.clone(),
            work,
            keep_restart_notice: false,
            sim_env: Vec::new(),
            sh: self.sh.clone(),
            sim: None,
            bg: Vec::new(),
            crashed: false,
        }
    }

    // ---- output and checks ---------------------------------------------

    /// A line of output (e.g. "== the section's part").
    pub fn say(&self, line: &str) {
        self.sh.say(line);
    }
    pub fn ok(&self, msg: &str) {
        self.sh.say(&format!("  PASS  {msg}{}", self.sh.since_last()));
        self.sh.pass.fetch_add(1, Ordering::SeqCst);
    }
    pub fn bad(&self, msg: &str) {
        self.sh.fail_line(msg);
    }
    pub fn fail_line(&self, msg: &str) {
        self.sh.fail_line(msg);
    }
    /// ok(`ok`) if `cond`, else bad with the message `bad` makes (only then
    /// run, so it can ask the device what went wrong).
    pub fn check(&mut self, cond: bool, ok: &str, bad: impl FnOnce(&mut T) -> String) -> bool {
        if cond {
            self.ok(ok);
        } else {
            let m = bad(self);
            self.bad(&m);
        }
        cond
    }
    pub fn failed(&self) -> bool {
        self.sh.fail.load(Ordering::SeqCst) > 0
    }

    // ---- files -----------------------------------------------------------

    /// A path in the work folder, as a string for the CLI's arguments.
    pub fn path(&self, name: &str) -> String {
        self.work.join(name).to_string_lossy().into_owned()
    }
    /// Write a file in the work folder (making its folders); returns its path.
    pub fn write(&self, name: &str, content: impl AsRef<[u8]>) -> String {
        let p = self.work.join(name);
        if let Some(d) = p.parent() {
            fs::create_dir_all(d).unwrap();
        }
        fs::write(&p, content).unwrap();
        p.to_string_lossy().into_owned()
    }
    /// A file in the work folder as text ("" if it isn't there).
    pub fn read(&self, name: &str) -> String {
        String::from_utf8_lossy(&fs::read(self.work.join(name)).unwrap_or_default()).into_owned()
    }

    // ---- the simulator -----------------------------------------------------

    /// The port the simulator's Wi-Fi server took (it binds any free one,
    /// wifi_port=0, and prints it): there once Wi-Fi is up. 0 if it never is.
    pub fn wifi_port(&mut self) -> u16 {
        let mut port = 0;
        self.until_sim(5.0, |t| {
            port = t
                .read("sim.out")
                .lines()
                .find_map(|l| l.strip_prefix("Port 3333: 127.0.0.1:")?.trim().parse::<u16>().ok())
                .unwrap_or(0);
            port != 0
        });
        port
    }

    /// NVS (settings, Wi-Fi, secrets) is a file; the apps partition and the
    /// app slots (firmware updates) are folders next to it.
    pub fn wipe(&self) {
        for ext in ["", ".flash", ".rtc", ".run", ".ota"] {
            let p = self.work.join(format!("nvs.bin{ext}"));
            let _ = fs::remove_dir_all(&p);
            let _ = fs::remove_file(&p);
        }
    }

    /// Start the simulator and run it through its power-on (the firmware's
    /// hardware delays, the boot-touch window) in fast-forward; returns once it
    /// answers on the serial port and on its control channel. It runs until
    /// stop (a clean power-off, which writes screen.ppm) or crash (killed).
    /// Its Wi-Fi server takes any free port (wifi_port=0), so parallel sections
    /// don't collide; wifi_port() says which, once Wi-Fi is up.
    pub fn boot(&mut self) {
        self.boot_with(&[]);
    }
    pub fn boot_with(&mut self, extra: &[&str]) {
        assert!(self.sim.is_none(), "boot: the simulator is already running");
        let t0 = Instant::now();
        let _ = fs::remove_file(self.work.join("sim.out"));
        let time = E2E_TIME.to_string();
        let mut args: Vec<String> = vec![
            "--headless".into(),
            "--loopback".into(),
            "--nvs".into(),
            self.path("nvs.bin"),
            "--link".into(),
            self.tty.clone(),
            "--ppm".into(),
            self.path("screen.ppm"),
            "--control".into(),
            "0".into(),
            "--set".into(),
            "wifi_port=0".into(),
            "--set".into(),
            format!("time={time}"),
        ];
        args.extend(extra.iter().map(|s| s.to_string()));
        let mut cmd = Command::new(&self.sim_bin);
        cmd.args(&args)
            .env("SIM_IMAGE_LINK", "1") // a store's first image is a symbolic link, no copy (a copy's first start is slow on macOS)
            .env("SIM_BOOT_ADVANCE", "300") // every boot starts 300 ms fast-forwarded: its hardware delays, the boot-touch window
            .envs(self.sim_env.iter().map(|(k, v)| (k, v)))
            .stdin(Stdio::null())
            .stdout(fs::File::create(self.work.join("sim.out")).unwrap())
            .stderr(fs::File::create(self.work.join("sim.err")).unwrap());
        // A simulator a section just copied can be "busy" for a moment on
        // Linux: a CLI another section is starting right then still holds
        // the copy open for writing, until it has become the CLI.
        let busy_until = Instant::now() + Duration::from_secs(5);
        let child = loop {
            match cmd.spawn() {
                Ok(child) => break child,
                Err(e) if e.kind() == std::io::ErrorKind::ExecutableFileBusy && Instant::now() < busy_until => {
                    std::thread::sleep(Duration::from_millis(10));
                }
                Err(e) => panic!("starting the simulator {}: {e}", self.sim_bin.display()),
            }
        };
        self.sh.add_pid(child.id());
        // The control channel: a TCP port on 127.0.0.1, printed as
        // "Control: 127.0.0.1:N" once the serial link is there too.
        let deadline = Instant::now() + Duration::from_secs(5);
        let mut port = None;
        while Instant::now() < deadline {
            port = self
                .read("sim.out")
                .lines()
                .find_map(|l| l.strip_prefix("Control:")?.trim().strip_prefix("127.0.0.1:")?.parse::<u16>().ok());
            if port.is_some() {
                break;
            }
            std::thread::sleep(Duration::from_millis(1));
        }
        let Some(port) = port else {
            let mut child = child;
            let ended = child.try_wait();
            let _ = child.kill();
            panic!(
                "simulator did not start ({ended:?}); it printed: {} / {}",
                self.read("sim.out"),
                self.read("sim.err")
            )
        };
        let (ctl, acks) = ctl_connect(port);
        self.sim = Some(Sim { child, ctl, acks, ctl_port: port });
        // The boot's own fast-forward (SIM_BOOT_ADVANCE) is over when this is answered.
        self.advance(0);
        // After crash() the firmware shows its Restarted screen first (10 s of
        // its clock): run through that too, unless a test looks at it.
        if std::mem::take(&mut self.crashed) && !self.keep_restart_notice {
            self.advance(RESTART_NOTICE_S as u64 * 1000 + 100);
        }
        self.sh.spent("boot", t0);
    }

    /// Whether the simulator is running.
    pub fn sim_running(&mut self) -> bool {
        match &mut self.sim {
            Some(s) => matches!(s.child.try_wait(), Ok(None)),
            None => false,
        }
    }

    /// The simulator's process id.
    pub fn sim_pid(&self) -> Option<u32> {
        self.sim.as_ref().map(|s| s.child.id())
    }

    /// A clean power-off: quit=1 on the control channel (it writes screen.ppm).
    pub fn stop(&mut self) {
        let Some(mut s) = self.sim.take() else { return };
        let t0 = Instant::now();
        if matches!(s.child.try_wait(), Ok(None)) {
            let quit = s.ctl.write_all(b"quit=1\n").is_ok() && s.acks.recv_timeout(Duration::from_secs(10)).is_ok();
            if !quit {
                let _ = s.child.kill();
            }
        }
        if !wait_timeout(&mut s.child, Duration::from_secs(10)) {
            let _ = s.child.kill();
            let _ = s.child.wait();
        }
        self.sh.remove_pid(s.child.id());
        self.sh.spent("stop", t0);
    }

    /// A power-off by signal (not Windows), as Ctrl-C or `kill` gives one:
    /// true if the simulator ended by itself within 10 s.
    #[cfg(unix)]
    pub fn term(&mut self) -> bool {
        if let Some(s) = &self.sim {
            unsafe {
                libc::kill(s.child.id() as i32, libc::SIGTERM);
            }
        }
        self.wait_exit(10.0)
    }

    /// A real crash in the simulator (not Windows): SIGSEGV, as a bad pointer
    /// gives. Its crash handler records it and the chip restarts by itself.
    #[cfg(unix)]
    pub fn segv(&mut self) {
        if let Some(s) = &self.sim {
            unsafe {
                libc::kill(s.child.id() as i32, libc::SIGSEGV);
            }
        }
    }

    /// Power gone at once (kill -9): no clean shutdown.
    pub fn crash(&mut self) {
        let Some(mut s) = self.sim.take() else { return };
        self.crashed = true;
        let _ = s.child.kill();
        let _ = s.child.wait();
        self.sh.remove_pid(s.child.id());
    }

    /// Wait for the simulator to end by itself (it reboots into an update,
    /// quits, ...) for up to `secs`; true if it did.
    pub fn wait_exit(&mut self, secs: f64) -> bool {
        let Some(s) = self.sim.as_mut() else { return true };
        if wait_timeout(&mut s.child, Duration::from_secs_f64(secs)) {
            let s = self.sim.take().unwrap();
            self.sh.remove_pid(s.child.id());
            true
        } else {
            false
        }
    }

    /// Control lines for the simulator (--control): button=A|B|C|-,
    /// touch=X,Y|-, power=short|long, battery=N, ... and advance=MS. Each
    /// returns once it has taken effect; advance once the firmware has run
    /// through MS of its own time, which takes a few milliseconds instead of MS.
    pub fn ctl(&mut self, lines: &[&str]) {
        let first = lines.first().copied().unwrap_or("");
        if !self.sim_running() {
            panic!("simulator not running for control line {first}");
        }
        let s = self.sim.as_mut().unwrap();
        // All lines in one write, then the answers: the simulator takes them in
        // order (lines after an advance wait for it), so no real time passes
        // between them while the device's clock runs (a press could turn into
        // a hold).
        let mut all = String::new();
        for l in lines {
            all.push_str(l);
            all.push('\n');
        }
        if s.ctl.write_all(all.as_bytes()).and_then(|_| s.ctl.flush()).is_err() {
            panic!("simulator not running for control line {first}");
        }
        let mut wrong = Vec::new();
        let mut unanswered = None;
        let t0 = Instant::now();
        for l in lines {
            match s.acks.recv_timeout(Duration::from_secs(30)) {
                Ok(ack) if ack == format!("ok {l}") => {}
                Ok(ack) => wrong.push(format!("control line {l}: {ack}")),
                Err(e) => {
                    unanswered = Some(format!("{l} ({e})"));
                    break;
                }
            }
        }
        if let Some(l) = unanswered {
            let log = self.read("sim.err");
            let tail: Vec<&str> = log.lines().rev().take(8).collect();
            panic!("simulator did not answer control line {l}; it said last: {}", tail.join(" / "));
        }
        self.sh.spent("ctl", t0);
        if times() && t0.elapsed() >= Duration::from_millis(100) {
            self.say(&format!("  [{} ms: control {}]", t0.elapsed().as_millis(), lines.join(" ")));
        }
        for w in wrong {
            self.bad(&w);
        }
    }

    /// Is the board in this state: "on", "off" (powered down) or "light"
    /// (asleep with light sleep allowed)?
    pub fn is(&mut self, state: &str) -> bool {
        self.ask(&format!("is={state}"))
    }

    /// The board has powered down: true once it has. The firmware says it's
    /// turning off a moment before the power really goes, so this waits for
    /// it (up to 2 s) where `is("off")` would ask too soon.
    pub fn wait_off(&mut self) -> bool {
        for _ in 0..1000 {
            if self.is("off") {
                return true;
            }
            std::thread::sleep(Duration::from_millis(2));
        }
        false
    }

    /// A question on the control channel, such as `is=off` or
    /// `pwm=8:1000:250` (is that pin pulsing at 1000 Hz, 250 per mille on?):
    /// true if the answer is ok. A no doesn't count as a failure.
    pub fn ask(&mut self, line: &str) -> bool {
        if !self.sim_running() {
            panic!("simulator not running for control line {line}");
        }
        let s = self.sim.as_mut().unwrap();
        if s.ctl.write_all(format!("{line}\n").as_bytes()).and_then(|_| s.ctl.flush()).is_err() {
            panic!("simulator not running for control line {line}");
        }
        match s.acks.recv_timeout(Duration::from_secs(30)) {
            Ok(ack) => ack == format!("ok {line}"),
            Err(_) => panic!("simulator did not answer control line {line}"),
        }
    }

    /// The simulator's control channel port (127.0.0.1).
    pub fn ctl_port(&self) -> u16 {
        self.sim.as_ref().map_or(0, |s| s.ctl_port)
    }

    /// `luaboard sim-ctl LINES` against this simulator: the harness lets go
    /// of the control channel meanwhile (one client at a time). Its output.
    pub fn sim_ctl(&mut self, lines: &[&str]) -> String {
        let Some(s) = self.sim.as_mut() else { panic!("sim_ctl: the simulator isn't running") };
        let port = s.ctl_port;
        let _ = s.ctl.shutdown(std::net::Shutdown::Both);
        let control = port.to_string();
        let mut args = vec!["sim-ctl", "--control", control.as_str()];
        args.extend_from_slice(lines);
        let out = self.cli(&args);
        let (ctl, acks) = ctl_connect(port);
        let s = self.sim.as_mut().unwrap();
        (s.ctl, s.acks) = (ctl, acks);
        out
    }

    /// Let MS of the device's time pass now (timers, timeouts, holds). Not
    /// while the device waits for a reply from outside: that still takes real
    /// time, and the device's timeout for it would run out first.
    pub fn advance(&mut self, ms: u64) {
        self.ctl(&[&format!("advance={ms}")]);
    }

    /// Hold a touch button for 150 ms, release it, and give the firmware 250 ms
    /// to act on it, all in the device's time.
    pub fn press(&mut self, button: &str) {
        self.press_for(button, 150);
    }
    pub fn press_for(&mut self, button: &str, ms: u64) {
        self.ctl(&[&format!("button={button}"), &format!("advance={ms}"), "button=-", "advance=250"]);
    }
    pub fn tap_at(&mut self, x: i32, y: i32) {
        self.ctl(&[&format!("touch={x},{y}"), "advance=150", "touch=-", "advance=250"]);
    }

    // ---- the CLI -------------------------------------------------------------

    /// The CLI on the simulator, without lingering for the device's output
    /// after run, put --run, install --run or a login (a script's top-level
    /// prints still come before the RUNNING reply): checks wait for what they
    /// need themselves. Its output and errors as one text (device errors are
    /// expected output here), without trailing newlines.
    pub fn e(&self, args: &[&str]) -> String {
        self.e_env(&[], args)
    }
    pub fn e_env(&self, env: &[(&str, &str)], args: &[&str]) -> String {
        let mut all = vec!["-p", &self.tty, "--linger", "0"];
        all.extend_from_slice(args);
        self.cli_env(env, &all)
    }
    /// The CLI with exactly these arguments (no -p / --linger).
    pub fn cli(&self, args: &[&str]) -> String {
        self.cli_env(&[], args)
    }
    pub fn cli_env(&self, env: &[(&str, &str)], args: &[&str]) -> String {
        self.cli_status(env, args).1
    }
    /// The CLI with exactly these arguments: (whether it succeeded, its output).
    pub fn cli_status(&self, env: &[(&str, &str)], args: &[&str]) -> (bool, String) {
        let mut cmd = Command::new(&self.cli);
        cmd.args(args).env("LUABOARD_KEYS", &self.keys).envs(env.iter().copied());
        let t0 = Instant::now();
        let (ok, out) = run_merged(&mut cmd, None, &self.sh);
        self.log_cli(args, t0, ok, &out);
        (ok, out)
    }
    /// Every CLI call and its output go to cli.log in the work folder (kept
    /// with E2E_KEEP=1 when a section fails).
    fn log_cli(&self, args: &[&str], t0: Instant, ok: bool, out: &str) {
        if times() && t0.elapsed() >= Duration::from_millis(100) {
            let line: String = args.join(" ").chars().take(100).collect();
            self.say(&format!("  [{} ms: luaboard {line}]", t0.elapsed().as_millis()));
        }
        let Ok(mut f) = fs::OpenOptions::new().create(true).append(true).open(self.work.join("cli.log")) else {
            return;
        };
        let _ = writeln!(
            f,
            "$ luaboard {} ({:.2} s{})\n{out}",
            args.join(" "),
            t0.elapsed().as_secs_f64(),
            if ok { "" } else { ", failed" }
        );
    }
    /// `e`, with the device's clock running ahead once the CLI has printed
    /// `from`: for a call that waits for the device to do something by its
    /// own clock (after an update the device waits half a second, reboots
    /// and starts, or crashes and starts again). Not before `from`: while
    /// the CLI still sends, the device's timeouts for it would run out.
    pub fn e_ahead(&mut self, args: &[&str], from: &str) -> String {
        let tty = self.tty.clone();
        let mut all = vec!["-p", &tty, "--linger", "0"];
        all.extend_from_slice(args);
        self.cli_ahead(&all, from)
    }
    /// e_ahead with exactly these arguments (no -p / --linger).
    pub fn cli_ahead(&mut self, args: &[&str], from: &str) -> String {
        let out = self.work.join("ahead.out");
        let f = fs::File::create(&out).unwrap();
        let mut child = Command::new(&self.cli)
            .args(args)
            .env("LUABOARD_KEYS", &self.keys)
            .stdin(Stdio::null())
            .stdout(f.try_clone().unwrap())
            .stderr(f)
            .spawn()
            .unwrap();
        self.sh.add_pid(child.id());
        let t0 = Instant::now();
        let mut ahead = false;
        let ok = loop {
            if let Ok(Some(status)) = child.try_wait() {
                break status.success();
            }
            if t0.elapsed() > CLI_LIMIT {
                let _ = child.kill();
                let _ = child.wait();
                break false;
            }
            ahead = ahead || self.read("ahead.out").contains(from);
            if ahead && self.sim_running() {
                self.advance(100);
            } else {
                std::thread::sleep(Duration::from_millis(1));
            }
        };
        self.sh.remove_pid(child.id());
        self.sh.spent("cli", t0);
        let text = chomp(self.read("ahead.out"));
        self.log_cli(args, t0, ok, &text);
        text
    }
    /// The CLI, with `input` on its standard input.
    pub fn cli_input(&self, args: &[&str], input: &[u8]) -> String {
        let mut cmd = Command::new(&self.cli);
        cmd.args(args).env("LUABOARD_KEYS", &self.keys);
        run_merged(&mut cmd, Some(input), &self.sh).1
    }
    /// The device over its serial link, opened from this program: what the
    /// CLI does for `eval` and `info`, without starting it. The tests ask
    /// the device something two thousand times, and a process each time was
    /// most of their time.
    fn device(&self) -> anyhow::Result<luaboard::Device> {
        luaboard::Device::open(&self.tty, 115200)
    }
    /// What `luaboard eval EXPR` prints: what the script printed meanwhile,
    /// then the result; "Error: ..." if it failed. (`e(&["eval", ..])` is
    /// the CLI itself.)
    pub fn eval(&self, expr: &str) -> String {
        let t0 = Instant::now();
        let mut out: Vec<String> = Vec::new();
        let result = self.device().and_then(|mut dev| {
            dev.eval(expr, &mut |m| {
                if let luaboard::Msg::Log(s) = m {
                    out.push(s);
                }
            })
        });
        match result {
            Ok(r) if r.is_empty() => {}
            Ok(r) => out.push(r),
            Err(e) => out.push(format!("Error: {e:?}")),
        }
        let text = chomp(out.join("\n"));
        self.sh.spent("dev", t0);
        self.log_cli(&["(in this program) eval", expr], t0, true, &text);
        text
    }
    /// eval for a call that never returns by itself (a busy loop): once it
    /// is under way the device's clock skips `ms` ahead, so the watchdog that
    /// ends it isn't waited for in real time.
    pub fn eval_skipping(&mut self, expr: &str, ms: u64) -> String {
        const UNDER_WAY: &str = "-- the call is under way --";
        let t0 = Instant::now();
        let code = format!("print('{UNDER_WAY}') {expr}");
        let mut out: Vec<String> = Vec::new();
        let mut skipped = false;
        let device = self.device();
        let Some(sim) = self.sim.as_mut() else { panic!("eval_skipping: the simulator isn't running") };
        let result = device.and_then(|mut dev| {
            dev.eval(&code, &mut |m| match m {
                luaboard::Msg::Log(s) if s == UNDER_WAY => {
                    skipped = sim.ctl.write_all(format!("skip={ms}\n").as_bytes()).is_ok();
                }
                luaboard::Msg::Log(s) => out.push(s),
                _ => {}
            })
        });
        if skipped {
            let _ = sim.acks.recv_timeout(Duration::from_secs(30));
        }
        match result {
            Ok(r) if r.is_empty() => {}
            Ok(r) => out.push(r),
            Err(e) => out.push(format!("Error: {e:?}")),
        }
        let text = chomp(out.join("\n"));
        self.sh.spent("dev", t0);
        self.log_cli(&["(in this program) eval, skipping", expr], t0, true, &text);
        text
    }
    /// A field of the device's INFO reply ("slot=ota_0" -> info("slot") =
    /// "ota_0"), "" if it has none or doesn't answer.
    pub fn info(&self, key: &str) -> String {
        let t0 = Instant::now();
        let reply = self.device().and_then(|mut dev| dev.info(&mut |_| {})).unwrap_or_default();
        self.sh.spent("dev", t0);
        let value =
            reply.split_whitespace().find_map(|kv| kv.split_once('=').filter(|(k, _)| *k == key).map(|(_, v)| v));
        value.unwrap_or("").to_string()
    }
    /// Until EXPR evaluates to true on the device.
    pub fn wait_lua(&mut self, secs: f64, expr: &str) -> bool {
        self.until(secs, |t| t.eval(expr) == "true")
    }

    /// The CLI in the background (luaboard -p TTY ARGS...), its output and
    /// errors to `out` in the work folder; stopped at the end of the section.
    pub fn e_bg(&mut self, out: &str, args: &[&str]) -> u32 {
        let mut all = vec!["-p", &self.tty];
        all.extend_from_slice(args);
        let f = fs::File::create(self.work.join(out)).unwrap();
        let child = Command::new(&self.cli)
            .args(&all)
            .env("LUABOARD_KEYS", &self.keys)
            .stdin(Stdio::null())
            .stdout(f.try_clone().unwrap())
            .stderr(f)
            .spawn()
            .unwrap();
        let pid = child.id();
        self.sh.add_pid(pid);
        self.bg.push(child);
        pid
    }
    /// Wait (up to `secs`) for the background CLI with this pid to end.
    pub fn wait_bg(&mut self, pid: u32, secs: f64) -> bool {
        let Some(i) = self.bg.iter().position(|c| c.id() == pid) else { return true };
        let done = wait_timeout(&mut self.bg[i], Duration::from_secs_f64(secs));
        if done {
            let _ = self.bg.remove(i).wait();
            self.sh.remove_pid(pid);
        }
        done
    }

    /// Protocol lines in one write, as they are (luaboard raw), and the reply
    /// lines (OK/ERR, not LOG/EVT), one per line. It ends when there is a
    /// reply for every line (the pixels after an FB reply included); a line
    /// the device doesn't answer costs the half second the CLI then waits for
    /// the link to go quiet.
    pub fn raw(&self, lines: &[&str]) -> String {
        let mut input = String::new();
        for l in lines {
            input.push_str(l);
            input.push('\n');
        }
        let t0 = Instant::now();
        let mut child = Command::new(&self.cli)
            .args(["-p", &self.tty, "--linger", "0.5", "raw"])
            .env("LUABOARD_KEYS", &self.keys)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .unwrap();
        self.sh.add_pid(child.id());
        let mut stdin = child.stdin.take();
        if let Some(s) = stdin.as_mut() {
            let _ = s.write_all(input.as_bytes()).and_then(|_| s.flush());
        }
        let mut stdout = child.stdout.take().unwrap();
        let (tx, rx) = mpsc::channel::<Vec<u8>>();
        std::thread::spawn(move || {
            let mut buf = [0u8; 8192];
            while let Ok(n) = stdout.read(&mut buf) {
                if n == 0 || tx.send(buf[..n].to_vec()).is_err() {
                    break;
                }
            }
        });
        let mut out = Vec::new();
        let replies = loop {
            let (replies, whole) = raw_replies(&out);
            if replies.len() >= lines.len() && whole {
                break replies;
            }
            match rx.recv_timeout(Duration::from_millis(500)) {
                Ok(chunk) => out.extend_from_slice(&chunk),
                Err(mpsc::RecvTimeoutError::Timeout) if stdin.is_some() => stdin = None, // the CLI ends once it's quiet
                Err(mpsc::RecvTimeoutError::Timeout) if Instant::now() - t0 < CLI_LIMIT => {}
                Err(_) => break raw_replies(&out).0,
            }
        };
        let _ = child.kill();
        let _ = child.wait();
        self.sh.remove_pid(child.id());
        self.sh.spent("cli raw", t0);
        replies.join("\n")
    }

    /// The login of `luaboard browser`, with this test executable as the
    /// browser: it fetches the device's address and follows the redirects.
    pub fn browse(&self) -> String {
        let me = std::env::current_exe().unwrap();
        let browser = format!("{} --browse", me.display());
        self.e_env(&[("LUABOARD_BROWSER", &browser)], &["browser", "--wait", "5"])
    }

    /// The screen, saved as `name` in the work folder: the simulator's LCD
    /// written by the simulator (ppm= on the control channel), between two
    /// passes of the firmware's loop. `e(&["screenshot", ..])` is the CLI's
    /// way, the frame over the serial link: a hundred and fifty round trips
    /// through the pseudo-terminal, which a busy computer makes slow.
    pub fn shot(&self, name: &str) -> Ppm {
        let p = self.path(name);
        let _ = fs::remove_file(&p);
        let t0 = Instant::now();
        let line = format!("ppm={p}");
        let Some(s) = self.sim.as_ref() else { panic!("simulator not running for control line {line}") };
        if (&s.ctl).write_all(format!("{line}\n").as_bytes()).is_err() {
            panic!("simulator not running for control line {line}");
        }
        // (no answer: panic as ctl does, or the late one would be taken for
        // the next line's)
        match s.acks.recv_timeout(Duration::from_secs(30)) {
            Ok(ack) if ack == format!("ok {line}") => {}
            Ok(ack) => self.bad(&format!("screen to {name}: {ack}")),
            Err(e) => panic!("simulator did not answer control line {line} ({e})"),
        }
        self.sh.spent("ctl", t0);
        Ppm::load(Path::new(&p))
    }

    // ---- waiting ---------------------------------------------------------------

    /// Retry `f` for `secs` of real time; false if it never holds. The
    /// device's clock runs at 20 times real time meanwhile (20 ms ahead,
    /// then a real millisecond): what is waited for is mostly the device
    /// getting on with something by its clock (an app's frames, 30 a second;
    /// its steps through a request), while a server on this computer does
    /// its part in real time, which the device's timeouts still leave room
    /// for. `until_real` is the same in real time only.
    pub fn until(&mut self, secs: f64, mut f: impl FnMut(&T) -> bool) -> bool {
        let deadline = Instant::now() + Duration::from_secs_f64(secs);
        let tries = (secs * 50.0) as usize;
        let mut i = 0;
        while i < tries || Instant::now() < deadline {
            if f(self) {
                return true;
            }
            if self.sim_running() {
                self.advance(20);
            }
            let t0 = Instant::now();
            std::thread::sleep(Duration::from_millis(1));
            self.sh.spent("wait", t0);
            i += 1;
        }
        f(self)
    }
    /// Retry `f` every 5 ms for `secs`; false if it never holds. The device's
    /// clock runs in real time.
    pub fn until_real(&self, secs: f64, mut f: impl FnMut(&T) -> bool) -> bool {
        let deadline = Instant::now() + Duration::from_secs_f64(secs);
        let tries = (secs * 50.0) as usize;
        let mut i = 0;
        while i < tries || Instant::now() < deadline {
            if f(self) {
                return true;
            }
            let t0 = Instant::now();
            std::thread::sleep(Duration::from_millis(5));
            self.sh.spent("wait", t0);
            i += 1;
        }
        f(self)
    }
    /// until for what the device does by its own clock (timers, back-off,
    /// a sound that ends): the clock runs 100 ms ahead between checks, so
    /// SECS of the device's time take no real time to speak of.
    pub fn until_sim(&mut self, secs: f64, f: impl FnMut(&T) -> bool) -> bool {
        self.until_paced(secs, 0, f)
    }
    /// until_sim for what a server on this computer has a part in (a
    /// download, a broker's reply): a real 5 ms between the steps, so the
    /// device's clock runs at 20 times real time and no faster. Its timeouts
    /// for the server (seconds) are then still tens of real milliseconds.
    pub fn until_net(&mut self, secs: f64, f: impl FnMut(&T) -> bool) -> bool {
        self.until_paced(secs, 5, f)
    }
    fn until_paced(&mut self, secs: f64, pause_ms: u64, mut f: impl FnMut(&T) -> bool) -> bool {
        let tries = (secs * 10.0) as usize;
        for _ in 0..tries {
            if f(self) {
                return true;
            }
            self.advance(100);
            if pause_ms > 0 {
                let t0 = Instant::now();
                std::thread::sleep(Duration::from_millis(pause_ms));
                self.sh.spent("wait", t0);
            }
        }
        // The device has had its time. What is looked at may still be on its
        // way through another program (a monitor's output file, a server's
        // log): half a second of real time for that, used only when failing.
        self.until_real(0.5, f)
    }
    /// Wi-Fi joins (the network was set before): takes the device's time,
    /// not ours.
    pub fn wifi_up(&mut self) -> bool {
        self.until_sim(5.0, |t| !matches!(t.info("ip").as_str(), "" | "-"))
    }
    pub fn wait_port(&self, port: u16) -> bool {
        self.until_real(5.0, |_| port_open(port))
    }
}

impl T {
    /// Did a listener fail because its port was taken ("address in use")?
    /// Ports the tests pick ahead (free_port) can be taken by something else
    /// before they're bound; such a section is run again once.
    pub fn port_clash(&self) -> bool {
        let Ok(dir) = fs::read_dir(&self.work) else { return false };
        dir.filter_map(|e| e.ok()).filter(|e| e.path().is_file()).any(|e| {
            let text = String::from_utf8_lossy(&fs::read(e.path()).unwrap_or_default()).to_lowercase();
            ["address already in use", "os error 48)", "os error 98)", "os error 10048)", "eaddrinuse"]
                .iter()
                .any(|p| text.contains(p))
        })
    }
}

impl Drop for T {
    fn drop(&mut self) {
        for mut c in self.bg.drain(..) {
            let _ = c.kill();
            let _ = c.wait();
        }
        if let Some(mut s) = self.sim.take() {
            let _ = s.child.kill();
            let _ = s.child.wait();
        }
        if self.failed() && std::env::var_os("E2E_KEEP").is_some() {
            self.say(&format!("  (work folder kept: {})", self.work.display()));
        } else {
            let _ = fs::remove_dir_all(&self.work);
        }
    }
}

fn make_work_dir() -> PathBuf {
    let tmp = std::env::temp_dir();
    loop {
        let mut b = [0u8; 3];
        getrandom::fill(&mut b).unwrap();
        let p = tmp.join(format!("luaboard-e2e.{:02x}{:02x}{:02x}", b[0], b[1], b[2]));
        if fs::create_dir(&p).is_ok() {
            // canonical: macOS's /var is a link to /private/var (not on
            // Windows, where it would be a \\?\ path)
            if WINDOWS {
                return p;
            }
            return p.canonicalize().unwrap_or(p);
        }
    }
}

/// The serial port of a simulator working in folder `work`: a link in it, on
/// Windows a named pipe named after it.
pub fn tty_in(work: &Path) -> String {
    if WINDOWS {
        let name: String = work
            .to_string_lossy()
            .chars()
            .map(|c| if c.is_ascii_alphanumeric() || c == '.' || c == '-' { c } else { '_' })
            .collect();
        format!("//./pipe/luaboard-{name}")
    } else {
        work.join("tty").to_string_lossy().into_owned()
    }
}

pub fn wait_timeout(child: &mut Child, d: Duration) -> bool {
    let deadline = Instant::now() + d;
    loop {
        match child.try_wait() {
            Ok(Some(_)) | Err(_) => return true,
            Ok(None) if Instant::now() >= deadline => return false,
            Ok(None) => std::thread::sleep(Duration::from_millis(1)),
        }
    }
}

/// A CLI call may take this long before it counts as hung.
const CLI_LIMIT: Duration = Duration::from_secs(60);

/// Run `cmd` with its output and errors in one pipe (in the order written),
/// `input` on its standard input; the text without trailing newlines.
fn run_merged(cmd: &mut Command, input: Option<&[u8]>, sh: &Shared) -> (bool, String) {
    let (mut r, w) = std::io::pipe().unwrap();
    cmd.stdout(w.try_clone().unwrap()).stderr(w);
    cmd.stdin(if input.is_some() { Stdio::piped() } else { Stdio::null() });
    let (ok, out) = run_reading(cmd, input, sh, move || {
        let mut buf = Vec::new();
        let _ = r.read_to_end(&mut buf);
        buf
    });
    (ok, chomp(String::from_utf8_lossy(&out).into_owned()))
}

/// The reply lines (OK/ERR) in what `luaboard raw` printed so far, and
/// whether the last reply is whole: after "OK <w> <h> rgb565 <bytes> <crc>"
/// (FB) come that many bytes of pixels, which are no lines.
fn raw_replies(out: &[u8]) -> (Vec<String>, bool) {
    let mut replies = Vec::new();
    let mut pos = 0;
    while let Some(n) = out[pos..].iter().position(|&b| b == b'\n') {
        let line = String::from_utf8_lossy(&out[pos..pos + n]).trim_end_matches('\r').to_string();
        pos += n + 1;
        if !(line.starts_with("OK") || line.starts_with("ERR")) {
            continue;
        }
        let w: Vec<&str> = line.split_whitespace().collect();
        let pixels = if w.len() == 6 && w[3] == "rgb565" { w[4].parse::<usize>().unwrap_or(0) } else { 0 };
        replies.push(line);
        if out.len() < pos + pixels {
            return (replies, false);
        }
        pos += pixels;
    }
    (replies, true)
}

fn run_reading(
    cmd: &mut Command,
    input: Option<&[u8]>,
    sh: &Shared,
    read: impl FnOnce() -> Vec<u8> + Send + 'static,
) -> (bool, Vec<u8>) {
    let t0 = Instant::now();
    let mut child = cmd.spawn().unwrap();
    // The command holds the pipe's write end: drop ours, or the read never ends.
    drop(std::mem::replace(cmd, Command::new("")));
    sh.add_pid(child.id());
    if let (Some(data), Some(mut stdin)) = (input, child.stdin.take()) {
        let data = data.to_vec();
        std::thread::spawn(move || {
            let _ = stdin.write_all(&data);
        });
    }
    let out = read_all(&mut child, read);
    let ok = child.wait().is_ok_and(|s| s.success());
    sh.remove_pid(child.id());
    sh.spent("cli", t0);
    (ok, out)
}

/// What `read` reads of the child's output, to its end: the child has
/// exited then, so nothing waits in steps for it. A child that takes longer
/// than CLI_LIMIT is killed.
fn read_all(child: &mut Child, read: impl FnOnce() -> Vec<u8> + Send + 'static) -> Vec<u8> {
    let (tx, rx) = mpsc::channel();
    std::thread::spawn(move || {
        let _ = tx.send(read());
    });
    match rx.recv_timeout(CLI_LIMIT) {
        Ok(out) => out,
        Err(_) => {
            let _ = child.kill();
            rx.recv_timeout(Duration::from_secs(5)).unwrap_or_default()
        }
    }
}
