//! The serial worker thread: talks to the device for the UI (Cmd in, Ev out).

use super::*;

/// A project's secrets file: (name, value).
pub(crate) type Secrets = Vec<(String, String)>;

/// The device's clock as of `at` (TIME reply).
#[derive(Clone)]
pub(crate) struct DeviceClock {
    /// UTC seconds, None until the device has the time from the network.
    pub(crate) utc: Option<i64>,
    /// Seconds east of UTC (from the local time's +hh:mm).
    pub(crate) offset: i64,
    /// POSIX TZ string.
    pub(crate) tz: String,
    /// "ntp", "rtc" (the board's clock chip, NTP not yet) or "-".
    pub(crate) source: String,
    pub(crate) at: Instant,
}

impl DeviceClock {
    pub(crate) fn parse(reply: &str) -> Self {
        let get = |k: &str| luaboard::kv_field(reply, k).unwrap_or("-");
        let local = get("local");
        // "2026-11-06T15:54:41+02:00": the offset is the last 6 characters.
        let offset = (local.len() >= 6)
            .then(|| &local[local.len() - 6..])
            .and_then(|o| {
                let sign = if o.starts_with('-') { -1 } else { 1 };
                let h: i64 = o.get(1..3)?.parse().ok()?;
                let m: i64 = o.get(4..6)?.parse().ok()?;
                Some(sign * (h * 3600 + m * 60))
            })
            .unwrap_or(0);
        DeviceClock {
            utc: get("utc").parse().ok(),
            offset,
            tz: get("tz").to_string(),
            source: get("source").to_string(),
            at: Instant::now(),
        }
    }

    /// The device's local time now, "2026-11-06 15:54:41".
    pub(crate) fn now_local(&self) -> Option<String> {
        let t = self.utc? + self.at.elapsed().as_secs() as i64 + self.offset;
        let (y, m, d) = luaboard::timezone::civil(t.div_euclid(86400));
        let secs = t.rem_euclid(86400);
        Some(format!("{y:04}-{m:02}-{d:02} {:02}:{:02}:{:02}", secs / 3600, secs % 3600 / 60, secs % 60))
    }
}

/// Requests from the UI to the serial worker thread.
pub(crate) enum Cmd {
    Connect(String),
    /// Connect to this port once it's there (a simulator started later, a
    /// board plugged in later), retrying quietly like after a lost link.
    Await(String),
    Disconnect,
    /// Store `secrets`, upload `files` (bare names) to RAM as app `app`, then run `<app>/<run>`.
    Upload {
        app: String,
        files: Vec<(String, Vec<u8>)>,
        secrets: Secrets,
        run: Option<String>,
    },
    /// Store `secrets`, upload `files` as app `app`, drop the app's other device files, write it to flash.
    Install {
        app: String,
        files: Vec<(String, Vec<u8>)>,
        secrets: Secrets,
    },
    /// Write an app's RAM uploads on the device to flash.
    InstallApp(String),
    Uninstall(String),
    /// Autostart app: Some(app) or None (boot into the menu).
    Autorun(Option<String>),
    /// Ask for the device's clock and time zone.
    Time,
    /// Set the device's time zone (a POSIX TZ string).
    SetTz(String),
    Run(Option<String>),
    Stop,
    /// Refresh the device's files, apps and autostart.
    List,
    Remove(String),
    Pull(String),
    Eval(String),
    /// The last crash's record, into the console.
    Crash,
    /// The framebuffer, unless it still has this CRC.
    Framebuffer(Option<u32>),
    /// A finger from here: down/moved at (x, y) in the touch area, or let go.
    Touch(Option<(u32, u32)>),
    /// A short press of the power key (home).
    PowerKey,
    /// Flash a firmware image to the device on `port`, then reconnect.
    Flash {
        port: String,
        image: Vec<u8>,
    },
    /// Update the firmware over the open link (OTA), then reconnect.
    Update(ota::UpdateImage),
    WifiStatus,
    WifiSet {
        ssid: String,
        password: String,
    },
    WifiOff,
    /// Pair this computer for Wi-Fi (USB only); `new`: a new key.
    Pair(bool),
    /// Unpair: the device takes no commands over Wi-Fi (USB only).
    Unpair,
    /// Ask for a pending browser-login request (after connecting).
    BrowserPending,
    /// Hand a browser login's redirect query to the device.
    BrowserAnswer(u32, String),
    BrowserCancel(u32),
}

