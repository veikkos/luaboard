//! `luaboard-studio` - desktop GUI for the ESP32 Lua devboard.
//!
//! Edit a project (a folder of Lua scripts = one app on the device), upload
//! it over serial or Wi-Fi (real board or simulator) to try it, install it,
//! run/stop it, watch `print()` output and use a live REPL.
//!
//!   luaboard-studio [--port PATH] [project-folder | script.lua ...] [firmware.bin]

mod console;
mod demo;
mod device_panel;
mod editor;
mod events;
mod flash;
mod highlight;
mod login_ui;
mod preview;
mod project;
mod selftest;
mod sidebar;
mod toolbar;
mod wifi;
mod worker;

use editor::*;
use flash::*;
use login_ui::*;
use preview::*;
use project::*;
use selftest::*;
use wifi::*;
use worker::*;

use eframe::egui;
use luaboard::firmware::{self, FlashEvent, ImageInfo};
use luaboard::login;
use luaboard::ota;
use luaboard::{AppInfo, AppState, Device, FileInfo, Frame, Msg, DEFAULT_BAUD, SIM_LINK};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError, Sender};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

const API_DOC: &str = include_str!("../../../../docs/LUA_API.md");
const MAX_CONSOLE: usize = 2000;
/// Live: the project is uploaded and run again this long after its code last
/// changed (typing, or a save from another editor), so a burst of edits is
/// one upload.
const LIVE_QUIET: Duration = Duration::from_millis(600);
/// How often the project's files are checked for saves from another editor.
const DISK_CHECK_EVERY: Duration = Duration::from_millis(500);

/// Actions that ask first.
enum Confirm {
    /// Open this project folder, dropping unsaved edits.
    OpenProject(PathBuf),
    /// Delete this app from the device.
    Uninstall(String),
}

/// `path` relative to the current directory when it's inside it (for display).
fn short_path(path: &std::path::Path) -> String {
    std::env::current_dir()
        .ok()
        .and_then(|cwd| path.strip_prefix(cwd).ok().map(|p| p.display().to_string()))
        .filter(|p| !p.is_empty())
        .unwrap_or_else(|| path.display().to_string())
}

#[derive(Clone, Copy, PartialEq)]
enum LineKind {
    Log,
    Event,
    Info,
    Error,
    Input,
}

struct Studio {
    ctx: egui::Context,
    tx: Sender<Cmd>,
    rx: Receiver<Ev>,
    port: String,
    ports: Vec<String>,
    /// When `ports` was last listed: it's listed again now and then, so a
    /// simulator or board that appears later shows up.
    ports_at: std::time::Instant,
    /// Live update on: the project is uploaded and run when its code changes.
    live: bool,
    /// When the code last changed plus LIVE_QUIET: upload then.
    live_due: Option<Instant>,
    /// The project as last uploaded and run (project_hash), not sent again.
    live_sent: Option<u64>,
    /// When the project's files were last checked on disk.
    disk_at: Instant,
    connected: bool,
    /// Port of the current connection (the selection may differ).
    connected_port: Option<String>,
    /// Test hook (env LUABOARD_STUDIO_SELECT_PORT): select this port once connected.
    select_port_hook: Option<String>,
    device_info: String,
    device_files: Vec<FileInfo>,
    device_apps: Vec<AppInfo>,
    /// The device's autostart app (None: it boots into its menu).
    autostart: Option<String>,
    /// The device's clock and time zone (None: not asked yet).
    clock: Option<DeviceClock>,
    /// The time-zone picker's custom POSIX TZ field.
    tz_custom: String,
    /// The project folder (None: nothing saved yet). Its files are the app.
    project_dir: Option<PathBuf>,
    /// The app name on the device: the project folder's name.
    app: String,
    scripts: Vec<Script>,
    /// The project's assets (pictures, sounds): sent with it, read from disk
    /// each time, not edited here.
    assets: Vec<PathBuf>,
    selected: usize,
    entry: String,
    /// A question waiting for an answer (discard changes, uninstall).
    confirm: Option<Confirm>,
    console: Vec<(LineKind, String)>,
    repl: String,
    show_api: bool,
    flash: FlashUi,
    wifi: WifiUi,
    /// A Wi-Fi device this computer isn't paired with: (port, why).
    not_paired: Option<(String, String)>,
    login: Option<LoginPrompt>,
    /// Test hook (LUABOARD_STUDIO_AUTO_LOGIN): start a login as soon as one is asked for.
    auto_login: bool,
    discovery: luaboard::wifi::Discovery,
    highlighter: highlight::Highlighter,
    /// Location of the last script error: (file, 1-based line).
    error_at: Option<(String, usize)>,
    preview: Preview,
    /// Test hook (env LUABOARD_STUDIO_SCREENSHOT=out.ppm): upload & run the project
    /// once connected, then save a screenshot of the window and exit.
    selftest: Option<SelfTest>,
    /// Test hook (LUABOARD_STUDIO_DEMO, LUABOARD_STUDIO_RECORD): type a demo, recording it (demo.rs).
    demo: Option<demo::Demo>,
}

