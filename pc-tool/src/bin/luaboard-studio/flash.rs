//! The Firmware window: update over the link, or flash over USB.

use super::*;

/// A firmware file picked in the Firmware window.
pub(crate) struct FwImage {
    /// What an update over the link sends (a device or a simulator image).
    pub(crate) update: ota::UpdateImage,
    /// A merged device image (the `fw` build) that can be flashed over USB too.
    pub(crate) usb: Option<(ImageInfo, Vec<u8>)>,
}

/// The Firmware window: pick an image, update or flash it, show progress.
pub(crate) struct FlashUi {
    pub(crate) open: bool,
    pub(crate) path: String,
    /// Inspected image for `checked_path`, or an error message.
    pub(crate) image: Option<Result<FwImage, String>>,
    pub(crate) checked_path: String,
    pub(crate) busy: bool,
    pub(crate) progress: f32,
    pub(crate) status: String,
}

impl FlashUi {
    pub(crate) fn new() -> Self {
        // Default: the image the `fw` build writes, relative to the repo root.
        let path = std::fs::read_dir("build")
            .ok()
            .and_then(|it| {
                it.filter_map(|e| e.ok()).map(|e| e.path()).find(|p| {
                    let n = p.file_name().unwrap_or_default().to_string_lossy();
                    n.starts_with("luaboard-") && n.ends_with(".bin")
                })
            })
            .map(|p| p.display().to_string())
            .unwrap_or_default();
        FlashUi {
            open: false,
            path,
            image: None,
            checked_path: String::new(),
            busy: false,
            progress: 0.0,
            status: String::new(),
        }
    }

    /// (Re)inspect the image when the path changed.
    pub(crate) fn refresh(&mut self) {
        if self.path == self.checked_path {
            return;
        }
        self.checked_path = self.path.clone();
        self.image = if self.path.trim().is_empty() {
            None
        } else {
            Some(std::fs::read(self.path.trim()).map_err(|e| format!("cannot read file: {e}")).and_then(|bytes| {
                let update = ota::image(&bytes).map_err(|e| format!("{e:#}"))?;
                let usb = firmware::inspect(&bytes)
                    .ok()
                    .filter(|info| firmware::check_for_device(info).is_ok())
                    .map(|info| (info, bytes));
                Ok(FwImage { update, usb })
            }))
        };
    }
}

