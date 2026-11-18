//! A project folder on the computer: its files, secrets file and app name.

use anyhow::{anyhow, bail, Context, Result};

/// Is this a name the device accepts for an app (or a file in one)?
/// 1..23 characters from `[A-Za-z0-9_.-]`, not starting with `.`.
pub fn valid_app_name(name: &str) -> bool {
    !name.is_empty()
        && name.len() <= 23
        && !name.starts_with('.')
        && name.bytes().all(|c| c.is_ascii_alphanumeric() || b"_.-".contains(&c))
}

/// Name of the optional metadata file in a project (title, description, ...).
pub const APP_META_FILE: &str = "app.json";

/// Name of the optional per-project secrets file: `NAME=value` lines that
/// install and upload put into the device's secret store (`secrets.get(NAME)`),
/// so values like API keys stay out of the scripts. It is never uploaded as a
/// file; keep it out of git.
pub const SECRETS_FILE: &str = "secrets.env";

/// Whether `name` is a valid secret name: 1..15 characters `A-Z a-z 0-9 _ . -`.
pub fn valid_secret_name(name: &str) -> bool {
    (1..=15).contains(&name.len())
        && !name.starts_with('_')
        && name.bytes().all(|c| c.is_ascii_alphanumeric() || b"_.-".contains(&c))
}

/// Parse a secrets file: `NAME=value` per line; blank lines and `#` comments
/// are skipped, whitespace around names and values is trimmed, and a value may
/// be wrapped in matching quotes.
pub fn parse_secrets(text: &str) -> Result<Vec<(String, String)>> {
    let mut out: Vec<(String, String)> = Vec::new();
    for (i, line) in text.lines().enumerate() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let (name, value) =
            line.split_once('=').ok_or_else(|| anyhow!("{SECRETS_FILE}:{}: expected NAME=value", i + 1))?;
        let name = name.trim();
        let mut value = value.trim();
        if value.len() >= 2
            && (value.starts_with('"') && value.ends_with('"') || value.starts_with('\'') && value.ends_with('\''))
        {
            value = &value[1..value.len() - 1];
        }
        if !valid_secret_name(name) {
            bail!("{SECRETS_FILE}:{}: '{name}' can't be a secret name: 1..15 characters, A-Z a-z 0-9 _ . -, not starting with _", i + 1);
        }
        if value.len() > 2048 {
            bail!("{SECRETS_FILE}:{}: the value of {name} is over 2 KB", i + 1);
        }
        out.retain(|(n, _)| n != name);
        out.push((name.to_string(), value.to_string()));
    }
    Ok(out)
}

/// The secrets of the project folder `dir` (empty if it has no secrets file).
pub fn project_secrets(dir: &std::path::Path) -> Result<Vec<(String, String)>> {
    let path = dir.join(SECRETS_FILE);
    match std::fs::read_to_string(&path) {
        Ok(text) => parse_secrets(&text).with_context(|| format!("in {}", dir.display())),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Vec::new()),
        Err(e) => Err(e).with_context(|| format!("reading {}", path.display())),
    }
}

/// What to tell the user before code is installed or uploaded into an app
/// that keeps secrets on the device (None: nothing to say).
pub fn kept_secrets_note(app: &str, kept: &[String]) -> Option<String> {
    if kept.is_empty() {
        return None;
    }
    Some(format!(
        "note: app '{app}' has secrets stored on the device (named: {}), and the code you put into it can read them. \
         Put code into an app only if you'd trust its author with them.",
        kept.join(", ")
    ))
}

/// A device file name that is safe to use as a local file name: the device's
/// own rule (A-Z a-z 0-9 _ . -, not starting with '.', 1..31 characters), so
/// no path separators or "..", whatever the other end sends.
pub fn is_plain_file_name(name: &str) -> bool {
    !name.is_empty()
        && name.len() < 32
        && !name.starts_with('.')
        && name.chars().all(|c| c.is_ascii_alphanumeric() || matches!(c, '_' | '.' | '-'))
}

/// Kinds of files an app can carry besides its Lua files and app.json, for
/// `asset()` on the device: images for `disp.jpeg` and `disp.png`, sounds for
/// `sound.play`.
pub const ASSET_EXTENSIONS: &[&str] = &["jpg", "jpeg", "png", "wav"];

/// Is this file name an asset (by its extension, in any case)?
pub fn is_asset(name: &str) -> bool {
    name.rsplit_once('.').is_some_and(|(_, x)| ASSET_EXTENSIONS.iter().any(|e| x.eq_ignore_ascii_case(e)))
}