/// Events from the worker back to the UI.
pub(crate) enum Ev {
    /// Connected to (port, device INFO).
    Connected(String, String),
    Disconnected(String),
    Device(Msg),
    Info(String),
    Error(String),
    Files(Vec<FileInfo>),
    Apps(Vec<AppInfo>),
    /// The autostart app (None: the device boots into its menu).
    Autorun(Option<String>),
    Clock(DeviceClock),
    Pulled(String, Vec<u8>),
    /// A new frame, or None: unchanged.
    Framebuffer(Option<Frame>),
    FramebufferError(String),
    /// A Touch / PowerKey went through (or why not).
    TouchDone(Result<(), String>),
    Flash(FlashEvent),
    FlashFinished(Result<(), String>),
    Ota(ota::Event),
    /// The update is done: Ok(the INFO after it) or why not.
    OtaFinished(Result<String, String>),
    Wifi(luaboard::wifi::Status),
    /// The device on `port` can't be used over Wi-Fi from here: not paired.
    NotPaired {
        port: String,
        error: String,
    },
    PairStatus(luaboard::PairState),
    /// A script waits for a browser login (None: nothing pending).
    Browser(Option<login::Request>),
}

pub(crate) enum ConnectError {
    NotPaired(String),
    Other(String),
}

/// Open `port` and fetch its info. Over Wi-Fi this needs the pairing key
/// (luaboard::keys); without it, NotPaired.
pub(crate) fn connect(port: &str) -> Result<(Device, String), ConnectError> {
    let err = |e: anyhow::Error| {
        let text = format!("{e:#}");
        if luaboard::wifi::not_paired(&e) {
            ConnectError::NotPaired(text.replace("device: ", ""))
        } else {
            ConnectError::Other(text)
        }
    };
    let mut d = Device::open(port, DEFAULT_BAUD).map_err(err)?;
    let info = d.info(&mut |_| {}).map_err(err)?;
    Ok((d, info))
}

/// "AUTORUN <app>|off" -> the autostart app.
pub(crate) fn parse_autorun(reply: &str) -> Option<String> {
    let v = reply.strip_prefix("AUTORUN").unwrap_or(reply).trim();
    (!v.is_empty() && v != "off" && v != "-").then(|| v.to_string())
}

/// Log the secrets a project stored on the device (names, never values).
pub(crate) fn tell_secrets(app: &str, secrets: &Secrets, send: &dyn Fn(Ev)) {
    for (name, _) in secrets {
        send(Ev::Info(format!("secret {app} {name} (from {})", luaboard::SECRETS_FILE)));
    }
}

/// Send the device's files, apps, autostart app and clock to the UI.
pub(crate) fn refresh(d: &mut Device, send: &dyn Fn(Ev), a: &mut dyn FnMut(Msg)) -> anyhow::Result<()> {
    send(Ev::Files(d.list(a)?));
    send(Ev::Apps(d.apps(a)?));
    send(Ev::Autorun(parse_autorun(&d.autorun(None, a)?)));
    send(Ev::Clock(DeviceClock::parse(&d.time(a)?)));
    Ok(())
}

/// A device connected: tell the UI, and send it the device's lists.
fn adopt(port: &str, info: String, mut d: Device, send: &dyn Fn(Ev)) -> Device {
    send(Ev::Connected(port.to_string(), info));
    let _ = refresh(&mut d, send, &mut |_| {});
    d
}