impl Studio {
    pub(crate) fn firmware_window(&mut self, ui: &mut egui::Ui) {
        self.flash.refresh();
        let info = self.device_info.clone();
        let field = |k: &str| luaboard::kv_field(&info, k).map(str::to_string);
        let connected = !info.is_empty();
        let on_sim = field("target").as_deref() == Some("sim");

        ui.label("Image: the .bin the `fw` build target writes for a device, a simulator executable for a simulator.");
        ui.horizontal(|ui| {
            ui.add_enabled(
                !self.flash.busy,
                egui::TextEdit::singleline(&mut self.flash.path).desired_width(360.0).hint_text("build/luaboard-….bin"),
            );
            if ui.add_enabled(!self.flash.busy, egui::Button::new("Browse…")).clicked() {
                if let Some(p) =
                    rfd::FileDialog::new().add_filter("Firmware", &["bin"]).add_filter("All files", &["*"]).pick_file()
                {
                    self.flash.path = p.display().to_string();
                }
            }
        });
        // Connected to a simulator: offer the one the build writes.
        if on_sim && !self.flash.busy {
            let board = field("board").unwrap_or_default();
            let built = format!("build/{board}/esp32-sim{}", std::env::consts::EXE_SUFFIX);
            if std::path::Path::new(&built).exists()
                && self.flash.path.trim() != built
                && ui.small_button(format!("Use {built}")).clicked()
            {
                self.flash.path = built;
            }
        }

        // Only what's needed to draw: the image itself is copied on a click.
        let mut fits = Err(String::new());
        let mut has_usb = false;
        match &self.flash.image {
            None => {
                ui.weak("No image selected. Build one with the `fw` target (or the simulator, the default target).");
            }
            Some(Err(e)) => {
                ui.colored_label(egui::Color32::from_rgb(255, 110, 110), e);
            }
            Some(Ok(img)) => {
                egui::Grid::new("fwinfo").num_columns(2).spacing([12.0, 2.0]).show(ui, |ui| {
                    ui.weak("firmware");
                    ui.label(format!("{} {}", img.update.project, img.update.version));
                    ui.end_row();
                    ui.weak("for");
                    ui.label(match &img.update.target {
                        ota::Target::Chip(c) => c.clone(),
                        ota::Target::Sim { board } => format!("the {board} simulator"),
                    });
                    ui.end_row();
                    if let Some((fi, _)) = &img.usb {
                        ui.weak("built");
                        ui.label(format!("{} with ESP-IDF {}", fi.build_date, fi.idf_version));
                        ui.end_row();
                    }
                    ui.weak("size");
                    ui.label(format!("{} KB", img.update.data.len().div_ceil(1024)));
                    ui.end_row();
                    ui.weak("on device");
                    ui.label(match (field("ver"), field("slot")) {
                        (Some(v), Some(s)) => format!("luaboard {v} ({s})"),
                        (Some(v), None) => format!("luaboard {v}"),
                        _ => "unknown (not connected)".into(),
                    });
                    ui.end_row();
                });
                fits = if connected {
                    ota::check_for(&img.update, &info).map_err(|e| format!("{e:#}"))
                } else {
                    Err("not connected to a device".into())
                };
                has_usb = img.usb.is_some();
            }
        }

        ui.separator();
        // Over the link: USB, Wi-Fi or the simulator, whatever is connected.
        let update_label = match self.connected_port.as_deref() {
            None => "Update".to_string(),
            Some(_) if on_sim => "Update the simulator".to_string(),
            Some(p) => match luaboard::wifi::parse_tcp_port(p) {
                Some((host, _)) => format!("Update over Wi-Fi ({host})"),
                None => format!("Update over USB ({})", short_port(p)),
            },
        };
        ui.horizontal(|ui| {
            let b = ui.add_enabled(fits.is_ok() && !self.flash.busy, egui::Button::new(update_label)).on_hover_text(
                "The device writes it into its free firmware slot and reboots into it. Apps, settings and \
                     secrets stay. If the new firmware keeps crashing, the device goes back to the old one.",
            );
            if b.clicked() {
                let img = match &self.flash.image {
                    Some(Ok(i)) => Some(i.update.clone()),
                    _ => None,
                };
                if let Some(img) = img {
                    self.flash.busy = true;
                    self.flash.progress = 0.0;
                    self.flash.status = "starting…".into();
                    self.preview.pending = false;
                    self.preview.remote = Remote::default();
                    self.send(Cmd::Update(img));
                }
            }
            if self.flash.busy {
                ui.spinner();
            }
        });
        if let Err(e) = &fits {
            if !e.is_empty() {
                ui.colored_label(egui::Color32::from_rgb(230, 180, 90), e);
            }
        }

        // Over USB with the ROM bootloader: works on any firmware, even a broken one.
        let over_wifi = luaboard::wifi::parse_tcp_port(&self.port).is_some();
        let sim_port = luaboard::is_simulator_port(&self.port);
        ui.horizontal(|ui| {
            let can = has_usb && !over_wifi && !sim_port && !self.flash.busy && !self.port.trim().is_empty();
            if ui
                .add_enabled(can, egui::Button::new(format!("Flash with the bootloader ({})", short_port(&self.port))))
                .on_hover_text(
                    "Writes the image with the chip's bootloader: for a first install, or a device whose firmware \
                     doesn't answer. Installed apps and settings are kept.",
                )
                .clicked()
            {
                let bytes = match &self.flash.image {
                    Some(Ok(i)) => i.usb.as_ref().map(|(_, b)| b.clone()),
                    _ => None,
                };
                if let Some(bytes) = bytes {
                    self.flash.busy = true;
                    self.flash.progress = 0.0;
                    self.flash.status = "starting…".into();
                    self.preview.pending = false;
                    self.preview.remote = Remote::default();
                    self.send(Cmd::Flash { port: self.port.clone(), image: bytes });
                }
            }
        });
        if self.flash.busy || self.flash.progress > 0.0 {
            ui.add(egui::ProgressBar::new(self.flash.progress).show_percentage());
        }
        if !self.flash.status.is_empty() {
            ui.small(&self.flash.status);
        }
        ui.add_space(4.0);
        ui.weak("If entering download mode fails: hold the CoreS3's reset button until the green LED lights, then flash again.");
    }
}

/// A port as the buttons name it: a serial device's file name, else the port.
fn short_port(port: &str) -> String {
    std::path::Path::new(port).file_name().map_or(port.to_string(), |n| n.to_string_lossy().into_owned())
}
