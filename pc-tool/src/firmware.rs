//! Firmware images and flashing over USB (ESP32-S3 ROM bootloader).
//!
//! The firmware build (the `fw` target) produces one merged image that is written
//! at flash address 0: bootloader at 0x0, partition table at 0x8000, the app
//! at the offset the partition table gives (0x20000 for luaboard: the first
//! of two OTA slots).
//!
//! [`inspect`] validates such an image offline (no device needed);
//! [`flash`] writes it with `espflash`, which puts the chip into download
//! mode via the USB-Serial/JTAG reset lines, flashes, verifies and resets.

use anyhow::{bail, Context, Result};
use std::time::Duration;

/// ESP image header magic byte (bootloader and app images).
const IMAGE_MAGIC: u8 = 0xE9;
/// `chip_id` field values in the extended image header.
const CHIP_ID_ESP32S3: u16 = 9;
const PARTITION_TABLE_OFFSET: usize = 0x8000;
const PARTITION_MAGIC: [u8; 2] = [0xAA, 0x50];
/// `esp_app_desc_t.magic_word`.
const APP_DESC_MAGIC: u32 = 0xABCD_5432;

/// What a firmware image contains.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ImageInfo {
    pub chip: &'static str,
    pub size: usize,
    pub app_offset: u32,
    pub project: String,
    pub version: String,
    pub idf_version: String,
    pub build_date: String,
}

fn chip_name(id: u16) -> &'static str {
    match id {
        0 => "ESP32",
        2 => "ESP32-S2",
        5 => "ESP32-C3",
        9 => "ESP32-S3",
        12 => "ESP32-C2",
        13 => "ESP32-C6",
        16 => "ESP32-H2",
        _ => "unknown",
    }
}

fn cstr(b: &[u8]) -> String {
    let end = b.iter().position(|&c| c == 0).unwrap_or(b.len());
    String::from_utf8_lossy(&b[..end]).into_owned()
}

fn u32le(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}

/// Validate a merged image and read its app description.
pub fn inspect(img: &[u8]) -> Result<ImageInfo> {
    if img.len() < PARTITION_TABLE_OFFSET + 64 || img[0] != IMAGE_MAGIC {
        bail!("not a merged ESP firmware image (expected a bootloader at offset 0; use the .bin from the `fw` build)");
    }
    // Image header (24 bytes): ..., chip_id (u16) at offset 12.
    let chip_id = u16::from_le_bytes([img[12], img[13]]);
    let chip = chip_name(chip_id);

    // Partition table: 32-byte entries, find the first app partition (type 0).
    let mut app_offset = None;
    let mut at = PARTITION_TABLE_OFFSET;
    while at + 32 <= img.len() && img[at..at + 2] == PARTITION_MAGIC {
        let (ptype, offset) = (img[at + 2], u32le(img, at + 4));
        if ptype == 0 && app_offset.is_none() {
            app_offset = Some(offset);
        }
        at += 32;
    }
    let app_offset = app_offset.context("no app partition in the image's partition table")?;
    let app = app_offset as usize;
    if app + 0x20 + 256 > img.len() || img[app] != IMAGE_MAGIC {
        bail!("no app image at 0x{app_offset:x} (image too short or not merged with the app?)");
    }
    let mut info = app_desc(img, app).with_context(|| format!("app at 0x{app_offset:x}"))?;
    info.chip = chip;
    info.size = img.len();
    info.app_offset = app_offset;
    Ok(info)
}

/// The app image's description at `app`: a 24-byte image header and an
/// 8-byte first segment header, then esp_app_desc_t.
fn app_desc(img: &[u8], app: usize) -> Result<ImageInfo> {
    let d = app + 24 + 8;
    if d + 144 > img.len() || u32le(img, d) != APP_DESC_MAGIC {
        bail!("no ESP-IDF app description");
    }
    Ok(ImageInfo {
        chip: chip_name(u16::from_le_bytes([img[app + 12], img[app + 13]])),
        size: img.len(),
        app_offset: 0,
        version: cstr(&img[d + 16..d + 48]),
        project: cstr(&img[d + 48..d + 80]),
        build_date: format!("{} {}", cstr(&img[d + 96..d + 112]), cstr(&img[d + 80..d + 96])),
        idf_version: cstr(&img[d + 112..d + 144]),
    })
}