impl Studio {
    fn new(cc: &eframe::CreationContext<'_>, port: Option<String>, files: Vec<PathBuf>) -> Self {
        let (cmd_tx, cmd_rx) = mpsc::channel();
        let (ev_tx, ev_rx) = mpsc::channel();
        let ctx = cc.egui_ctx.clone();
        thread::spawn(move || worker(cmd_rx, ev_tx, ctx));

        let ports = luaboard::list_ports();
        let port = port.or_else(|| ports.first().cloned()).unwrap_or_else(|| SIM_LINK.to_string());
        let mut s = Studio {
            ctx: cc.egui_ctx.clone(),
            tx: cmd_tx,
            rx: ev_rx,
            port,
            ports,
            ports_at: std::time::Instant::now(),
            live: false,
            live_due: None,
            live_sent: None,
            disk_at: Instant::now(),
            connected: false,
            connected_port: None,
            select_port_hook: std::env::var("LUABOARD_STUDIO_SELECT_PORT").ok(),
            device_info: String::new(),
            device_files: Vec::new(),
            device_apps: Vec::new(),
            autostart: None,
            clock: None,
            tz_custom: String::new(),
            project_dir: None,
            app: DEFAULT_APP.into(),
            scripts: Vec::new(),
            assets: Vec::new(),
            selected: 0,
            entry: "main.lua".into(),
            confirm: None,
            console: Vec::new(),
            repl: String::new(),
            show_api: false,
            flash: FlashUi::new(),
            wifi: WifiUi::default(),
            not_paired: None,
            login: None,
            auto_login: std::env::var_os("LUABOARD_STUDIO_AUTO_LOGIN").is_some(),
            discovery: luaboard::wifi::Discovery::start(),
            highlighter: Default::default(),
            error_at: None,
            preview: Preview {
                shown: true,
                crc: None,
                remote: Remote::default(),
                live: true,
                pending: false,
                last_request: Instant::now(),
                texture: None,
                have_frame: false,
                size: [320, 240],
            },
            demo: demo::Demo::from_env(),
            selftest: std::env::var_os("LUABOARD_STUDIO_SCREENSHOT").map(|p| SelfTest {
                out: p.into(),
                started: None,
                requested: false,
            }),
        };
        // A folder is the project; loose .lua files open their folder as the
        // project with just those files; a .bin opens the Firmware window.
        let mut scripts = Vec::new();
        for f in files {
            if f.extension().is_some_and(|e| e == "bin") {
                s.flash.path = f.display().to_string();
                s.flash.open = true;
            } else if f.is_dir() {
                if s.project_dir.is_some() {
                    s.log(LineKind::Error, format!("{}: one project at a time, not opened", f.display()));
                } else {
                    s.open_project(f, None);
                }
            } else {
                scripts.push(f);
            }
        }
        if !scripts.is_empty() {
            if s.project_dir.is_some() {
                scripts.into_iter().for_each(|f| s.open_file(f));
            } else {
                let dir = luaboard::folder_of(&scripts[0]).to_path_buf();
                s.open_project(dir, Some(scripts));
            }
        }
        // Test hook (with LUABOARD_STUDIO_SCREENSHOT): open a window at startup.
        match std::env::var("LUABOARD_STUDIO_OPEN").as_deref() {
            Ok("wifi") => s.wifi.open = true,
            Ok("firmware") => s.flash.open = true,
            _ => {}
        }
        if s.scripts.is_empty() {
            s.add_template();
        }
        s.pick_entry();
        if luaboard::wifi::parse_tcp_port(&s.port).is_some() || std::path::Path::new(&s.port).exists() {
            s.send(Cmd::Connect(s.port.clone()));
        } else {
            s.send(Cmd::Await(s.port.clone()));
        }
        s
    }

    fn send(&self, c: Cmd) {
        let _ = self.tx.send(c);
    }

    fn log(&mut self, k: LineKind, s: impl Into<String>) {
        self.console.push((k, s.into()));
        if self.console.len() > MAX_CONSOLE {
            self.console.drain(..self.console.len() - MAX_CONSOLE);
        }
    }

