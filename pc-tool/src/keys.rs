//! Pairing keys for the Wi-Fi link, kept on this computer.
//!
//! `luaboard pair` (over USB) gets the device's 256-bit key and stores it
//! here under the key's id, which the device names when a Wi-Fi connection
//! starts. Anyone with this file can control the device over Wi-Fi, so it is
//! readable only by you. File: `$LUABOARD_KEYS`, else
//! `~/.config/luaboard/wifi-keys` (Windows: `%APPDATA%\luaboard\wifi-keys`,
//! in the profile only you can read), one line per device: `<id> <key hex>`.

use anyhow::{bail, Context, Result};
use std::path::PathBuf;

pub const KEY_LEN: usize = 32;

pub fn path() -> PathBuf {
    if let Some(p) = std::env::var_os("LUABOARD_KEYS") {
        return PathBuf::from(p);
    }
    config_dir().join("luaboard").join("wifi-keys")
}

/// Where per-user settings go. Never the current folder: a key file there
/// could end up shared or committed.
#[cfg(windows)]
fn config_dir() -> PathBuf {
    let env = |v| std::env::var_os(v).filter(|s| !s.is_empty()).map(PathBuf::from);
    env("APPDATA")
        .or_else(|| env("USERPROFILE").map(|h| h.join("AppData").join("Roaming")))
        .unwrap_or_else(|| PathBuf::from(r"C:\ProgramData"))
}

#[cfg(not(windows))]
fn config_dir() -> PathBuf {
    let home = std::env::var_os("HOME").filter(|s| !s.is_empty()).map(PathBuf::from);
    home.unwrap_or_else(|| PathBuf::from(".")).join(".config")
}

fn valid_id(id: &str) -> bool {
    id.len() == 16 && id.bytes().all(|b| b.is_ascii_hexdigit())
}

pub(crate) fn unhex(h: &str) -> Option<Vec<u8>> {
    if !h.len().is_multiple_of(2) || !h.bytes().all(|b| b.is_ascii_hexdigit()) {
        return None;
    }
    (0..h.len()).step_by(2).map(|i| u8::from_str_radix(&h[i..i + 2], 16).ok()).collect()
}

pub(crate) fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

fn lines() -> Vec<(String, String)> {
    let Ok(text) = std::fs::read_to_string(path()) else { return Vec::new() };
    text.lines()
        .filter_map(|l| {
            let mut it = l.split_whitespace();
            Some((it.next()?.to_ascii_lowercase(), it.next()?.to_string()))
        })
        .collect()
}

fn save(all: &[(String, String)]) -> Result<()> {
    let p = path();
    if let Some(dir) = p.parent().filter(|d| !d.as_os_str().is_empty()) {
        std::fs::create_dir_all(dir).with_context(|| format!("cannot create {}", dir.display()))?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let _ = std::fs::set_permissions(dir, std::fs::Permissions::from_mode(0o700));
        }
    }
    let tmp = p.with_extension("tmp");
    let text: String = all.iter().map(|(id, k)| format!("{id} {k}\n")).collect();
    {
        use std::io::Write;
        let mut o = std::fs::OpenOptions::new();
        o.write(true).create(true).truncate(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            o.mode(0o600);
        }
        let mut f = o.open(&tmp).with_context(|| format!("cannot write {}", tmp.display()))?;
        f.write_all(text.as_bytes())?;
        f.sync_all()?;
    }
    std::fs::rename(&tmp, &p).with_context(|| format!("cannot write {}", p.display()))?;
    Ok(())
}

/// The key for a device's key id, if this computer was paired with it.
pub fn get(id: &str) -> Option<[u8; KEY_LEN]> {
    let id = id.to_ascii_lowercase();
    lines().into_iter().find(|(i, _)| *i == id).and_then(|(_, k)| unhex(&k)?.try_into().ok())
}

/// Remember a device's key (replacing one with the same id).
pub fn put(id: &str, key: &[u8; KEY_LEN]) -> Result<()> {
    if !valid_id(id) {
        bail!("bad key id from device: {id}");
    }
    let id = id.to_ascii_lowercase();
    let mut all: Vec<_> = lines().into_iter().filter(|(i, _)| *i != id).collect();
    all.push((id, hex(key)));
    save(&all)
}

/// Forget a device's key.
pub fn remove(id: &str) -> Result<()> {
    let id = id.to_ascii_lowercase();
    let all = lines();
    if all.iter().any(|(i, _)| *i == id) {
        save(&all.into_iter().filter(|(i, _)| *i != id).collect::<Vec<_>>())?;
    }
    Ok(())
}
