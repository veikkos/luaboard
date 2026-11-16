//! The Wi-Fi window: the device's Wi-Fi status and credentials.

use super::*;

/// The Wi-Fi window: device's Wi-Fi status and credentials.
#[derive(Default)]
pub(crate) struct WifiUi {
    pub(crate) open: bool,
    /// When the status was last requested (re-polled while "connecting").
    pub(crate) last_poll: Option<std::time::Instant>,
    pub(crate) pair: Option<luaboard::PairState>,
    pub(crate) ssid: String,
    pub(crate) password: String,
    pub(crate) status: Option<luaboard::wifi::Status>,
    pub(crate) requested: bool,
}

impl Studio {
    pub(crate) fn wifi_window(&mut self, ui: &mut egui::Ui) {
        if !self.connected {
            ui.weak("Connect to the device (USB or Wi-Fi) to see and change its Wi-Fi settings.");
            return;
        }
        // Ask once when the window opens, then every second while the device is
        // still joining (the reply to "Save & connect" is always "connecting").
        let joining = self.wifi.status.as_ref().is_some_and(|s| s.state == "connecting");
        let due = self.wifi.last_poll.is_none_or(|t| t.elapsed() >= Duration::from_secs(1));
        if !self.wifi.requested || (joining && due) {
            self.wifi.requested = true;
            self.wifi.last_poll = Some(std::time::Instant::now());
            self.send(Cmd::WifiStatus);
        }
        if joining {
            ui.ctx().request_repaint_after(Duration::from_millis(500));
        }
        egui::Grid::new("wifist").num_columns(2).spacing([12.0, 2.0]).show(ui, |ui| match &self.wifi.status {
            None => {
                ui.weak("status");
                ui.label("…");
                ui.end_row();
            }
            Some(st) => {
                ui.weak("status");
                let col = match st.state.as_str() {
                    "connected" => egui::Color32::from_rgb(80, 200, 120),
                    "connecting" => egui::Color32::from_rgb(230, 180, 90),
                    _ => ui.visuals().weak_text_color(),
                };
                ui.colored_label(col, &st.state);
                ui.end_row();
                ui.weak("network");
                ui.label(st.ssid.as_deref().unwrap_or("-"));
                ui.end_row();
                if let Some(ip) = &st.ip {
                    ui.weak("address");
                    ui.label(format!("{ip}  ({} dBm)", st.rssi));
                    ui.end_row();
                }
            }
        });
        ui.separator();
        egui::Grid::new("wificreds").num_columns(2).spacing([12.0, 4.0]).show(ui, |ui| {
            ui.label("Network");
            ui.add(egui::TextEdit::singleline(&mut self.wifi.ssid).desired_width(240.0).hint_text("SSID"));
            ui.end_row();
            ui.label("Password");
            ui.add(
                egui::TextEdit::singleline(&mut self.wifi.password)
                    .password(true)
                    .desired_width(240.0)
                    .hint_text("empty for an open network"),
            );
            ui.end_row();
        });
        ui.horizontal(|ui| {
            let valid = !self.wifi.ssid.is_empty() && self.wifi.ssid.len() <= 32 && self.wifi.password.len() <= 64;
            if ui.add_enabled(valid, egui::Button::new("Save & connect")).clicked() {
                let password = std::mem::take(&mut self.wifi.password); // don't keep it in memory
                self.send(Cmd::WifiSet { ssid: self.wifi.ssid.clone(), password });
            }
            if ui.button("Turn Wi-Fi off").clicked() {
                self.send(Cmd::WifiOff);
            }
            if ui.button("↻").on_hover_text("Refresh status").clicked() {
                self.send(Cmd::WifiStatus);
            }
        });

        // Pairing for Wi-Fi (changing it needs USB).
        ui.separator();
        let on_usb = luaboard::wifi::parse_tcp_port(&self.port).is_none();
        ui.horizontal(|ui| {
            ui.label("Wi-Fi pairing:");
            match &self.wifi.pair {
                Some(st) if st.on && st.here => {
                    ui.colored_label(egui::Color32::from_rgb(80, 200, 120), "paired with this computer")
                }
                Some(st) if st.on => {
                    ui.colored_label(egui::Color32::from_rgb(230, 180, 90), "paired, but not with this computer")
                }
                Some(_) => ui.colored_label(
                    egui::Color32::from_rgb(230, 180, 90),
                    "not paired: the Wi-Fi link takes no commands",
                ),
                None => ui.weak("…"),
            };
        });
        ui.add_enabled_ui(on_usb, |ui| {
            ui.horizontal(|ui| {
                if ui
                    .button("Pair this computer")
                    .on_hover_text(format!(
                        "Saves the device's key in {}; the Wi-Fi link is encrypted with it",
                        luaboard::keys::path().display()
                    ))
                    .clicked()
                {
                    self.send(Cmd::Pair(false));
                }
                if ui
                    .add_enabled(self.wifi.pair.as_ref().is_some_and(|p| p.on), egui::Button::new("New key"))
                    .on_hover_text("Every other paired computer has to pair again")
                    .clicked()
                {
                    self.send(Cmd::Pair(true));
                }
                if ui.add_enabled(self.wifi.pair.as_ref().is_some_and(|p| p.on), egui::Button::new("Unpair")).clicked()
                {
                    self.send(Cmd::Unpair);
                }
            });
        });
        if !on_usb {
            ui.weak("Pairing only works over USB.");
        }

        // Offer to move this studio session onto Wi-Fi.
        let on_wifi = luaboard::wifi::parse_tcp_port(&self.port).is_some();
        let target = self.discovery.devices().into_iter().next().map(|d| d.port).or_else(|| {
            let st = self.wifi.status.as_ref()?;
            Some(format!("tcp://{}:{}", st.ip.as_ref()?, st.port))
        });
        if let (false, Some(target), true) =
            (on_wifi, target, self.wifi.status.as_ref().is_some_and(|s| s.state == "connected"))
        {
            ui.separator();
            // Unpaired, the switch would drop the USB link and then be refused.
            let paired_here = self.wifi.pair.as_ref().is_some_and(|p| p.on && p.here);
            if ui.add_enabled(paired_here, egui::Button::new(format!("Use Wi-Fi in this studio ({target})"))).clicked()
            {
                self.port = target.clone();
                self.send(Cmd::Connect(target));
            }
            if paired_here {
                ui.weak("Moves this studio from USB to the device's Wi-Fi. The USB cable can then be unplugged.");
            } else {
                ui.weak("Pair this computer first: Wi-Fi takes commands only from paired computers.");
            }
        }
    }
}