/// Files of a project folder that go to the device: Lua files, app.json and assets.
pub fn is_project_file(p: &std::path::Path) -> bool {
    p.is_file()
        && (p.extension().is_some_and(|x| x == "lua")
            || p.file_name().is_some_and(|n| n == APP_META_FILE || is_asset(&n.to_string_lossy())))
}

/// The folder a file is in: "." for a bare file name, whose parent() is "".
pub fn folder_of(file: &std::path::Path) -> &std::path::Path {
    file.parent().filter(|d| !d.as_os_str().is_empty()).unwrap_or(std::path::Path::new("."))
}

/// The app name for a project folder: its name, with characters the device
/// doesn't accept replaced by `-` and cut to the maximum length.
pub fn app_name_for(dir: &std::path::Path) -> String {
    let base = dir
        .canonicalize()
        .ok()
        .and_then(|p| p.file_name().map(|n| n.to_string_lossy().into_owned()))
        .or_else(|| dir.file_name().map(|n| n.to_string_lossy().into_owned()))
        .unwrap_or_else(|| "app".into());
    let mut name: String =
        base.chars().map(|c| if c.is_ascii_alphanumeric() || "_.-".contains(c) { c } else { '-' }).collect();
    while name.starts_with('.') {
        name.remove(0);
    }
    name.truncate(23);
    if name.is_empty() {
        "app".into()
    } else {
        name
    }
}

/// CRC-32 (IEEE), identical to the firmware's `crc32_calc`.
pub fn crc32(data: &[u8]) -> u32 {
    let mut crc = !0u32;
    for &b in data {
        crc ^= b as u32;
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0xEDB8_8320 & (0u32.wrapping_sub(crc & 1)));
        }
    }
    !crc
}

/// `git` working in `dir`, whatever repository the caller's environment
/// points at: a git hook (such as pre-push in a submodule) exports GIT_DIR
/// and friends, which would send it to that repository instead.
pub fn git_in(dir: &std::path::Path) -> std::process::Command {
    let mut c = std::process::Command::new("git");
    c.current_dir(dir);
    for v in ["GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY", "GIT_COMMON_DIR", "GIT_PREFIX"] {
        c.env_remove(v);
    }
    c
}

/// The app folders at `path`: the folder itself if it has a main.lua (one
/// app), else its subfolders that have one (a folder of apps, such as the
/// marketplace), sorted. An error if `path` isn't a folder.
pub fn app_folders(path: &std::path::Path) -> Result<Vec<std::path::PathBuf>> {
    let is_app = |p: &std::path::Path| p.join("main.lua").is_file();
    if !path.is_dir() {
        bail!("{} isn't a folder", path.display());
    }
    if is_app(path) {
        return Ok(vec![path.to_path_buf()]);
    }
    let mut apps: Vec<std::path::PathBuf> = std::fs::read_dir(path)
        .with_context(|| format!("reading folder {}", path.display()))?
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| p.is_dir() && is_app(p))
        .collect();
    apps.sort();
    Ok(apps)
}

/// The version stamp of a project folder in a git work tree: `+<short
/// commit>`, and `-dirty` when git sees changes inside the folder (ignored
/// files such as secrets.env don't count). None outside git or without git.
/// `+` is semver build metadata: `1.0.0+abc1234` still compares as 1.0.0.
pub fn dev_version_suffix(dir: &std::path::Path) -> Option<String> {
    let git = |args: &[&str]| {
        let out = git_in(dir).args(args).output().ok()?;
        out.status.success().then(|| String::from_utf8_lossy(&out.stdout).trim().to_string())
    };
    let short = git(&["rev-parse", "--short=7", "HEAD"])?;
    let dirty = !git(&["status", "--porcelain", "--", "."])?.is_empty();
    Some(format!("+{short}{}", if dirty { "-dirty" } else { "" }))
}

