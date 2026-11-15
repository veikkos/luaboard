//! The console: the device's output and the REPL.

use super::*;

impl Studio {
    pub(crate) fn console(&mut self, ui: &mut egui::Ui) {
        // egui persists a panel's size from the space its contents used, and the
        // log + REPL can come up a few px short, so claim the full height or the
        // panel creeps back down after a resize.
        ui.set_min_height(ui.available_height());
        ui.horizontal(|ui| {
            ui.strong("Console");
            if ui.small_button("clear").clicked() {
                self.console.clear();
            }
        });
        let h = ui.available_height() - 30.0;
        egui::ScrollArea::vertical().max_height(h.max(40.0)).auto_shrink([false, false]).stick_to_bottom(true).show(
            ui,
            |ui| {
                for (k, line) in &self.console {
                    let color = match k {
                        LineKind::Log => ui.visuals().text_color(),
                        LineKind::Event => egui::Color32::from_rgb(120, 170, 255),
                        LineKind::Info => egui::Color32::from_rgb(140, 200, 140),
                        LineKind::Error => egui::Color32::from_rgb(255, 110, 110),
                        LineKind::Input => egui::Color32::from_rgb(200, 180, 100),
                    };
                    ui.label(egui::RichText::new(line).monospace().color(color));
                }
            },
        );
        ui.horizontal(|ui| {
            ui.monospace("lua>");
            let r = ui.add(
                egui::TextEdit::singleline(&mut self.repl)
                    .font(egui::TextStyle::Monospace)
                    .desired_width(f32::INFINITY)
                    .hint_text("expression or statement, e.g. adc.read(adc.PORTB_IN)  (Enter)"),
            );
            if r.lost_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter)) && !self.repl.trim().is_empty() {
                let code = std::mem::take(&mut self.repl);
                self.log(LineKind::Input, format!("lua> {code}"));
                self.send(Cmd::Eval(code));
                r.request_focus();
            }
        });
    }
}