pub(crate) fn worker(rx: Receiver<Cmd>, tx: Sender<Ev>, ctx: egui::Context) {
    let mut dev: Option<Device> = None;
    // The port the user asked for. Kept after a link loss so we reconnect when
    // it comes back; cleared only by an explicit Disconnect.
    let mut want: Option<String> = None;
    let mut next_retry = std::time::Instant::now();
    // Apps whose kept-secrets note was shown on Upload: once is enough, as
    // Upload & Run is the edit-and-try loop.
    let mut noted: std::collections::HashSet<String> = Default::default();
    let send = |ev: Ev| {
        let _ = tx.send(ev);
        ctx.request_repaint();
    };

    loop {
        let cmd = match rx.recv_timeout(Duration::from_millis(15)) {
            Ok(c) => Some(c),
            Err(RecvTimeoutError::Timeout) => None,
            Err(RecvTimeoutError::Disconnected) => return,
        };

        let Some(cmd) = cmd else {
            if let Some(d) = dev.as_mut() {
                // Idle: forward asynchronous device output (LOG / EVT).
                match d.poll(Duration::from_millis(30)) {
                    Ok(msgs) => msgs.into_iter().for_each(|m| send(Ev::Device(m))),
                    Err(e) => {
                        dev = None;
                        next_retry = std::time::Instant::now() + Duration::from_secs(1);
                        send(Ev::Disconnected(format!(
                            "connection lost ({e:#}), waiting for the device to come back…"
                        )));
                    }
                }
            } else if let Some(port) = want.as_ref().filter(|_| std::time::Instant::now() >= next_retry) {
                // Lost link: quietly retry, e.g. until a new simulator instance appears.
                next_retry = std::time::Instant::now() + Duration::from_secs(1);
                if luaboard::wifi::parse_tcp_port(port).is_some() || std::path::Path::new(port).exists() {
                    match connect(port) {
                        Ok((d, info)) => dev = Some(adopt(port, info, d, &send)),
                        Err(ConnectError::NotPaired(error)) => {
                            // Paired anew or unpaired meanwhile: stop retrying, say so.
                            send(Ev::NotPaired { port: port.clone(), error });
                            want = None;
                        }
                        Err(ConnectError::Other(_)) => {}
                    }
                }
            }
            continue;
        };

        if let Cmd::Connect(path) = &cmd {
            dev = None;
            match connect(path) {
                Ok((d, info)) => {
                    dev = Some(adopt(path, info, d, &send));
                    want = Some(path.clone()); // reconnect to it if the link is lost later
                }
                Err(ConnectError::NotPaired(error)) => {
                    want = None;
                    send(Ev::NotPaired { port: path.clone(), error });
                }
                Err(ConnectError::Other(e)) => {
                    want = None; // a failed explicit connect is not retried in the background
                    send(Ev::Disconnected(e));
                }
            }
            continue;
        }
        if let Cmd::Await(path) = cmd {
            want = Some(path);
            next_retry = std::time::Instant::now();
            continue;
        }
        if let Cmd::Disconnect = cmd {
            dev = None;
            want = None;
            send(Ev::Disconnected("disconnected".into()));
            continue;
        }
        if let Cmd::Flash { port, image } = cmd {
            // The bootloader needs the port to itself: close our session first.
            if dev.take().is_some() {
                send(Ev::Disconnected("disconnected for flashing".into()));
            }
            let result = firmware::flash(&port, &image, &mut |ev| send(Ev::Flash(ev)));
            let ok = result.is_ok();
            send(Ev::FlashFinished(result.map_err(|e| format!("{e:#}"))));
            if ok {
                // The chip resets and its USB port re-enumerates; reconnect when it's back.
                for _ in 0..15 {
                    thread::sleep(Duration::from_secs(1));
                    if let Ok(mut d) = Device::open(&port, DEFAULT_BAUD) {
                        let info = d.info(&mut |_| {}).unwrap_or_default();
                        dev = Some(adopt(&port, info, d, &send));
                        break;
                    }
                }
                if dev.is_none() {
                    send(Ev::Error(format!("flashed, but {port} did not come back; reconnect manually")));
                }
            }
            continue;
        }

        if let Cmd::Update(img) = cmd {
            let (Some(d), Some(port)) = (dev.take(), want.clone()) else {
                send(Ev::OtaFinished(Err("not connected".into())));
                continue;
            };
            let mut fwd = |m: Msg| send(Ev::Device(m));
            let result = ota::update(d, &port, DEFAULT_BAUD, &img, &mut |ev| send(Ev::Ota(ev)), &mut fwd);
            match result {
                Ok((d, info)) => {
                    send(Ev::OtaFinished(Ok(info.clone())));
                    dev = Some(adopt(&port, info, d, &send));
                }
                // The link is closed now; the reconnect above picks the device up again.
                Err(e) => send(Ev::OtaFinished(Err(format!("{e:#}")))),
            }
            next_retry = std::time::Instant::now();
            continue;
        }

        let Some(d) = dev.as_mut() else {
            // Preview polls queued just before a disconnect/switch are internal: drop quietly.
            if !matches!(cmd, Cmd::Framebuffer(_) | Cmd::Touch(_) | Cmd::PowerKey) {
                send(Ev::Error("not connected".into()));
            }
            continue;
        };
        let mut fwd = |m: Msg| send(Ev::Device(m));
        let result: anyhow::Result<()> = (|| {
            match cmd {
                Cmd::Upload { app, files, secrets, run } => {
                    // (the note once per app: uploads come one after another)
                    let note = d.push_secrets(&app, &secrets, &mut fwd)?;
                    if let Some(note) = note.filter(|_| noted.insert(app.clone())) {
                        send(Ev::Info(note));
                    }
                    tell_secrets(&app, &secrets, &send);
                    for name in d.put_project(&app, &files, &mut fwd)? {
                        let size = files.iter().find(|(n, _)| *n == name).map_or(0, |(_, d)| d.len());
                        send(Ev::Info(format!("uploaded {app}/{name} ({size} B)")));
                    }
                    refresh(d, &send, &mut fwd)?;
                    if let Some(entry) = run {
                        send(Ev::Info(d.run(Some(&format!("{app}/{entry}")), &mut fwd)?));
                    }
                }
                Cmd::Install { app, files, secrets } => {
                    if let Some(note) = d.push_secrets(&app, &secrets, &mut fwd)? {
                        send(Ev::Info(note));
                    }
                    tell_secrets(&app, &secrets, &send);
                    let r = d.install_project(&app, &files, &mut fwd);
                    // Refresh even after a failure: some files may have changed.
                    let _ = refresh(d, &send, &mut fwd);
                    send(Ev::Info(format!("{} ({} file(s), in flash now)", r?, files.len())));
                }
                Cmd::InstallApp(app) => {
                    let r = d.install(&app, &mut fwd);
                    let _ = refresh(d, &send, &mut fwd);
                    send(Ev::Info(r?));
                }
                Cmd::Uninstall(app) => {
                    let r = d.uninstall(&app, &mut fwd);
                    let _ = refresh(d, &send, &mut fwd);
                    send(Ev::Info(r?));
                }
                Cmd::Autorun(app) => {
                    let r = d.autorun(Some(app.as_deref().unwrap_or("off")), &mut fwd)?;
                    send(Ev::Autorun(parse_autorun(&r)));
                    send(Ev::Info(match parse_autorun(&r) {
                        Some(a) => format!("autostart: {a} (instead of the menu)"),
                        None => "autostart off: the device boots into its menu".into(),
                    }));
                }
                Cmd::Time => send(Ev::Clock(DeviceClock::parse(&d.time(&mut fwd)?))),
                Cmd::SetTz(tz) => {
                    let tz = d.timezone(Some(&tz), &mut fwd)?;
                    send(Ev::Info(format!(
                        "time zone: {}",
                        luaboard::timezone::name_of(&tz).map_or(tz.clone(), |n| format!("{n} ({tz})"))
                    )));
                    send(Ev::Clock(DeviceClock::parse(&d.time(&mut fwd)?)));
                }
                Cmd::Run(name) => send(Ev::Info(d.run(name.as_deref(), &mut fwd)?)),
                Cmd::Stop => {
                    d.stop(&mut fwd)?;
                    send(Ev::Info("stopped".into()));
                }
                Cmd::List => refresh(d, &send, &mut fwd)?,
                Cmd::Remove(name) => {
                    d.remove(&name, &mut fwd)?;
                    send(Ev::Info(format!("removed {name}")));
                    refresh(d, &send, &mut fwd)?;
                }
                Cmd::Pull(name) => {
                    let data = d.get(&name, &mut fwd)?;
                    send(Ev::Pulled(name, data));
                }
                Cmd::WifiStatus => {
                    send(Ev::Wifi(d.wifi_status(&mut fwd)?));
                    send(Ev::PairStatus(d.pair_status(&mut fwd)?));
                }
                Cmd::Pair(new) => {
                    let id = d.pair(new, &mut fwd)?;
                    send(Ev::Info(format!("paired for Wi-Fi: key {id} saved in {}", luaboard::keys::path().display())));
                    send(Ev::PairStatus(d.pair_status(&mut fwd)?));
                }
                Cmd::Unpair => {
                    d.unpair(&mut fwd)?;
                    send(Ev::Info("unpaired: the device takes no commands over Wi-Fi".into()));
                    send(Ev::PairStatus(d.pair_status(&mut fwd)?));
                }
                Cmd::WifiSet { ssid, password } => {
                    let st = d.wifi_set(&ssid, &password, &mut fwd)?;
                    send(Ev::Info(format!("Wi-Fi: joining \"{ssid}\"")));
                    send(Ev::Wifi(st));
                }
                Cmd::BrowserPending => send(Ev::Browser(d.browser_pending(&mut fwd)?)),
                Cmd::BrowserAnswer(id, query) => {
                    d.browser_answer(id, &query, &mut fwd)?;
                    send(Ev::Info("login handed to the device".into()));
                }
                Cmd::BrowserCancel(id) => {
                    // Already over (answered elsewhere, script ended).
                    let _ = d.browser_cancel(id, &mut fwd);
                }
                Cmd::WifiOff => {
                    send(Ev::Wifi(d.wifi_off(&mut fwd)?));
                    send(Ev::Info("Wi-Fi turned off".into()));
                }
                Cmd::Eval(code) => {
                    let r = d.eval(&code, &mut fwd)?;
                    send(Ev::Info(format!("= {r}")));
                }
                Cmd::Crash => {
                    if let Some(c) = d.crash(&mut fwd)? {
                        luaboard::crash::describe(&c, None).into_iter().for_each(|l| send(Ev::Error(l)));
                    }
                }
                Cmd::Framebuffer(last) => match d.framebuffer_if_changed(last, &mut fwd) {
                    Ok(fb) => send(Ev::Framebuffer(fb)),
                    Err(e) if luaboard::is_link_error(&e) => return Err(e),
                    Err(e) => send(Ev::FramebufferError(format!("{e:#}"))),
                },
                Cmd::Touch(at) => match d.touch(at, &mut fwd) {
                    Ok(()) => send(Ev::TouchDone(Ok(()))),
                    Err(e) if luaboard::is_link_error(&e) => return Err(e),
                    Err(e) => send(Ev::TouchDone(Err(format!("{e:#}")))),
                },
                Cmd::PowerKey => match d.key_power(&mut fwd) {
                    Ok(()) => send(Ev::TouchDone(Ok(()))),
                    Err(e) if luaboard::is_link_error(&e) => return Err(e),
                    Err(e) => send(Ev::TouchDone(Err(format!("{e:#}")))),
                },
                Cmd::Connect(_) | Cmd::Await(_) | Cmd::Disconnect | Cmd::Flash { .. } | Cmd::Update(_) => {
                    unreachable!()
                }
            }
            Ok(())
        })();
        if let Err(e) = result {
            if luaboard::is_link_error(&e) {
                // The handle is dead (e.g. the simulator was closed): drop it so the
                // reconnect logic can pick up the device / a new simulator.
                dev = None;
                next_retry = std::time::Instant::now();
                send(Ev::Disconnected(format!("connection lost ({e:#}), waiting for the device to come back…")));
            } else {
                send(Ev::Error(format!("{e:#}")));
            }
        }
    }
}
