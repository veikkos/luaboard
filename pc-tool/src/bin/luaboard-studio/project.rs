//! The project: its scripts on disk, saving, uploading and installing them.

use super::*;

#[derive(Default)]
pub(crate) struct Script {
    pub(crate) name: String,
    pub(crate) path: Option<PathBuf>,
    pub(crate) text: String,
    pub(crate) dirty: bool,
    /// Syntax error from the last check (same parser as the firmware).
    pub(crate) syntax: Option<String>,
    /// `syntax` is up to date with `text`.
    pub(crate) checked: bool,
    /// The file's modification time when it was last read or written, to
    /// notice saves from another editor.
    pub(crate) disk: Option<std::time::SystemTime>,
}

pub(crate) fn modified(p: &std::path::Path) -> Option<std::time::SystemTime> {
    std::fs::metadata(p).and_then(|m| m.modified()).ok()
}

impl Script {
    pub(crate) fn check(&mut self) {
        if !self.checked {
            self.syntax = if self.name.ends_with(".lua") {
                luaboard::syntax::check(&self.name, self.text.as_bytes()).err()
            } else {
                None
            };
            self.checked = true;
        }
    }
}

/// App name used until the project has a folder.
pub(crate) const DEFAULT_APP: &str = "sketch";

pub(crate) const MAIN_TEMPLATE: &str = "-- main.lua: the app's entry script (started from the device's menu)\n\
disp.clear(disp.NAVY)\n\
disp.text(10, 10, \"Hello from Lua!\", disp.WHITE, 3)\n\n\
function loop(dt)\n\
\x20\x20local knob = adc.read(adc.PORTB_IN)\n\
\x20\x20disp.rect(10, 60, 300, 20, disp.DARKGRAY, true)\n\
\x20\x20disp.rect(10, 60, knob * 300 // 4096, 20, disp.ORANGE, true)\n\
\x20\x20local x, y = touch.read()\n\
\x20\x20if x then disp.circle(x, y, 6, disp.YELLOW, true) end\n\
end\n";

impl Studio {
    /// Make `dir` the project: its `*.lua` files and app.json (or just `only`), app named
    /// after the folder. An empty folder starts with a new main.lua.
    pub(crate) fn open_project(&mut self, dir: PathBuf, only: Option<Vec<PathBuf>>) {
        let files = match only {
            Some(f) => f,
            None => match std::fs::read_dir(&dir) {
                Ok(it) => {
                    let mut f: Vec<PathBuf> =
                        it.filter_map(|e| e.ok()).map(|e| e.path()).filter(|p| luaboard::is_project_file(p)).collect();
                    f.sort();
                    f
                }
                Err(e) => {
                    self.log(LineKind::Error, format!("open {}: {e}", dir.display()));
                    return;
                }
            },
        };
        self.app = luaboard::app_name_for(&dir);
        self.project_dir = Some(dir);
        self.scripts.clear();
        self.assets.clear();
        self.selected = 0;
        self.error_at = None;
        files.into_iter().for_each(|f| self.open_file(f));
        if self.scripts.is_empty() {
            self.add_template();
        }
        self.entry = "main.lua".into();
        self.pick_entry();
    }

    pub(crate) fn add_template(&mut self) {
        self.scripts.push(Script {
            name: "main.lua".into(),
            path: None,
            text: MAIN_TEMPLATE.into(),
            ..Default::default()
        });
    }

    /// Keep the entry script if the project has it, else main.lua, else the first file.
    pub(crate) fn pick_entry(&mut self) {
        if !self.scripts.iter().any(|x| x.name == self.entry) {
            self.entry = if self.scripts.iter().any(|x| x.name == "main.lua") {
                "main.lua".into()
            } else {
                let lua = self.scripts.iter().find(|x| x.name.ends_with(".lua"));
                lua.unwrap_or(&self.scripts[0]).name.clone()
            };
        }
        if let Some(i) = self.scripts.iter().position(|x| x.name == self.entry) {
            self.selected = i;
        }
    }

    pub(crate) fn has_unsaved(&self) -> bool {
        self.scripts.iter().any(|s| s.dirty)
    }

    pub(crate) fn open_file(&mut self, path: PathBuf) {
        let name = path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        if luaboard::is_asset(&name) {
            self.assets.retain(|p| p.file_name() != path.file_name());
            self.assets.push(path);
            self.assets.sort();
            return;
        }
        match std::fs::read_to_string(&path) {
            Ok(text) => {
                let name = path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
                let disk = modified(&path);
                if let Some(i) = self.scripts.iter().position(|s| s.name == name) {
                    self.scripts[i] = Script { name, path: Some(path), text, disk, ..Default::default() };
                    self.selected = i;
                } else {
                    self.scripts.push(Script { name, path: Some(path), text, disk, ..Default::default() });
                    self.selected = self.scripts.len() - 1;
                }
            }
            Err(e) => self.log(LineKind::Error, format!("open {}: {e}", path.display())),
        }
    }

