//! What the worker reports, applied to the UI's state.

use super::*;

impl Studio {
    /// The device is gone, or another one comes: nothing of its stays shown.
    fn link_down(&mut self) {
        self.connected = false;
        self.connected_port = None;
        self.preview.pending = false;
        self.preview.remote = Remote::default();
        self.device_files.clear();
        self.device_apps.clear();
        self.autostart = None;
    }

    pub(crate) fn handle_events(&mut self) {
        while let Ok(ev) = self.rx.try_recv() {
            match ev {
                Ev::Connected(port, info) => {
                    self.link_down();
                    self.connected = true;
                    self.preview.crc = None;
                    self.log(LineKind::Info, format!("connected to {port}"));
                    self.connected_port = Some(port);
                    if luaboard::kv_field(&info, "reset").is_some_and(luaboard::crash::is_crash_reset) {
                        self.send(Cmd::Crash); // the device restarted after a crash: what it left
                    }
                    self.device_info = info;
                    self.send(Cmd::BrowserPending);
                }
                Ev::Browser(req) => self.set_login(req),
                Ev::Disconnected(why) => {
                    self.link_down();
                    self.log(LineKind::Error, why);
                }
                Ev::Device(Msg::Log(s)) => self.log(LineKind::Log, s),
                Ev::Device(Msg::Evt(s)) if s.starts_with("browser") => {
                    self.log(LineKind::Event, format!("[evt] {}", s.split(' ').take(2).collect::<Vec<_>>().join(" ")));
                    if let Some(req) = login::parse_event(&s) {
                        self.set_login(Some(req));
                    } else if let Some(id) = login::done_event(&s) {
                        if self.login.as_ref().is_some_and(|l| l.req.id == id) {
                            self.set_login(None);
                        }
                    }
                }
                Ev::Device(Msg::Evt(s)) if s.starts_with("time ") => {
                    // "time synced ...": the device's clock is set now.
                    self.log(LineKind::Event, format!("[evt] {s}"));
                    self.send(Cmd::Time);
                }
                Ev::Device(Msg::Evt(s)) if s.starts_with("wifi ") => {
                    // "wifi connected ..." / "wifi lost ...": refresh the Wi-Fi window.
                    self.log(LineKind::Event, format!("[evt] {s}"));
                    if self.wifi.open {
                        self.send(Cmd::WifiStatus);
                    }
                }
                Ev::Device(Msg::Evt(s)) => {
                    let k = if s.starts_with("error") {
                        self.error_at = parse_error_location(&s).or(self.error_at.take());
                        LineKind::Error
                    } else {
                        LineKind::Event
                    };
                    self.log(k, format!("[evt] {s}"));
                }
                Ev::Device(m) => self.log(LineKind::Event, format!("{m:?}")),
                Ev::Info(s) => self.log(LineKind::Info, s),
                Ev::Error(s) => {
                    if let Some(loc) = parse_error_location(&s) {
                        self.error_at = Some(loc);
                    }
                    self.log(LineKind::Error, s);
                }
                Ev::Framebuffer(fb) => {
                    self.preview.pending = false;
                    if let Some(fb) = fb {
                        self.preview.crc = Some(fb.crc);
                        self.update_preview_texture(&fb);
                    }
                }
                Ev::TouchDone(r) => {
                    let rm = &mut self.preview.remote;
                    rm.in_flight = false;
                    if let Err(e) = r {
                        rm.queue.clear();
                        rm.down = None;
                        rm.key = None;
                        rm.mouse = false;
                        self.log(LineKind::Error, format!("touch: {e}"));
                    }
                }
                Ev::Flash(ev) => {
                    let f = &mut self.flash;
                    match ev {
                        FlashEvent::Connecting => f.status = "entering download mode…".into(),
                        FlashEvent::Connected { chip } => f.status = format!("connected to {chip} bootloader"),
                        FlashEvent::Writing { done, total } => {
                            f.progress = done as f32 / total.max(1) as f32;
                            f.status = format!("writing {} / {} KB", done / 1024, total / 1024);
                        }
                        FlashEvent::Verifying => f.status = "verifying…".into(),
                        FlashEvent::Done => {
                            f.progress = 1.0;
                            f.status = "done, device is rebooting into the new firmware".into();
                        }
                    }
                }
                Ev::Ota(ev) => {
                    let f = &mut self.flash;
                    match ev {
                        ota::Event::Sending { done, total } => {
                            f.progress = done as f32 / total.max(1) as f32;
                            f.status = format!("sending {} / {} KB", done / 1024, total / 1024);
                        }
                        ota::Event::Checking => f.status = "the device checks the image…".into(),
                        ota::Event::Installed(v) => {
                            f.progress = 1.0;
                            f.status = format!("installed {v}, the device reboots…");
                        }
                        ota::Event::Rebooted(info) => {
                            f.status = format!(
                                "running {} on {}",
                                luaboard::kv_field(&info, "ver").unwrap_or("?"),
                                luaboard::kv_field(&info, "slot").unwrap_or("?")
                            );
                        }
                    }
                }
                Ev::OtaFinished(r) => {
                    self.flash.busy = false;
                    match r {
                        Ok(info) => {
                            let ver = luaboard::kv_field(&info, "ver").unwrap_or("?").to_string();
                            self.flash.status = format!("done: luaboard {ver} runs");
                            self.log(LineKind::Info, format!("firmware updated to {ver}"));
                        }
                        Err(e) => {
                            self.flash.status = format!("failed: {e}");
                            self.log(LineKind::Error, format!("update failed: {e}"));
                        }
                    }
                }
                Ev::FlashFinished(r) => {
                    self.flash.busy = false;
                    match r {
                        Ok(()) => self.log(LineKind::Info, "firmware flashed, waiting for the device to come back"),
                        Err(e) => {
                            self.flash.status = format!("failed: {e}");
                            self.log(LineKind::Error, format!("flash failed: {e}"));
                        }
                    }
                }
                Ev::FramebufferError(e) => {
                    self.preview.pending = false;
                    self.preview.live = false;
                    self.log(LineKind::Error, format!("display preview off: {e}"));
                }
                Ev::Files(f) => self.device_files = f,
                Ev::Apps(a) => self.device_apps = a,
                Ev::Autorun(a) => self.autostart = a,
                Ev::Clock(c) => self.clock = Some(c),
                Ev::NotPaired { port, error } => {
                    self.link_down();
                    self.log(LineKind::Error, format!("{port}: {error}"));
                    self.not_paired = Some((port, error));
                }
                Ev::PairStatus(st) => self.wifi.pair = Some(st),
                Ev::Wifi(st) => {
                    if self.wifi.ssid.is_empty() {
                        if let Some(s) = &st.ssid {
                            self.wifi.ssid = s.clone();
                        }
                    }
                    self.wifi.status = Some(st);
                }
                Ev::Pulled(device_name, data) => {
                    // "<app>/<file>" goes into the project as "<file>". The name
                    // comes from the device: keep it a plain file name (no "..",
                    // no Windows "\\" separators) before it becomes a path.
                    let name = device_name.rsplit('/').next().unwrap_or(&device_name).to_string();
                    if !luaboard::is_plain_file_name(&name) {
                        self.log(LineKind::Error, format!("not pulling {device_name}: unusual file name"));
                        continue;
                    }
                    let text = String::from_utf8_lossy(&data).into_owned();
                    if let Some(i) = self.scripts.iter().position(|s| s.name == name) {
                        self.scripts[i].text = text;
                        self.scripts[i].dirty = true;
                        self.selected = i;
                    } else {
                        self.scripts.push(Script {
                            name: name.clone(),
                            path: None,
                            text,
                            dirty: true,
                            ..Default::default()
                        });
                        self.selected = self.scripts.len() - 1;
                    }
                    self.log(LineKind::Info, format!("pulled {device_name} from device into the project as {name}"));
                }
            }
        }
    }
}
