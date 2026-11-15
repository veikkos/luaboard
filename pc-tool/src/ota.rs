//! Firmware updates over the device's own link (USB, Wi-Fi or the simulator's
//! serial port): the `OTA` command. The device writes the image into its free
//! app slot and reboots into it; data partitions (apps, settings, secrets)
//! are never touched. Unlike [`crate::firmware::flash`] this needs no
//! bootloader mode, works over Wi-Fi, and updates a running simulator.
//!
//! A device takes the app part of an ESP image (the merged .bin from
//! the `fw` build, or the app-only one). The simulator takes a simulator
//! executable for the same board (build/<board>/esp32-sim), which carries a tag with its
//! project, version and board (platform/esp32-mock/src/mock_ota.c).

use crate::{firmware, AppState, Device, Msg};
use anyhow::{bail, Context, Result};
use std::time::{Duration, Instant};

/// What an update image is for.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Target {
    /// A device with this chip (`ESP32-S3`).
    Chip(String),
    /// The simulator of this board.
    Sim { board: String },
}

#[derive(Debug, Clone)]
pub struct UpdateImage {
    pub target: Target,
    pub project: String,
    pub version: String,
    /// What the device is sent.
    pub data: Vec<u8>,
}

impl UpdateImage {
    /// "luaboard 94bf241 for ESP32-S3" and the like.
    pub fn describe(&self) -> String {
        let what = match &self.target {
            Target::Chip(c) => c.clone(),
            Target::Sim { board } => format!("the {board} simulator"),
        };
        format!("{} {} for {what}, {} KB", self.project, self.version, self.data.len().div_ceil(1024))
    }
}

const SIM_TAG: &[u8] = b"esp32-sim-image project=";

/// The simulator image tag's fields, or None if `bytes` isn't a simulator executable.
fn sim_tag(bytes: &[u8]) -> Option<(String, String, String)> {
    // Mach-O (macOS), ELF (Linux) or PE (Windows): the simulator checks it's its own kind.
    let exe = bytes.starts_with(&[0xcf, 0xfa, 0xed, 0xfe]) || bytes.starts_with(b"\x7fELF") || bytes.starts_with(b"MZ");
    if !exe {
        return None;
    }
    let mut from = 0;
    while let Some(pos) = bytes[from..].windows(SIM_TAG.len()).position(|w| w == SIM_TAG) {
        let at = from + pos;
        let end = bytes[at..].iter().take(160).position(|&b| b == 0).map_or(bytes.len().min(at + 160), |e| at + e);
        let tag = String::from_utf8_lossy(&bytes[at..end]);
        let field = |k: &str| crate::kv_field(&tag, k).map(str::to_string);
        if let (Some(p), Some(v), Some(b)) = (field("project"), field("ver"), field("board")) {
            return Some((p, v, b));
        }
        from = at + 1;
    }
    None
}

/// Read an update image: an ESP firmware image or a simulator executable.
pub fn image(bytes: &[u8]) -> Result<UpdateImage> {
    if let Some((project, version, board)) = sim_tag(bytes) {
        return Ok(UpdateImage { target: Target::Sim { board }, project, version, data: bytes.to_vec() });
    }
    let (info, app) = firmware::app_image(bytes)
        .context("not a firmware image (a .bin from the `fw` build) or a simulator executable")?;
    Ok(UpdateImage {
        target: Target::Chip(info.chip.to_string()),
        project: info.project,
        version: info.version,
        data: app.to_vec(),
    })
}

/// Refuse an image that isn't for the device that gave this INFO.
pub fn check_for(img: &UpdateImage, info: &str) -> Result<()> {
    if img.project != "luaboard" {
        bail!("this is '{}' firmware, not luaboard", img.project);
    }
    let Some(target) = crate::kv_field(info, "target") else {
        bail!("the device's firmware can't take updates over its link: flash it over USB once (luaboard flash)");
    };
    match (&img.target, target) {
        (Target::Sim { board }, "sim") => {
            let dev_board = crate::kv_field(info, "board").unwrap_or("?");
            if board != dev_board {
                bail!("the image is a simulator of {board}, this one simulates {dev_board}");
            }
        }
        (Target::Sim { .. }, _) => bail!("this is a simulator executable; the device needs a .bin from the `fw` build"),
        (Target::Chip(_), "sim") => {
            bail!("the simulator takes a simulator executable (build/<board>/esp32-sim), not a device image")
        }
        (Target::Chip(chip), t) => {
            if chip.replace('-', "").to_lowercase() != t.to_lowercase() {
                bail!("the image is built for {chip}, the device is {t}");
            }
        }
    }
    Ok(())
}

/// Progress of [`update`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Event {
    Sending {
        done: usize,
        total: usize,
    },
    /// All sent; the device checks the image.
    Checking,
    /// The device took it (its version) and reboots.
    Installed(String),
    /// Back up after the reboot, running the update: its INFO.
    Rebooted(String),
}

