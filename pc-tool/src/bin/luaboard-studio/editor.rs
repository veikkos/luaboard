//! The editor for the open script.

use super::*;

/// Width of the line-number gutter inside the editor, in points.
const GUTTER: i8 = 44;

/// Find "name.lua:LINE:" in a Lua error message.
pub(crate) fn parse_error_location(msg: &str) -> Option<(String, usize)> {
    msg.split_whitespace().find_map(|tok| {
        let mut parts = tok.split(':');
        let file = parts.next()?;
        let line = parts.next()?.parse().ok()?;
        file.ends_with(".lua").then(|| (file.to_string(), line))
    })
}

impl Studio {
    pub(crate) fn editor(&mut self, ui: &mut egui::Ui) {
        let idx = self.selected;
        let mut save = false;
        ui.horizontal(|ui| {
            let s = &self.scripts[idx];
            ui.strong(&s.name);
            if let Some(p) = &s.path {
                ui.weak(p.display().to_string());
            } else {
                ui.weak("(not saved to disk)");
            }
            if ui.button("Save to disk").clicked() {
                save = true;
            }
        });
        if save || ui.input_mut(|i| i.consume_key(egui::Modifiers::COMMAND, egui::Key::S)) {
            self.save_file(idx);
        }

        let font = egui::TextStyle::Monospace.resolve(ui.style());
        let dark = ui.visuals().dark_mode;
        let error_msg_color = egui::Color32::from_rgb(255, 90, 90);
        let name = self.scripts[idx].name.clone();
        let syntax_line = self.scripts[idx].syntax.as_deref().and_then(|e| luaboard::syntax::error_line(&name, e));
        let err_line = syntax_line.or_else(|| self.error_at.as_ref().filter(|(f, _)| *f == name).map(|(_, l)| *l));
        if let Some(e) = &self.scripts[idx].syntax {
            ui.colored_label(error_msg_color, format!("⚠ {e}"));
        }
        let hl = &mut self.highlighter;
        let script = &mut self.scripts[idx];
        let mut edited = false;

        let mut layouter = |ui: &egui::Ui, buf: &dyn egui::TextBuffer, _wrap: f32| {
            let mut job = hl.job(buf.as_str(), &font, dark);
            job.wrap.max_width = f32::INFINITY; // no wrapping: one row per source line
            ui.fonts_mut(|f| f.layout_job(job))
        };

        // Fixed ids: the syntax error line above comes and goes, and with ids
        // from the layout the editor would lose its focus, cursor and scroll.
        egui::ScrollArea::both().id_salt(("editor scroll", &name)).auto_shrink([false, false]).show(ui, |ui| {
            let out = egui::TextEdit::multiline(&mut script.text)
                .id_salt(("editor", &name))
                .code_editor()
                .desired_width(f32::INFINITY)
                .desired_rows(24)
                .lock_focus(true)
                .margin(egui::Margin { left: GUTTER, right: 6, top: 4, bottom: 4 })
                .layouter(&mut layouter)
                .show(ui);
            if out.response.changed() {
                script.dirty = true;
                script.checked = false;
                edited = true;
            }

            // Gutter: line numbers aligned with the galley rows, error line marked.
            let painter = ui.painter_at(out.response.rect);
            let gutter_right = out.galley_pos.x - 8.0;
            let weak = ui.visuals().weak_text_color();
            let mut line = 1;
            let mut line_start = true;
            for row in &out.galley.rows {
                let r = row.rect().translate(out.galley_pos.to_vec2());
                if line_start {
                    let is_err = err_line == Some(line);
                    if is_err {
                        let band = egui::Rect::from_x_y_ranges(out.response.rect.x_range(), r.y_range());
                        painter.rect_filled(band, 0.0, error_msg_color.gamma_multiply(0.12));
                        painter.text(
                            egui::pos2(out.response.rect.left() + 4.0, r.center().y),
                            egui::Align2::LEFT_CENTER,
                            "●",
                            font.clone(),
                            error_msg_color,
                        );
                    }
                    painter.text(
                        egui::pos2(gutter_right, r.center().y),
                        egui::Align2::RIGHT_CENTER,
                        line.to_string(),
                        font.clone(),
                        if is_err { error_msg_color } else { weak },
                    );
                }
                line_start = row.ends_with_newline;
                if row.ends_with_newline {
                    line += 1;
                }
            }
        });
        if edited {
            self.live_due = Some(Instant::now() + LIVE_QUIET);
        }
    }
}