    fn confirm_window(&mut self, ctx: &egui::Context) {
        let Some(c) = &self.confirm else { return };
        let (text, action) = match c {
            Confirm::OpenProject(dir) => {
                (format!("Open {}? Unsaved changes in this project are lost.", short_path(dir)), "Discard and open")
            }
            Confirm::Uninstall(app) => {
                (format!("Delete the app \"{app}\" from the device, in RAM and flash?"), "Uninstall")
            }
        };
        let (mut yes, mut no) = (false, false);
        egui::Window::new("Confirm")
            .collapsible(false)
            .resizable(false)
            .anchor(egui::Align2::CENTER_CENTER, [0.0, 0.0])
            .show(ctx, |ui| {
                ui.label(text);
                ui.horizontal(|ui| {
                    yes = ui.button(action).clicked();
                    no = ui.button("Cancel").clicked();
                });
            });
        if yes {
            match self.confirm.take() {
                Some(Confirm::OpenProject(dir)) => self.open_project(dir, None),
                Some(Confirm::Uninstall(app)) => {
                    self.log(LineKind::Input, format!("uninstall {app}"));
                    self.send(Cmd::Uninstall(app));
                }
                None => {}
            }
        } else if no {
            self.confirm = None;
        }
    }
}

impl eframe::App for Studio {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        self.handle_events();
        const PORTS_EVERY: Duration = Duration::from_secs(2);
        if self.ports_at.elapsed() >= PORTS_EVERY {
            self.ports = luaboard::list_ports();
            self.ports_at = std::time::Instant::now();
        }
        ui.ctx().request_repaint_after(PORTS_EVERY);
        if self.scripts.iter().any(|s| s.path.is_some()) {
            self.check_disk();
            ui.ctx().request_repaint_after(DISK_CHECK_EVERY);
        }
        self.live_tick(ui.ctx());
        for s in &mut self.scripts {
            s.check();
        }
        self.run_selftest(ui.ctx());
        self.run_demo(ui.ctx());
        self.poll_preview();
        if ui.input_mut(|i| i.consume_key(egui::Modifiers::COMMAND, egui::Key::Enter)) && self.connected {
            self.upload(true);
        }

        egui::Panel::top("toolbar").show(ui, |ui| {
            ui.add_space(4.0);
            self.toolbar(ui);
            ui.add_space(2.0);
        });
        egui::Panel::left("sidebar").resizable(true).default_size(260.0).show(ui, |ui| {
            egui::ScrollArea::vertical().show(ui, |ui| self.sidebar(ui));
        });
        // Order matters: earlier panels claim the outer space. The console spans
        // below both the editor and the display preview.
        egui::Panel::bottom("console").resizable(true).default_size(220.0).show(ui, |ui| {
            self.console(ui);
        });
        if self.preview.shown {
            egui::Panel::right("preview").resizable(true).default_size(350.0).max_size(700.0).show(ui, |ui| {
                self.preview_panel(ui);
            });
        }
        egui::CentralPanel::default().show(ui, |ui| self.editor(ui));

        let mut open = self.show_api;
        egui::Window::new("Lua API reference").open(&mut open).default_size([560.0, 520.0]).vscroll(true).show(
            ui.ctx(),
            |ui| {
                ui.label(egui::RichText::new(API_DOC).monospace().size(11.5));
            },
        );
        self.show_api = open;

        let mut open = self.flash.open;
        egui::Window::new("Firmware")
            .open(&mut open)
            .default_width(460.0)
            .max_width(520.0)
            .resizable(false)
            .show(ui.ctx(), |ui| self.firmware_window(ui));
        self.flash.open = open || self.flash.busy;

        if let Some((port, error)) = self.not_paired.clone() {
            let mut keep = true;
            egui::Window::new("Not paired for Wi-Fi")
                .collapsible(false)
                .resizable(false)
                .anchor(egui::Align2::CENTER_CENTER, [0.0, 0.0])
                .show(ui.ctx(), |ui| {
                    ui.label(format!("Can't use {port} over Wi-Fi from this computer."));
                    ui.colored_label(egui::Color32::from_rgb(255, 110, 110), &error);
                    ui.label("Connect the device by USB, open Wi-Fi and press \"Pair this computer\".");
                    if ui.button("OK").clicked() {
                        keep = false;
                    }
                });
            if !keep {
                self.not_paired = None;
            }
        }

        self.login_window(ui.ctx());
        self.confirm_window(ui.ctx());

        let mut open = self.wifi.open;
        egui::Window::new("Wi-Fi")
            .open(&mut open)
            .default_width(420.0)
            .resizable(false)
            .show(ui.ctx(), |ui| self.wifi_window(ui));
        self.wifi.open = open;
    }
}

fn main() -> eframe::Result {
    let mut port = None;
    let mut files = Vec::new();
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        match a.as_str() {
            "-p" | "--port" => port = args.next(),
            "-h" | "--help" => {
                println!("usage: luaboard-studio [--port PATH] [project-folder | script.lua ...] [firmware.bin]");
                println!("  A folder opens as the project (the app named after it); .lua files open");
                println!("  their folder as the project with just those files.");
                return Ok(());
            }
            _ => files.push(PathBuf::from(a)),
        }
    }

    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default().with_title("Luaboard Studio").with_inner_size([1100.0, 760.0]),
        ..Default::default()
    };
    eframe::run_native("Luaboard Studio", options, Box::new(move |cc| Ok(Box::new(Studio::new(cc, port, files)))))
}