/// An app image on its own (as an update carries it): the merged image's
/// part from the first app slot on, or an app-only .bin (`build/idf-*/luaboard.bin`).
/// Returns its description and the app bytes.
pub fn app_image(img: &[u8]) -> Result<(ImageInfo, &[u8])> {
    if img.first() != Some(&IMAGE_MAGIC) {
        bail!("not an ESP firmware image");
    }
    if let Ok(info) = app_desc(img, 0) {
        return Ok((info, img));
    }
    let info = inspect(img)?;
    let app = &img[info.app_offset as usize..];
    Ok((info, app))
}

/// Data partition subtypes.
const SUBTYPE_OTA_DATA: u8 = 0x00;
const SUBTYPE_PHY: u8 = 0x01;

/// Size of the partition table area (ESP-IDF default, includes the MD5 entry).
const PARTITION_TABLE_LEN: usize = 0xC00;

#[derive(Debug, Clone)]
struct Partition {
    ptype: u8,
    subtype: u8,
    offset: u32,
    size: u32,
    label: String,
}

fn partitions(img: &[u8]) -> Vec<Partition> {
    let mut out = Vec::new();
    let mut at = PARTITION_TABLE_OFFSET;
    while at + 32 <= img.len() && img[at..at + 2] == PARTITION_MAGIC {
        out.push(Partition {
            ptype: img[at + 2],
            subtype: img[at + 3],
            offset: u32le(img, at + 4),
            size: u32le(img, at + 8),
            label: cstr(&img[at + 12..at + 28]),
        });
        at += 32;
    }
    out
}

/// One part of the merged image that gets written to flash.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Region {
    pub name: &'static str,
    pub offset: u32,
    pub len: usize,
}

/// What to write from a merged image: the bootloader, the partition table
/// area and the app, never the data partitions (NVS: settings, Wi-Fi, pairing,
/// secrets; phy_init: RF calibration; apps). The merged .bin fills the gaps
/// with 0xFF, so writing it whole would erase them.
pub fn flash_regions(img: &[u8]) -> Result<Vec<Region>> {
    let info = inspect(img)?;
    // Bootloader: up to its last non-0xFF byte (4-byte aligned).
    let boot_end = img[..PARTITION_TABLE_OFFSET].iter().rposition(|&b| b != 0xFF).map_or(0, |i| i + 1);
    let boot_len = (boot_end + 3) & !3;
    let app_start = info.app_offset as usize;
    let regions = vec![
        Region { name: "bootloader", offset: 0, len: boot_len },
        Region { name: "partition table", offset: PARTITION_TABLE_OFFSET as u32, len: PARTITION_TABLE_LEN },
        Region { name: "app", offset: info.app_offset, len: img.len() - app_start },
    ];
    // otadata, as the image has it (blank: boot the first slot). Otherwise a
    // device that took an update over Wi-Fi would keep booting that slot and
    // never run what was just flashed.
    let parts = partitions(img);
    let mut regions = regions;
    for p in parts.iter().filter(|p| p.ptype == 1 && p.subtype == SUBTYPE_OTA_DATA) {
        if p.offset as usize + p.size as usize > app_start {
            bail!("otadata at 0x{:x} isn't before the app", p.offset);
        }
        regions.push(Region { name: "otadata", offset: p.offset, len: p.size as usize });
    }
    regions.sort_by_key(|r| r.offset);
    // Refuse if any region would touch a data partition (but otadata its own).
    for p in parts.iter().filter(|p| p.ptype == 1) {
        let (ps, pe) = (p.offset as usize, p.offset as usize + p.size as usize);
        for r in regions.iter().filter(|r| !(r.name == "otadata" && r.offset == p.offset)) {
            let (rs, re) = (r.offset as usize, r.offset as usize + r.len);
            if rs < pe && ps < re {
                bail!("refusing to flash: {} would overwrite data partition '{}'", r.name, p.label);
            }
        }
    }
    Ok(regions)
}

/// Refuse images that don't belong on this device.
pub fn check_for_device(info: &ImageInfo) -> Result<()> {
    if info.chip != chip_name(CHIP_ID_ESP32S3) {
        bail!("image is built for {}, but the CoreS3 is an ESP32-S3", info.chip);
    }
    Ok(())
}

/// Progress reported while flashing.
#[derive(Debug, Clone)]
pub enum FlashEvent {
    Connecting,
    Connected { chip: String },
    Writing { done: usize, total: usize },
    Verifying,
    Done,
}

