//! The sidebar: the project's files, and below them what is on the device.

use super::*;

impl Studio {
    pub(crate) fn sidebar(&mut self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            ui.heading("Project:");
            ui.heading(egui::RichText::new(&self.app).strong());
        });
        match &self.project_dir {
            Some(dir) => ui.weak(short_path(dir)),
            None => ui.weak("no folder yet (saving a file picks one)"),
        }
        .on_hover_text(
            "The project folder's .lua files, app.json, pictures (.jpg) and sounds (.wav) are one app on the \
             device, named after the folder.",
        );
        ui.horizontal(|ui| {
            if ui
                .button("Open folder…")
                .on_hover_text("Open a project folder (or make a new one in the dialog). Its files are the app.")
                .clicked()
            {
                if let Some(dir) = rfd::FileDialog::new().pick_folder() {
                    if self.has_unsaved() {
                        self.confirm = Some(Confirm::OpenProject(dir));
                    } else {
                        self.open_project(dir, None);
                    }
                }
            }
            if ui
                .button("Add file…")
                .on_hover_text("Add .lua files, pictures or sounds to this project (they keep their location on disk)")
                .clicked()
            {
                let picked = rfd::FileDialog::new()
                    .add_filter("Lua, pictures, sounds", &["lua", "jpg", "jpeg", "png", "wav"])
                    .pick_files();
                if let Some(paths) = picked {
                    paths.into_iter().for_each(|p| self.open_file(p));
                }
            }
            if ui.button("New file").clicked() {
                let mut n = 1;
                while self.scripts.iter().any(|s| s.name == format!("module{n}.lua")) {
                    n += 1;
                }
                self.scripts.push(Script {
                    name: format!("module{n}.lua"),
                    path: None,
                    text: "local M = {}\n\nreturn M\n".into(),
                    dirty: true,
                    ..Default::default()
                });
                self.selected = self.scripts.len() - 1;
            }
        });
        let mut remove = None;
        for (i, s) in self.scripts.iter().enumerate() {
            let mut label = s.name.clone();
            if s.name == self.entry {
                label = format!("▶ {label}");
            }
            if s.dirty {
                label.push_str(" *");
            }
            if s.syntax.is_some() {
                label = format!("⚠ {label}");
            }
            let r = ui.selectable_label(i == self.selected, label);
            if r.clicked() {
                self.selected = i;
            }
            r.context_menu(|ui| {
                if ui.button("Set as entry").clicked() {
                    self.entry = s.name.clone();
                    ui.close();
                }
                if ui.button("Remove from project").clicked() {
                    remove = Some(i);
                    ui.close();
                }
            });
        }
        let mut remove_asset = None;
        for (i, p) in self.assets.iter().enumerate() {
            let name = p.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
            let kb = std::fs::metadata(p).map(|m| m.len().div_ceil(1024)).unwrap_or(0);
            let label = egui::Label::new(egui::RichText::new(format!("{name}  {kb} KB")).weak());
            let r = ui
                .add(label.sense(egui::Sense::click()))
                .on_hover_text(format!("Sent with the app; a script reads it with asset(\"{name}\")"));
            r.context_menu(|ui| {
                if ui.button("Remove from project").clicked() {
                    remove_asset = Some(i);
                    ui.close();
                }
            });
        }
        if let Some(i) = remove_asset {
            self.assets.remove(i);
        }
        if let Some(i) = remove {
            if self.scripts.len() > 1 {
                self.scripts.remove(i);
                self.selected = self.selected.min(self.scripts.len() - 1);
                let selected = self.selected;
                self.pick_entry();
                self.selected = selected;
            }
        }
        ui.small("right-click: entry / remove");
        if self.entry != "main.lua" {
            ui.small(format!("The device's menu starts main.lua; ▶ {} is for Run here.", self.entry));
        }

        ui.add_space(12.0);
        ui.separator();
        ui.horizontal(|ui| {
            ui.heading("On device");
            if ui.add_enabled(self.connected, egui::Button::new("↻")).clicked() {
                self.send(Cmd::List);
            }
        });
        self.device_panel(ui);
        if !self.device_info.is_empty() {
            ui.add_space(8.0);
            ui.collapsing("Device info", |ui| {
                for kv in self.device_info.split_whitespace() {
                    ui.small(kv);
                }
            });
        }
    }
}