/// Send `img` to the device with OTA and wait for it to reboot into it. Over
/// USB to a real board or over Wi-Fi the link goes away on the reboot and is
/// opened again on `port`; the simulator keeps its serial port. Fails if the
/// device came back on its old firmware: an update that crashes early in its
/// boot is rolled back by the bootloader. Refuses while an app has uploads
/// not installed, which the reboot would drop. Returns the link to the device,
/// open again, and its INFO after the reboot.
pub fn update(
    dev: Device,
    port: &str,
    baud: u32,
    img: &UpdateImage,
    on: &mut dyn FnMut(Event),
    a: &mut dyn FnMut(Msg),
) -> Result<(Device, String)> {
    let mut dev = dev;
    let before = dev.info(a)?;
    // Uploads not installed live in RAM: the reboot would drop them.
    let unsaved: Vec<String> =
        dev.apps(a)?.into_iter().filter(|app| app.state != AppState::Installed).map(|app| app.name).collect();
    if !unsaved.is_empty() {
        bail!(
            "the reboot would lose uploads not installed yet, of {}: install them first (or uninstall them)",
            unsaved.join(", ")
        );
    }
    let version = dev.ota(
        &img.data,
        &mut |done, total| {
            if done < total {
                on(Event::Sending { done, total })
            } else {
                on(Event::Checking)
            }
        },
        a,
    )?;
    on(Event::Installed(version));

    // The reboot: a "boot" event on a link that stays (simulator), or the
    // link going away and coming back (USB re-enumerates, Wi-Fi reconnects).
    let deadline = Instant::now() + Duration::from_secs(45);
    let mut link = Some(dev);
    let mut retry_ms = 50;
    let mut dev = loop {
        if Instant::now() > deadline {
            bail!("the device didn't come back after the update; check it and reconnect");
        }
        match link.take() {
            Some(mut d) => {
                // An error: the link is gone, open it again below.
                if let Ok(line) = d.read_line(Duration::from_millis(200)) {
                    let msg = line.filter(|l| !l.is_empty()).map(|l| Msg::parse(&l));
                    let booted = matches!(&msg, Some(Msg::Evt(e)) if e.starts_with("boot "));
                    if let Some(m) = msg {
                        a(m);
                    }
                    if booted {
                        break d;
                    }
                    link = Some(d);
                }
            }
            None => {
                // Soon at first (a device on this network is back within a
                // second), then every half second.
                std::thread::sleep(Duration::from_millis(retry_ms));
                retry_ms = (retry_ms * 2).min(500);
                if let Ok(d) = Device::open(port, baud) {
                    break d;
                }
            }
        }
    };
    let info = dev.info(a)?;
    if crate::kv_field(&info, "ota") == Some("rolledback")
        || crate::kv_field(&info, "slot") == crate::kv_field(&before, "slot")
    {
        bail!(
            "the new firmware crashed at boot and the device went back to {}",
            crate::kv_field(&info, "ver").unwrap_or("the old one")
        );
    }
    on(Event::Rebooted(info.clone()));
    Ok((dev, info))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sim_exe(tag: &str) -> Vec<u8> {
        let mut b = vec![0xcf, 0xfa, 0xed, 0xfe, 1, 2, 3];
        b.extend_from_slice(b"esp32-sim-image project=\0junk");
        b.extend_from_slice(tag.as_bytes());
        b.push(0);
        b
    }

    #[test]
    fn reads_simulator_tag_skipping_false_hits() {
        let img =
            image(&sim_exe("esp32-sim-image project=luaboard ver=abc123 board=m5stack-cores3 fail=none ")).unwrap();
        assert_eq!(img.target, Target::Sim { board: "m5stack-cores3".into() });
        assert_eq!(img.version, "abc123");
        assert_eq!(img.project, "luaboard");
    }

    #[test]
    fn rejects_garbage() {
        assert!(image(b"hello world").is_err());
        // Tag but not an executable.
        assert!(image(b"esp32-sim-image project=luaboard ver=1 board=x").is_err());
    }

    #[test]
    fn checks_target() {
        let sim = image(&sim_exe("esp32-sim-image project=luaboard ver=1 board=m5stack-cores3 ")).unwrap();
        let sim_info = "fw=luaboard ver=1 board=m5stack-cores3 target=sim slot=ota_0 ota=ok";
        let dev_info = "fw=luaboard ver=1 board=m5stack-cores3 target=esp32s3 slot=ota_0 ota=ok";
        assert!(check_for(&sim, sim_info).is_ok());
        assert!(check_for(&sim, dev_info).is_err());
        assert!(check_for(&sim, "fw=luaboard ver=1 board=other target=sim").is_err());
        assert!(check_for(&sim, "fw=luaboard ver=0.1.0 board=m5stack-cores3").is_err(), "old firmware: no OTA");
        let chip = UpdateImage {
            target: Target::Chip("ESP32-S3".into()),
            project: "luaboard".into(),
            version: "1".into(),
            data: vec![],
        };
        assert!(check_for(&chip, dev_info).is_ok());
        assert!(check_for(&chip, sim_info).is_err());
        let other = UpdateImage { project: "blink".into(), ..chip };
        assert!(check_for(&other, dev_info).is_err());
    }
}