/// Reports progress over all regions as one bar. espflash reports per region
/// in its own units, so each region's progress is scaled to its byte size.
struct Progress<'a> {
    sizes: Vec<usize>,
    index: usize,
    done_before: usize,
    region_units: usize,
    on: &'a mut dyn FnMut(FlashEvent),
}

impl Progress<'_> {
    fn total(&self) -> usize {
        self.sizes.iter().sum()
    }
}

impl espflash::target::ProgressCallbacks for Progress<'_> {
    fn init(&mut self, _addr: u32, units: usize) {
        self.region_units = units.max(1);
        let total = self.total();
        (self.on)(FlashEvent::Writing { done: self.done_before, total });
    }
    fn update(&mut self, current: usize) {
        let size = self.sizes.get(self.index).copied().unwrap_or(0);
        let done = self.done_before + size * current.min(self.region_units) / self.region_units;
        let total = self.total();
        (self.on)(FlashEvent::Writing { done, total });
    }
    fn verifying(&mut self) {
        (self.on)(FlashEvent::Verifying);
    }
    fn finish(&mut self, _skipped: bool) {
        self.done_before += self.sizes.get(self.index).copied().unwrap_or(0);
        self.index += 1;
    }
}

/// Flash the bootloader, partition table and app from a merged image (see
/// [`flash_regions`]; data partitions such as NVS are left alone) and reset
/// the chip into the new firmware.
///
/// `port` must be the real device (e.g. /dev/cu.usbmodem1101); nothing else
/// may have it open. On failure, holding the reset button for a few seconds
/// puts the CoreS3 into download mode manually.
pub fn flash(port: &str, image: &[u8], on: &mut dyn FnMut(FlashEvent)) -> Result<()> {
    let info = inspect(image)?;
    check_for_device(&info)?;
    let regions = flash_regions(image)?;
    let mut flasher = connect(port, on)?;

    let segments: Vec<espflash::image_format::Segment> = regions
        .iter()
        .map(|r| espflash::image_format::Segment {
            addr: r.offset,
            data: std::borrow::Cow::Borrowed(&image[r.offset as usize..r.offset as usize + r.len]),
        })
        .collect();
    let mut progress = Progress {
        sizes: regions.iter().map(|r| r.len).collect(),
        index: 0,
        done_before: 0,
        region_units: 1,
        on: &mut *on,
    };
    flasher.write_bins_to_flash(&segments, &mut progress).context("flashing failed")?;
    flasher.connection().reset().context("reset after flashing failed")?;
    on(FlashEvent::Done);
    Ok(())
}

/// The data partitions [`erase_data`] wipes: all but RF calibration
/// (phy_init), which the chip would only redo, and otadata, which says which
/// firmware slot runs (blank, it would boot the other one).
fn data_to_erase(table: &[Partition]) -> Vec<&Partition> {
    table.iter().filter(|p| p.ptype == 1 && p.subtype != SUBTYPE_PHY && p.subtype != SUBTYPE_OTA_DATA).collect()
}

/// Erase the data partitions of the device on `port` (USB): settings, Wi-Fi,
/// pairing, secrets (NVS) and the installed apps. The firmware stays; it starts
/// as new. Unlike FORMAT and WIFI OFF, which only mark NVS entries deleted,
/// this clears the flash itself, so nothing can be read back from it.
/// Calls `erasing` with each partition's label and size before erasing it.
pub fn erase_data(port: &str, on: &mut dyn FnMut(FlashEvent), erasing: &mut dyn FnMut(&str, u32)) -> Result<()> {
    let mut flasher = connect(port, on)?;
    // The device's own partition table, read back from flash.
    let tmp = std::env::temp_dir().join(format!("luaboard-pt-{}.bin", std::process::id()));
    let read = flasher.read_flash(PARTITION_TABLE_OFFSET as u32, PARTITION_TABLE_LEN as u32, 0x1000, 64, tmp.clone());
    let table = std::fs::read(&tmp);
    let _ = std::fs::remove_file(&tmp);
    read.context("reading the partition table failed")?;
    let mut img = vec![0xFF; PARTITION_TABLE_OFFSET];
    img.extend_from_slice(&table.context("reading the partition table failed")?);
    let parts = partitions(&img);
    let erase = data_to_erase(&parts);
    if !erase.iter().any(|p| p.subtype == 0x02) {
        bail!("no NVS partition in the device's partition table: is this luaboard firmware?");
    }
    for p in erase {
        erasing(&p.label, p.size);
        flasher.erase_region(p.offset, p.size).with_context(|| format!("erasing '{}' failed", p.label))?;
    }
    flasher.connection().reset().context("reset after erasing failed")?;
    on(FlashEvent::Done);
    Ok(())
}

