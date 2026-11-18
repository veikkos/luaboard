//! The toolbar along the top.

use super::*;

impl Studio {
    pub(crate) fn toolbar(&mut self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            ui.label("Port");
            let shown = if self.port.chars().count() > 32 {
                format!(
                    "…{}",
                    self.port.chars().rev().take(31).collect::<Vec<_>>().into_iter().rev().collect::<String>()
                )
            } else {
                self.port.clone()
            };
            let selected_before = self.port.clone();
            // Test hook: pick a port as if chosen in the list, once connected.
            if self.connected {
                if let Some(p) = self.select_port_hook.take() {
                    self.port = p;
                }
            }
            egui::ComboBox::from_id_salt("port").width(240.0).selected_text(shown).show_ui(ui, |ui| {
                for p in self.ports.clone() {
                    ui.selectable_value(&mut self.port, p.clone(), p);
                }
                for d in self.discovery.devices() {
                    let label = format!("{}  ·  {} (Wi-Fi)", d.port, d.name);
                    ui.selectable_value(&mut self.port, d.port.clone(), label);
                }
            });
            // Picking another port in the list connects to it right away.
            if self.port != selected_before {
                self.send(Cmd::Connect(self.port.clone()));
            }
            ui.add(egui::TextEdit::singleline(&mut self.port).desired_width(200.0).hint_text(luaboard::PORT_EXAMPLE))
                .on_hover_text("Port path (editable); press Connect after typing");
            if ui.button("Refresh").clicked() {
                self.ports = luaboard::list_ports();
                self.ports_at = std::time::Instant::now();
            }
            // "Disconnect" only for the port we're connected to; otherwise
            // "Connect" (switches over if connected elsewhere).
            if self.connected && self.connected_port.as_deref() == Some(self.port.as_str()) {
                if ui.button("Disconnect").clicked() {
                    self.send(Cmd::Disconnect);
                }
            } else if ui.button("Connect").clicked() {
                self.send(Cmd::Connect(self.port.clone()));
            }
            let (txt, col) = if self.connected {
                ("● connected", egui::Color32::from_rgb(80, 200, 120))
            } else {
                ("● offline", egui::Color32::from_rgb(200, 90, 90))
            };
            ui.colored_label(col, txt);
        });
        ui.horizontal(|ui| {
            ui.add_enabled_ui(self.connected, |ui| {
                if ui
                    .button("▶ Upload & Run")
                    .on_hover_text(format!(
                        "Upload the project as app \"{}\" to the device's RAM, then run the entry script (⌘/Ctrl+Enter). \
                         Not installed: gone after a restart.",
                        self.app
                    ))
                    .clicked()
                {
                    self.upload(true);
                }
                if ui
                    .checkbox(&mut self.live, "Live")
                    .on_hover_text(
                        "Upload & Run again shortly after the code changes: typing here, or a save from another \
                         editor. Code with a syntax error waits for the next change.",
                    )
                    .changed()
                    && self.live
                {
                    self.live_due = Some(Instant::now());
                }
                if ui.button("Upload").on_hover_text("Upload the project to the device's RAM without running it").clicked() {
                    self.upload(false);
                }
                if ui.button("Run").on_hover_text("Run the entry script of the app on the device").clicked() {
                    self.error_at = None;
                    self.send(Cmd::Run(Some(format!("{}/{}", self.app, self.entry))));
                }
                if ui.button("■ Stop").on_hover_text("Stop the script; the device shows its menu").clicked() {
                    self.send(Cmd::Stop);
                }
                if ui
                    .button("⬇ Install")
                    .on_hover_text(format!(
                        "Write the project to the device's flash as app \"{}\": it stays after a restart and shows \
                         in the device's menu. Device files of this app that the project doesn't have are removed.",
                        self.app
                    ))
                    .clicked()
                {
                    self.install();
                }
            });
            ui.separator();
            ui.label("entry:");
            egui::ComboBox::from_id_salt("entry").selected_text(self.entry.clone()).show_ui(ui, |ui| {
                for s in &self.scripts {
                    ui.selectable_value(&mut self.entry, s.name.clone(), s.name.clone());
                }
            });
            ui.separator();
            ui.toggle_value(&mut self.preview.shown, "Display")
                .on_hover_text("The device's screen, live, and touch from here. Hidden: nothing is polled or sent.");
            if ui.button("Lua API").on_hover_text(API_DOC_URL).clicked() {
                if let Err(e) = login::open_browser(API_DOC_URL) {
                    self.log(LineKind::Error, format!("couldn't open the browser: {e:#}"));
                }
            }
            ui.toggle_value(&mut self.flash.open, "Firmware…");
            if ui.toggle_value(&mut self.wifi.open, "Wi-Fi…").changed() && self.wifi.open {
                self.wifi.requested = false;
            }
        });
    }
}