/// `json` with `suffix` added to the string value of its top-level
/// "version" member, the rest byte for byte the same. None when there's no
/// such member, it already has a `+`, or the text isn't a JSON object.
pub fn stamp_app_json(json: &[u8], suffix: &str) -> Option<Vec<u8>> {
    let ws = |i: &mut usize| {
        while *i < json.len() && json[*i].is_ascii_whitespace() {
            *i += 1;
        }
    };
    // A string starting at json[i] == '"': the index of its closing quote.
    let string_end = |mut i: usize| -> Option<usize> {
        i += 1;
        while i < json.len() {
            match json[i] {
                b'\\' => i += 2,
                b'"' => return Some(i),
                _ => i += 1,
            }
        }
        None
    };
    // Any value at json[i]: the index just past it.
    let value_end = |mut i: usize| -> Option<usize> {
        match *json.get(i)? {
            b'"' => string_end(i).map(|e| e + 1),
            b'{' | b'[' => {
                let mut depth = 0;
                while i < json.len() {
                    match json[i] {
                        b'"' => i = string_end(i)?,
                        b'{' | b'[' => depth += 1,
                        b'}' | b']' => {
                            depth -= 1;
                            if depth == 0 {
                                return Some(i + 1);
                            }
                        }
                        _ => {}
                    }
                    i += 1;
                }
                None
            }
            _ => {
                while i < json.len() && !b",}] \t\r\n".contains(&json[i]) {
                    i += 1;
                }
                Some(i)
            }
        }
    };
    let mut i = 0;
    ws(&mut i);
    if json.get(i) != Some(&b'{') {
        return None;
    }
    i += 1;
    loop {
        ws(&mut i);
        if json.get(i) != Some(&b'"') {
            return None;
        }
        let key_end = string_end(i)?;
        let key = &json[i + 1..key_end];
        i = key_end + 1;
        ws(&mut i);
        if json.get(i) != Some(&b':') {
            return None;
        }
        i += 1;
        ws(&mut i);
        if key == b"version" {
            if json.get(i) != Some(&b'"') {
                return None;
            }
            let end = string_end(i)?;
            if json[i + 1..end].contains(&b'+') {
                return None;
            }
            let mut out = json[..end].to_vec();
            out.extend_from_slice(suffix.as_bytes());
            out.extend_from_slice(&json[end..]);
            return Some(out);
        }
        i = value_end(i)?;
        ws(&mut i);
        match json.get(i) {
            Some(b',') => i += 1,
            _ => return None,
        }
    }
}

