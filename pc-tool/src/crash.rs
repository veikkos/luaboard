//! The device's record of its last crash (`CRASH`, docs/PROTOCOL.md), and
//! turning the board's addresses into functions and lines with the
//! firmware build's ELF and ESP-IDF's `addr2line`. The simulator's frames
//! are function names already.

use std::path::{Path, PathBuf};
use std::process::Command;

/// What the last crash left: its cause, the firmware that crashed, and the
/// calls it was in, the innermost first.
#[derive(Debug, Clone, PartialEq)]
pub struct Crash {
    pub version: String,
    pub cause: String,
    pub frames: Vec<String>,
}

/// Parse a `CRASH` reply (`OK ver=.. frames=N cause=..` after `FRAME` lines,
/// or `OK none`): None when there's no crash to tell.
pub fn parse(reply: &str, lines: &[String]) -> Option<Crash> {
    if reply.trim() == "none" {
        return None;
    }
    let version = crate::kv_field(reply, "ver").unwrap_or("?").to_string();
    let cause = reply.split_once("cause=").map_or("?", |(_, c)| c).trim().to_string();
    Some(Crash { version, cause, frames: lines.to_vec() })
}

/// Is this reset reason (`reset=` in INFO) a crash, which may have left a record?
pub fn is_crash_reset(reason: &str) -> bool {
    matches!(reason, "panic" | "task-wdt" | "int-wdt" | "wdt" | "cpu-lockup")
}

/// Board frames are addresses ("0x4200a1b4"); the simulator's are names.
fn is_address(frame: &str) -> bool {
    frame.strip_prefix("0x").is_some_and(|h| !h.is_empty() && h.chars().all(|c| c.is_ascii_hexdigit()))
}

/// ESP-IDF's addr2line for the ESP32-S3: on the PATH, or where Espressif's
/// installer puts its tools (~/.espressif/tools/xtensa-esp-elf/<version>/).
pub fn addr2line() -> Option<PathBuf> {
    let exe = format!("xtensa-esp32s3-elf-addr2line{}", std::env::consts::EXE_SUFFIX);
    if let Some(p) = std::env::var_os("PATH")
        .and_then(|path| std::env::split_paths(&path).map(|d| d.join(&exe)).find(|p| p.is_file()))
    {
        return Some(p);
    }
    let home = std::env::var_os("HOME").or_else(|| std::env::var_os("USERPROFILE"))?;
    let tools = Path::new(&home).join(".espressif/tools/xtensa-esp-elf");
    let mut found: Vec<PathBuf> = std::fs::read_dir(tools)
        .ok()?
        .filter_map(|e| e.ok())
        .map(|e| e.path().join("xtensa-esp-elf/bin").join(&exe))
        .filter(|p| p.is_file())
        .collect();
    found.sort();
    found.pop()
}

/// The firmware ELF a build in this folder made (build/idf-<board>/luaboard.elf), if any.
pub fn default_elf() -> Option<PathBuf> {
    let mut found: Vec<PathBuf> = std::fs::read_dir("build")
        .ok()?
        .filter_map(|e| e.ok())
        .filter(|e| e.file_name().to_string_lossy().starts_with("idf-"))
        .map(|e| e.path().join("luaboard.elf"))
        .filter(|p| p.is_file())
        .collect();
    found.sort_by_key(|p| std::fs::metadata(p).and_then(|m| m.modified()).ok());
    found.pop()
}

/// Was `elf` built as this firmware version? Its app description holds the
/// version (`git describe`) as a string; another build's addresses would
/// point at the wrong code.
pub fn elf_is_version(elf: &Path, version: &str) -> bool {
    let Ok(data) = std::fs::read(elf) else { return false };
    let mut want = version.as_bytes().to_vec();
    want.push(0);
    data.windows(want.len()).any(|w| w == want.as_slice())
}