    pub(crate) fn save_file(&mut self, idx: usize) {
        let s = &mut self.scripts[idx];
        // New files go into the project folder; without one, ask where (and
        // that folder becomes the project).
        let path = match (&s.path, &self.project_dir) {
            (Some(p), _) => p.clone(),
            (None, Some(dir)) => dir.join(&s.name),
            (None, None) => match rfd::FileDialog::new().set_file_name(&s.name).add_filter("Lua", &["lua"]).save_file()
            {
                Some(p) => p,
                None => return,
            },
        };
        match std::fs::write(&path, &s.text) {
            Ok(()) => {
                let new = path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or(s.name.clone());
                let old = std::mem::replace(&mut s.name, new);
                if self.entry == old {
                    self.entry = s.name.clone();
                }
                s.dirty = false;
                s.disk = modified(&path);
                if self.project_dir.is_none() {
                    let dir = luaboard::folder_of(&path);
                    self.app = luaboard::app_name_for(dir);
                    self.project_dir = Some(dir.to_path_buf());
                    let msg = format!("project folder: {} (app \"{}\")", short_path(dir), self.app);
                    s.path = Some(path);
                    self.log(LineKind::Info, msg);
                    return;
                }
                s.path = Some(path);
            }
            Err(e) => {
                let msg = format!("save {}: {e}", path.display());
                self.log(LineKind::Error, msg);
            }
        }
    }

    /// The project's files (name, contents), or None after logging why not
    /// (a syntax error, so nothing broken reaches the device).
    pub(crate) fn project_files(&mut self, what: &str) -> Option<Vec<(String, Vec<u8>)>> {
        self.error_at = None;
        for s in &mut self.scripts {
            s.check();
        }
        if let Some(i) = self.scripts.iter().position(|s| s.syntax.is_some()) {
            let msg = self.scripts[i].syntax.clone().unwrap_or_default();
            self.log(LineKind::Error, format!("{msg}  (syntax error: nothing {what})"));
            self.selected = i;
            return None;
        }
        let mut files: Vec<(String, Vec<u8>)> =
            self.scripts.iter().map(|s| (s.name.clone(), s.text.clone().into_bytes())).collect();
        for p in self.assets.clone() {
            let name = p.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
            match std::fs::read(&p) {
                Ok(data) => files.push((name, data)),
                Err(e) => {
                    self.log(LineKind::Error, format!("{}: {e}  (nothing {what})", p.display()));
                    return None;
                }
            }
        }
        if let Some(dir) = &self.project_dir {
            luaboard::stamp_dev_version(dir, &mut files);
        }
        Some(files)
    }

    /// The project folder's secrets file (empty without one), or None after
    /// logging why it can't be read.
    pub(crate) fn project_secrets(&mut self) -> Option<Secrets> {
        let Some(dir) = self.project_dir.clone() else { return Some(Vec::new()) };
        match luaboard::project_secrets(&dir) {
            Ok(s) => Some(s),
            Err(e) => {
                self.log(LineKind::Error, format!("{e:#}"));
                None
            }
        }
    }

    /// Upload the project to the device's RAM (not installed), optionally run it.
    pub(crate) fn upload(&mut self, run: bool) {
        let Some(files) = self.project_files("uploaded") else { return };
        let Some(secrets) = self.project_secrets() else { return };
        self.live_sent = run.then(|| self.project_hash());
        self.log(
            LineKind::Input,
            format!(
                "upload {} file(s) as {}{}",
                files.len(),
                self.app,
                if run { format!(", run {}", self.entry) } else { String::new() }
            ),
        );
        self.send(Cmd::Upload { app: self.app.clone(), files, secrets, run: run.then(|| self.entry.clone()) });
    }

    /// What an upload and run sends, to tell whether it changed.
    pub(crate) fn project_hash(&self) -> u64 {
        use std::hash::{Hash, Hasher};
        let mut h = std::collections::hash_map::DefaultHasher::new();
        (&self.app, &self.entry).hash(&mut h);
        for s in &self.scripts {
            (&s.name, &s.text).hash(&mut h);
        }
        h.finish()
    }

    /// Saves from another editor: reload the file, unless it has edits here
    /// not saved (those are kept, and saving them overwrites the file).
    pub(crate) fn check_disk(&mut self) {
        if self.disk_at.elapsed() < DISK_CHECK_EVERY {
            return;
        }
        self.disk_at = Instant::now();
        let mut notes = Vec::new();
        for s in &mut self.scripts {
            let Some(path) = &s.path else { continue };
            let m = modified(path);
            if m.is_none() || m == s.disk {
                continue;
            }
            s.disk = m;
            if s.dirty {
                notes.push((
                    LineKind::Error,
                    format!("{} changed on disk; keeping the edits here (save to overwrite)", s.name),
                ));
            } else if let Ok(text) = std::fs::read_to_string(path) {
                if text != s.text {
                    s.text = text;
                    s.checked = false;
                    notes.push((LineKind::Info, format!("{} changed on disk, reloaded", s.name)));
                }
            }
        }
        if notes.iter().any(|(k, _)| matches!(k, LineKind::Info)) {
            self.live_due = Some(Instant::now() + LIVE_QUIET);
        }
        for (k, n) in notes {
            self.log(k, n);
        }
    }

    /// Live: upload and run once the code has been quiet LIVE_QUIET. Code
    /// with a syntax error waits for the next change (the editor shows it).
    pub(crate) fn live_tick(&mut self, ctx: &egui::Context) {
        let Some(due) = self.live_due else { return };
        if !self.live || !self.connected {
            self.live_due = None;
            return;
        }
        let now = Instant::now();
        if now < due {
            ctx.request_repaint_after(due - now);
            return;
        }
        self.live_due = None;
        for s in &mut self.scripts {
            s.check();
        }
        if self.scripts.iter().any(|s| s.syntax.is_some()) || self.live_sent == Some(self.project_hash()) {
            return;
        }
        self.upload(true);
    }

    /// Install the project to flash as its app.
    pub(crate) fn install(&mut self) {
        let Some(files) = self.project_files("installed") else { return };
        let Some(secrets) = self.project_secrets() else { return };
        self.log(LineKind::Input, format!("install {} ({} file(s))", self.app, files.len()));
        self.send(Cmd::Install { app: self.app.clone(), files, secrets });
    }
}