/// Stamp the app.json among `files` (name, contents) of the project folder
/// `dir` with [`dev_version_suffix`]. The file on disk stays as it is.
pub fn stamp_dev_version(dir: &std::path::Path, files: &mut [(String, Vec<u8>)]) {
    let Some(suffix) = dev_version_suffix(dir) else { return };
    for (name, data) in files.iter_mut() {
        if name == APP_META_FILE {
            if let Some(stamped) = stamp_app_json(data, &suffix) {
                *data = stamped;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn folder_of_a_bare_file_name_is_the_current_one() {
        assert_eq!(folder_of(std::path::Path::new("main.lua")), std::path::Path::new("."));
        assert_eq!(folder_of(std::path::Path::new("clock/main.lua")), std::path::Path::new("clock"));
        let here = std::env::current_dir().unwrap();
        let name = here.file_name().unwrap().to_string_lossy();
        assert_eq!(app_name_for(folder_of(std::path::Path::new("main.lua"))), name);
    }

    #[test]
    fn device_file_names_are_checked() {
        assert!(is_asset("logo.jpg") && is_asset("Ding.WAV") && is_asset("a.jpeg") && is_asset("x.PNG"));
        assert!(!is_asset("main.lua") && !is_asset("jpg") && !is_asset("x.gif"));
        assert!(is_plain_file_name("main.lua"));
        assert!(is_plain_file_name("app.json"));
        assert!(!is_plain_file_name(".."));
        assert!(!is_plain_file_name(".bashrc"));
        assert!(!is_plain_file_name("..\\..\\x.lua"));
        assert!(!is_plain_file_name(""));
    }

    #[test]
    fn secrets_file_parses() {
        let s = parse_secrets("# comment\n\nspotify_client = abc123\nkey=\"a = b\"\nkey='x'\n").unwrap();
        assert_eq!(s, vec![("spotify_client".into(), "abc123".into()), ("key".into(), "x".into())]);
        assert!(parse_secrets("no equals sign").is_err());
        assert!(parse_secrets("much_too_long_a_name=1").is_err());
        assert!(parse_secrets("bad name=1").is_err());
    }

    #[test]
    fn stamp_appends_to_version() {
        let j = br#"{"title":"A","version":"1.0.0"}"#;
        assert_eq!(stamp_app_json(j, "+abc1234").unwrap(), br#"{"title":"A","version":"1.0.0+abc1234"}"#.to_vec());
        let spaced = b"{\n  \"version\" : \"2.1.0\",\n  \"lua_api\": 1\n}\n";
        let want = b"{\n  \"version\" : \"2.1.0+abc1234-dirty\",\n  \"lua_api\": 1\n}\n";
        assert_eq!(stamp_app_json(spaced, "+abc1234-dirty").unwrap(), want.to_vec());
    }

    #[test]
    fn stamp_only_touches_the_version_member() {
        let j = br#"{"description":"the \"version\": \"x\" word","version":"1.0.0","nested":{"version":"9"}}"#;
        let want =
            br#"{"description":"the \"version\": \"x\" word","version":"1.0.0+abc1234","nested":{"version":"9"}}"#;
        assert_eq!(stamp_app_json(j, "+abc1234").unwrap(), want.to_vec());
        let nested_first = br#"{"nested":{"version":"9"},"list":["version"],"version":"1.0.0"}"#;
        let want = br#"{"nested":{"version":"9"},"list":["version"],"version":"1.0.0+abc1234"}"#;
        assert_eq!(stamp_app_json(nested_first, "+abc1234").unwrap(), want.to_vec());
    }

    #[test]
    fn stamp_none_without_version_or_with_plus() {
        assert!(stamp_app_json(br#"{"title":"A"}"#, "+abc1234").is_none());
        assert!(stamp_app_json(br#"{"version":"1.0.0+x"}"#, "+abc1234").is_none());
        assert!(stamp_app_json(br#"{"version":1}"#, "+abc1234").is_none());
        assert!(stamp_app_json(br#"{"nested":{"version":"9"}}"#, "+abc1234").is_none());
        assert!(stamp_app_json(b"not json", "+abc1234").is_none());
    }

    /// A fresh empty folder under the system's temp folder (outside any repo).
    fn fresh_dir(tag: &str) -> std::path::PathBuf {
        let d = std::env::temp_dir().join(format!("luaboard-stamp-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    fn git(dir: &std::path::Path, args: &[&str]) -> String {
        let out = git_in(dir)
            .args(["-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false"])
            .args(args)
            .output()
            .unwrap();
        assert!(out.status.success(), "git {args:?}: {}", String::from_utf8_lossy(&out.stderr));
        String::from_utf8_lossy(&out.stdout).trim().to_string()
    }

    #[test]
    fn dev_suffix_clean_and_dirty() {
        let repo = fresh_dir("git");
        let app = repo.join("hello");
        std::fs::create_dir(&app).unwrap();
        std::fs::write(app.join("app.json"), r#"{"version":"1.0.0"}"#).unwrap();
        std::fs::write(repo.join(".gitignore"), "secrets.env\n").unwrap();
        git(&repo, &["init", "-q"]);
        git(&repo, &["add", "."]);
        git(&repo, &["commit", "-q", "-m", "x"]);
        let short = git(&repo, &["rev-parse", "--short=7", "HEAD"]);
        std::fs::write(app.join("secrets.env"), "k=v\n").unwrap();
        assert_eq!(dev_version_suffix(&app), Some(format!("+{short}")));
        std::fs::write(repo.join("elsewhere.lua"), "x").unwrap(); // outside the app: still clean
        assert_eq!(dev_version_suffix(&app), Some(format!("+{short}")));
        std::fs::write(app.join("x.lua"), "x").unwrap();
        assert_eq!(dev_version_suffix(&app), Some(format!("+{short}-dirty")));

        let mut files =
            vec![("app.json".to_string(), br#"{"version":"1.0.0"}"#.to_vec()), ("main.lua".into(), b"m".to_vec())];
        stamp_dev_version(&app, &mut files);
        assert_eq!(files[0].1, format!(r#"{{"version":"1.0.0+{short}-dirty"}}"#).into_bytes());
        assert_eq!(files[1].1, b"m".to_vec());
        assert_eq!(std::fs::read(app.join("app.json")).unwrap(), br#"{"version":"1.0.0"}"#.to_vec());
        let _ = std::fs::remove_dir_all(&repo);
    }

    #[test]
    fn dev_suffix_outside_git() {
        let d = fresh_dir("plain");
        assert_eq!(dev_version_suffix(&d), None);
        let mut files = vec![("app.json".to_string(), br#"{"version":"1.0.0"}"#.to_vec())];
        stamp_dev_version(&d, &mut files);
        assert_eq!(files[0].1, br#"{"version":"1.0.0"}"#.to_vec());
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn app_folders_one_or_many() {
        let d = fresh_dir("apps");
        for (app, main) in [("b", true), ("a", true), ("tools", false), (".github", false)] {
            std::fs::create_dir_all(d.join(app)).unwrap();
            if main {
                std::fs::write(d.join(app).join("main.lua"), "x").unwrap();
            }
        }
        std::fs::write(d.join("README.md"), "x").unwrap();
        let names = |v: Vec<std::path::PathBuf>| -> Vec<String> {
            v.iter().map(|p| p.file_name().unwrap().to_string_lossy().into_owned()).collect()
        };
        assert_eq!(names(app_folders(&d).unwrap()), ["a", "b"]); // a folder of apps, sorted
        assert_eq!(names(app_folders(&d.join("b")).unwrap()), ["b"]); // one app
        assert!(app_folders(&d.join("tools")).unwrap().is_empty()); // neither
        assert!(app_folders(&d.join("nothing")).is_err());
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn crc_matches_zlib() {
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
        assert_eq!(crc32(b""), 0);
    }
}