/// Put the ESP32-S3 on `port` (USB) into download mode and connect to it.
fn connect(port: &str, on: &mut dyn FnMut(FlashEvent)) -> Result<espflash::flasher::Flasher> {
    use espflash::connection::{Connection, ResetAfterOperation, ResetBeforeOperation};
    use espflash::flasher::Flasher;
    use serialport::{SerialPortType, UsbPortInfo};

    if crate::wifi::parse_tcp_port(port).is_some() {
        bail!("{port} is a Wi-Fi connection: this needs the USB cable");
    }
    if crate::is_simulator_port(port) {
        bail!("{port} is the simulator: it runs the firmware it was built with, there is no flash to write");
    }

    // espflash picks the USB-Serial/JTAG reset sequence from the USB VID/PID.
    let usb = serialport::available_ports()
        .unwrap_or_default()
        .into_iter()
        .find(|p| p.port_name == port)
        .and_then(|p| match p.port_type {
            SerialPortType::UsbPort(u) => Some(u),
            _ => None,
        })
        .unwrap_or(UsbPortInfo { vid: 0, pid: 0, serial_number: None, manufacturer: None, product: None });

    on(FlashEvent::Connecting);
    let serial = serialport::new(port, 115_200)
        .flow_control(serialport::FlowControl::None)
        .timeout(Duration::from_secs(3))
        .open_native()
        .with_context(|| format!("cannot open {port}"))?;
    let conn =
        Connection::new(serial, usb, ResetAfterOperation::HardReset, ResetBeforeOperation::DefaultReset, 115_200);
    let flasher = Flasher::connect(conn, true, true, true, None, None).map_err(|e| {
        anyhow::anyhow!(
            "could not enter the bootloader: {e}\n\
             Hold the reset button until the green LED lights (download mode) and try again."
        )
    })?;
    on(FlashEvent::Connected { chip: flasher.chip().to_string() });
    if flasher.chip() != espflash::target::Chip::Esp32s3 {
        bail!("connected chip is {}, expected an ESP32-S3", flasher.chip());
    }
    Ok(flasher)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn erase_wipes_nvs_and_apps_not_calibration_or_firmware() {
        // luaboard's partitions.csv: nvs, phy_init, otadata, ota_0, ota_1, apps (littlefs).
        let mut img = vec![0xFF; PARTITION_TABLE_OFFSET + PARTITION_TABLE_LEN];
        let rows: [(u8, u8, u32, u32, &str); 6] = [
            (1, 0x02, 0x9000, 0x10000, "nvs"),
            (1, 0x01, 0x19000, 0x1000, "phy_init"),
            (1, 0x00, 0x1A000, 0x2000, "otadata"),
            (0, 0x10, 0x20000, 0x300000, "ota_0"),
            (0, 0x11, 0x320000, 0x300000, "ota_1"),
            (1, 0x83, 0x620000, 0x9E0000, "apps"),
        ];
        for (i, (t, st, off, size, label)) in rows.iter().enumerate() {
            let at = PARTITION_TABLE_OFFSET + 32 * i;
            img[at..at + 2].copy_from_slice(&PARTITION_MAGIC);
            img[at + 2] = *t;
            img[at + 3] = *st;
            img[at + 4..at + 8].copy_from_slice(&off.to_le_bytes());
            img[at + 8..at + 12].copy_from_slice(&size.to_le_bytes());
            img[at + 12..at + 28].fill(0);
            img[at + 12..at + 12 + label.len()].copy_from_slice(label.as_bytes());
        }
        let parts = partitions(&img);
        let labels: Vec<&str> = data_to_erase(&parts).iter().map(|p| p.label.as_str()).collect();
        assert_eq!(labels, ["nvs", "apps"]);
    }

    /// A minimal merged image: bootloader header, partition table, app with app desc.
    fn fake_image(chip_id: u16, app_offset: u32) -> Vec<u8> {
        let mut img = vec![0xFF; app_offset as usize + 0x1000];
        img[0] = IMAGE_MAGIC;
        img[12..14].copy_from_slice(&chip_id.to_le_bytes());
        let pt = PARTITION_TABLE_OFFSET;
        // nvs (data) then factory (app)
        img[pt..pt + 2].copy_from_slice(&PARTITION_MAGIC);
        img[pt + 2] = 1;
        img[pt + 4..pt + 8].copy_from_slice(&0x9000u32.to_le_bytes());
        let e = pt + 32;
        img[e..e + 2].copy_from_slice(&PARTITION_MAGIC);
        img[e + 2] = 0;
        img[e + 4..e + 8].copy_from_slice(&app_offset.to_le_bytes());
        let a = app_offset as usize;
        img[a] = IMAGE_MAGIC;
        let d = a + 32;
        img[d..d + 4].copy_from_slice(&APP_DESC_MAGIC.to_le_bytes());
        for (off, s) in [(16, "0.1.0"), (48, "luaboard"), (80, "12:00:00"), (96, "Nov  5 2026"), (112, "v6.0.3")] {
            img[d + off..d + off + 32.min(s.len() + 1)].fill(0);
            img[d + off..d + off + s.len()].copy_from_slice(s.as_bytes());
        }
        img
    }

    #[test]
    fn reads_app_description() {
        let info = inspect(&fake_image(CHIP_ID_ESP32S3, 0x20000)).unwrap();
        assert_eq!(info.chip, "ESP32-S3");
        assert_eq!(info.app_offset, 0x20000);
        assert_eq!(info.project, "luaboard");
        assert_eq!(info.version, "0.1.0");
        assert_eq!(info.idf_version, "v6.0.3");
        assert!(check_for_device(&info).is_ok());
    }

    /// Same layout as app/idf/partitions.csv: nvs 0x9000+0x10000, phy 0x19000, app 0x20000.
    fn image_with_nvs() -> Vec<u8> {
        let mut img = fake_image(CHIP_ID_ESP32S3, 0x20000);
        let pt = PARTITION_TABLE_OFFSET;
        img[pt + 8..pt + 12].copy_from_slice(&0x10000u32.to_le_bytes()); // nvs size
        img[0x100..0x200].fill(0x42); // some bootloader bytes
        img
    }

    #[test]
    fn flash_resets_otadata_so_the_flashed_slot_boots() {
        let mut img = image_with_nvs();
        let e = PARTITION_TABLE_OFFSET + 64; // after nvs and the app
        img[e..e + 2].copy_from_slice(&PARTITION_MAGIC);
        img[e + 2] = 1;
        img[e + 3] = SUBTYPE_OTA_DATA;
        img[e + 4..e + 8].copy_from_slice(&0x1A000u32.to_le_bytes());
        img[e + 8..e + 12].copy_from_slice(&0x2000u32.to_le_bytes());
        let regions = flash_regions(&img).unwrap();
        let ota = regions.iter().find(|r| r.name == "otadata").expect("otadata written");
        assert_eq!((ota.offset, ota.len), (0x1A000, 0x2000));
        assert!(img[0x1A000..0x1C000].iter().all(|&b| b == 0xFF), "blank: boot the first slot");
    }

    #[test]
    fn flash_regions_never_touch_data_partitions() {
        let img = image_with_nvs();
        let regions = flash_regions(&img).unwrap();
        let names: Vec<_> = regions.iter().map(|r| r.name).collect();
        assert_eq!(names, ["bootloader", "partition table", "app"]); // no otadata in this table
        for r in &regions {
            let (a, b) = (r.offset as usize, r.offset as usize + r.len);
            assert!(b <= 0x9000 || a >= 0x19000, "{} [{a:#x},{b:#x}) overlaps NVS", r.name);
        }
        let pt = regions.iter().find(|r| r.name == "partition table").unwrap();
        assert_eq!((pt.offset, pt.len), (0x8000, 0xC00), "whole table area, so old entries are cleared");
        let app = regions.iter().find(|r| r.name == "app").unwrap();
        assert_eq!((app.offset as usize, app.offset as usize + app.len), (0x20000, img.len()));
        let boot = &regions[0];
        assert!(boot.len >= 0x200 && boot.len <= 0x8000 && boot.len.is_multiple_of(4));
    }

    #[test]
    fn rejects_wrong_chip_and_garbage() {
        let info = inspect(&fake_image(0, 0x10000)).unwrap();
        assert_eq!(info.chip, "ESP32");
        assert!(check_for_device(&info).is_err());
        assert!(inspect(b"print('not firmware')").is_err());
        let mut img = fake_image(CHIP_ID_ESP32S3, 0x20000);
        img[0x20000] = 0; // app missing
        assert!(inspect(&img).is_err());
    }
}
