//! The "On device" part of the sidebar: the device's apps, files and clock.

use super::*;

impl Studio {
    /// The device's apps (state, autostart, actions), each with its files.
    pub(crate) fn device_panel(&mut self, ui: &mut egui::Ui) {
        if !self.connected {
            ui.weak("not connected");
            return;
        }
        if self.device_apps.is_empty() {
            ui.weak("No apps yet: Upload & Run tries the project, Install keeps it.");
        }
        let green = egui::Color32::from_rgb(80, 200, 120);
        let amber = egui::Color32::from_rgb(230, 180, 90);
        for app in self.device_apps.clone() {
            let (state, col, tip) = match app.state {
                AppState::Installed => ("installed", green, "In flash: in the device's menu, survives a restart"),
                AppState::Ram => {
                    ("not installed", amber, "Uploaded to RAM only: gone after a restart. Install keeps it.")
                }
                AppState::Modified => (
                    "modified",
                    amber,
                    "Installed, with newer uploads in RAM that a restart drops. Install keeps them.",
                ),
            };
            let auto = self.autostart.as_deref() == Some(app.name.as_str());
            let mut title = egui::text::LayoutJob::default();
            let font = egui::TextStyle::Body.resolve(ui.style());
            let fmt = |color| egui::TextFormat { font_id: font.clone(), color, ..Default::default() };
            title.append(&app.name, 0.0, fmt(ui.visuals().strong_text_color()));
            title.append(state, 8.0, fmt(col));
            if auto {
                title.append("autostart", 8.0, fmt(egui::Color32::from_rgb(120, 170, 255)));
            }
            if app.name == self.app {
                title.append("(this project)", 8.0, fmt(ui.visuals().weak_text_color()));
            }
            let header = egui::CollapsingHeader::new(title)
                .id_salt(("app", &app.name))
                .default_open(app.name == self.app)
                .show(ui, |ui| {
                    ui.horizontal_wrapped(|ui| {
                        if ui.small_button("▶ run").on_hover_text(format!("Run {}/main.lua", app.name)).clicked() {
                            self.error_at = None;
                            self.send(Cmd::Run(Some(app.name.clone())));
                        }
                        if app.state != AppState::Installed
                            && ui
                                .small_button("install")
                                .on_hover_text("Write this app's uploads on the device to flash")
                                .clicked()
                        {
                            self.send(Cmd::InstallApp(app.name.clone()));
                        }
                        if ui
                            .small_button("uninstall")
                            .on_hover_text("Delete the app from the device (RAM and flash)")
                            .clicked()
                        {
                            self.confirm = Some(Confirm::Uninstall(app.name.clone()));
                        }
                        let mut on = auto;
                        let can = app.state != AppState::Ram || auto;
                        if ui
                            .add_enabled(can, egui::Checkbox::new(&mut on, "autostart"))
                            .on_hover_text("Start this app at boot instead of the menu (also in the device's Settings)")
                            .on_disabled_hover_text("Install the app first")
                            .changed()
                        {
                            self.send(Cmd::Autorun(on.then(|| app.name.clone())));
                        }
                    });
                    let prefix = format!("{}/", app.name);
                    for f in self.device_files.clone().iter().filter(|f| f.name.starts_with(&prefix)) {
                        self.device_file_row(ui, f, &f.name[prefix.len()..]);
                    }
                });
            header.header_response.on_hover_text(format!("{tip}. {} file(s), {} B", app.files, app.bytes));
        }
        // Files outside any listed app, shown so they can be removed.
        let loose: Vec<FileInfo> = self
            .device_files
            .iter()
            .filter(|f| !self.device_apps.iter().any(|a| f.name.starts_with(&format!("{}/", a.name))))
            .cloned()
            .collect();
        for f in &loose {
            self.device_file_row(ui, f, &f.name);
        }
        if let Some(a) = &self.autostart {
            if !self.device_apps.iter().any(|x| &x.name == a) {
                ui.colored_label(amber, format!("autostart: {a} (not on the device)"));
            }
        }
        ui.horizontal(|ui| {
            ui.small(match &self.autostart {
                Some(a) => format!("At boot: {a}"),
                None => "At boot: the menu".into(),
            });
            if self.autostart.is_some() && ui.small_button("menu instead").clicked() {
                self.send(Cmd::Autorun(None));
            }
        });
        self.clock_ui(ui);
    }

    /// The device's clock and its time-zone picker.
    pub(crate) fn clock_ui(&mut self, ui: &mut egui::Ui) {
        let Some(clock) = self.clock.clone() else { return };
        ui.separator();
        ui.horizontal(|ui| {
            ui.small("Clock:");
            match clock.now_local() {
                Some(t) => {
                    let r = ui.small(t);
                    if clock.source == "rtc" {
                        r.on_hover_text("From the board's clock chip (kept while powered); NTP sets it once on Wi-Fi.");
                    }
                    ui.ctx().request_repaint_after(Duration::from_millis(500));
                }
                None => {
                    ui.colored_label(egui::Color32::from_rgb(230, 180, 90), "not set yet")
                        .on_hover_text("The device sets its clock from the network (NTP) once it's on Wi-Fi. It has no clock that keeps running while it's off.");
                }
            }
        });
        let name = luaboard::timezone::name_of(&clock.tz);
        let mut pick: Option<String> = None;
        ui.horizontal(|ui| {
            ui.small("Time zone:");
            egui::ComboBox::from_id_salt("tz")
                .selected_text(name.unwrap_or("custom"))
                .height(320.0)
                .show_ui(ui, |ui| {
                    for (n, tz) in luaboard::timezone::ZONES {
                        if ui.selectable_label(name == Some(*n), *n).on_hover_text(*tz).clicked() && *tz != clock.tz {
                            pick = Some(tz.to_string());
                        }
                    }
                })
                .response
                .on_hover_text(format!("{}\nFor the menu's clock and sys.date() in scripts.", clock.tz));
        });
        ui.horizontal(|ui| {
            if self.tz_custom.is_empty() && name.is_none() {
                self.tz_custom = clock.tz.clone();
            }
            ui.add(
                egui::TextEdit::singleline(&mut self.tz_custom)
                    .desired_width(180.0)
                    .font(egui::TextStyle::Small)
                    .hint_text("POSIX TZ, e.g. <+0530>-5:30"),
            )
            .on_hover_text("Any zone as a POSIX TZ string. Offsets are west-positive: UTC+2 is -2.");
            let custom = self.tz_custom.trim().to_string();
            if ui.add_enabled(!custom.is_empty() && custom != clock.tz, egui::Button::new("set").small()).clicked() {
                pick = Some(custom);
            }
        });
        if let Some(tz) = pick {
            self.tz_custom.clear();
            self.send(Cmd::SetTz(tz));
        }
    }

    pub(crate) fn device_file_row(&mut self, ui: &mut egui::Ui, f: &FileInfo, label: &str) {
        ui.horizontal(|ui| {
            ui.monospace(format!("{:<16}{:>6} B", label, f.size));
            if ui.small_button("pull").on_hover_text("Download into the project").clicked() {
                self.send(Cmd::Pull(f.name.clone()));
            }
            if ui.small_button("rm").on_hover_text("Delete this file from the device").clicked() {
                self.send(Cmd::Remove(f.name.clone()));
            }
            if f.name.ends_with(".lua") && ui.small_button("run").clicked() {
                self.error_at = None;
                self.send(Cmd::Run(Some(f.name.clone())));
            }
        });
    }
}