/// The crash's frames, each with its function and line where `elf` knows the
/// address: "0x4200a1b4  draw_footer (menu.c:230)". Names (the simulator's)
/// and addresses addr2line can't place stay as they are.
pub fn symbolize(frames: &[String], elf: &Path, tool: &Path) -> Vec<String> {
    let addrs: Vec<&String> = frames.iter().filter(|f| is_address(f)).collect();
    if addrs.is_empty() {
        return frames.to_vec();
    }
    let out = Command::new(tool).arg("-pfC").arg("-e").arg(elf).args(&addrs).output();
    let lines: Vec<String> = match out {
        Ok(o) if o.status.success() => String::from_utf8_lossy(&o.stdout).lines().map(String::from).collect(),
        _ => return frames.to_vec(),
    };
    let mut place = lines.into_iter();
    frames
        .iter()
        .map(|f| {
            if !is_address(f) {
                return f.clone();
            }
            match place.next() {
                // "draw_footer at /path/app/src/menu.c:230"
                Some(l) if !l.starts_with("??") => {
                    let (func, at) = l.split_once(" at ").unwrap_or((l.as_str(), ""));
                    let file = at.rsplit(['/', '\\']).next().unwrap_or(at);
                    if file.is_empty() || file.starts_with("??") {
                        format!("{f}  {func}")
                    } else {
                        format!("{f}  {func} ({file})")
                    }
                }
                _ => f.clone(),
            }
        })
        .collect()
}

/// The crash as lines to show: its cause and firmware, then the calls,
/// symbolized when the ELF of that very build is at hand (`elf`, or the one
/// built here). A note says how to get names when it isn't.
pub fn describe(c: &Crash, elf: Option<&Path>) -> Vec<String> {
    let mut out = vec![format!("last crash: {} (firmware {})", c.cause, c.version)];
    let mut frames = c.frames.clone();
    let mut note = None;
    if frames.iter().any(|f| is_address(f)) {
        let elf = elf.map(Path::to_path_buf).or_else(default_elf);
        match (elf, addr2line()) {
            (Some(elf), Some(tool)) if elf_is_version(&elf, &c.version) => frames = symbolize(&frames, &elf, &tool),
            (Some(elf), Some(_)) => {
                note = Some(format!("{} is another build than {}: no function names", elf.display(), c.version))
            }
            (None, _) => note = Some("for function names: the firmware's ELF from that build (--elf FILE)".into()),
            (_, None) => note = Some("for function names: ESP-IDF's xtensa-esp32s3-elf-addr2line".into()),
        }
    }
    if frames.is_empty() {
        out.push("  (no stack trace)".into());
    }
    out.extend(frames.iter().enumerate().map(|(i, f)| format!("  {i:2}  {f}")));
    out.extend(note.map(|n| format!("  ({n})")));
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_a_crash_reply() {
        let lines = vec!["0x4200a1b4".to_string(), "0x42001000".to_string()];
        let c = parse("ver=v1.0.0 frames=2 cause=LoadProhibited, data 0x00000000", &lines).unwrap();
        assert_eq!(c.version, "v1.0.0");
        assert_eq!(c.cause, "LoadProhibited, data 0x00000000");
        assert_eq!(c.frames.len(), 2);
        assert!(parse("none", &[]).is_none());
    }

    #[test]
    fn tells_addresses_from_names() {
        assert!(is_address("0x4200a1b4"));
        assert!(!is_address("draw_footer+0xe0") && !is_address("0x"));
        assert!(is_crash_reset("panic") && is_crash_reset("task-wdt") && !is_crash_reset("brownout"));
    }

    #[test]
    fn names_stay_without_an_elf() {
        let c = Crash { version: "x".into(), cause: "SIGSEGV".into(), frames: vec!["draw_footer+0xe0".into()] };
        let d = describe(&c, None);
        assert_eq!(d[0], "last crash: SIGSEGV (firmware x)");
        assert!(d[1].ends_with("draw_footer+0xe0") && d.len() == 2);
    }
}
