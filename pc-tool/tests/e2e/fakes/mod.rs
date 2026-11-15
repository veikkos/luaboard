//! Fake servers the device talks to in the tests, in this process: an MQTT
//! broker, an OAuth provider with a small web API, the release downloads and
//! the app marketplace. Each takes a free port (port 0) and logs
//! every event as one line ("PUB dev1 dev1/temp 23", "TOKEN code", ...).

#![allow(dead_code)]

pub mod http;
pub mod marketplace;
pub mod mqtt;
pub mod oauth;
pub mod releases;
pub mod tls;

use std::sync::{Arc, Mutex};

/// A fake's event log, one line per event.
#[derive(Clone, Default)]
pub struct Log(Arc<Mutex<Vec<String>>>);

impl Log {
    pub fn line(&self, s: impl Into<String>) {
        self.0.lock().unwrap().push(s.into());
    }
    pub fn lines(&self) -> Vec<String> {
        self.0.lock().unwrap().clone()
    }
    /// All of it, a line each ending in a newline.
    pub fn text(&self) -> String {
        self.lines().iter().map(|l| format!("{l}\n")).collect()
    }
    /// The last `n` lines.
    pub fn tail(&self, n: usize) -> String {
        let l = self.lines();
        l[l.len().saturating_sub(n)..].join("\n")
    }
    /// Whether a line contains `s`.
    pub fn has(&self, s: &str) -> bool {
        self.0.lock().unwrap().iter().any(|l| l.contains(s))
    }
    /// Whether a line matches the regular expression.
    pub fn grep(&self, pattern: &str) -> bool {
        let re = regex::Regex::new(pattern).unwrap();
        self.0.lock().unwrap().iter().any(|l| re.is_match(l))
    }
    /// How many lines contain `s`.
    pub fn count(&self, s: &str) -> usize {
        self.0.lock().unwrap().iter().filter(|l| l.contains(s)).count()
    }
}

pub use luaboard::timezone::civil;

/// (year, month, day) to days since 1970-01-01.
pub fn days_from_civil(y: i64, m: u32, d: u32) -> i64 {
    let y = if m <= 2 { y - 1 } else { y };
    let era = y.div_euclid(400);
    let yoe = y.rem_euclid(400);
    let m = m as i64;
    let doy = (153 * (if m > 2 { m - 3 } else { m + 9 }) + 2) / 5 + d as i64 - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146097 + doe - 719468
}

/// UTC seconds as 2026-11-06T13:30:00 (no zone).
pub fn iso(t: i64) -> String {
    let (y, mo, d) = civil(t.div_euclid(86400));
    let s = t.rem_euclid(86400);
    format!("{y:04}-{mo:02}-{d:02}T{:02}:{:02}:{:02}", s / 3600, s / 60 % 60, s % 60)
}

/// UTC seconds as 2026-11-06.
pub fn date(t: i64) -> String {
    let (y, mo, d) = civil(t.div_euclid(86400));
    format!("{y:04}-{mo:02}-{d:02}")
}

/// UTC seconds as an HTTP date: Fri, 06 Nov 2026 13:30:00 GMT.
pub fn http_date(t: i64) -> String {
    const DAYS: [&str; 7] = ["Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"];
    const MONTHS: [&str; 12] = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
    let days = t.div_euclid(86400);
    let (y, mo, d) = civil(days);
    let s = t.rem_euclid(86400);
    format!(
        "{}, {d:02} {} {y} {:02}:{:02}:{:02} GMT",
        DAYS[days.rem_euclid(7) as usize],
        MONTHS[mo as usize - 1],
        s / 3600,
        s / 60 % 60,
        s % 60
    )
}

pub fn now_secs() -> i64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_secs() as i64
}

/// Base64 for URLs, without padding.
pub fn base64url(data: &[u8]) -> String {
    const A: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    let mut out = String::new();
    for c in data.chunks(3) {
        let n = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
        for i in 0..=c.len() {
            out.push(A[(n >> (18 - 6 * i) & 63) as usize] as char);
        }
    }
    out
}

/// A random token of `n` bytes, as Python's secrets.token_urlsafe(n).
pub fn token(n: usize) -> String {
    let mut b = vec![0u8; n];
    getrandom::fill(&mut b).unwrap();
    base64url(&b)
}

/// The PKCE challenge of a verifier (S256).
pub fn pkce_challenge(verifier: &str) -> String {
    use sha2::Digest;
    base64url(&sha2::Sha256::digest(verifier.as_bytes()))
}

/// %XX (and, with `plus`, + for space) decoded.
pub fn unquote(s: &str, plus: bool) -> String {
    let b = s.as_bytes();
    let mut out = Vec::new();
    let mut i = 0;
    while i < b.len() {
        match b[i] {
            b'+' if plus => out.push(b' '),
            b'%' if i + 2 < b.len() => {
                match u8::from_str_radix(std::str::from_utf8(&b[i + 1..i + 3]).unwrap_or("zz"), 16) {
                    Ok(v) => {
                        out.push(v);
                        i += 2;
                    }
                    Err(_) => out.push(b'%'),
                }
            }
            c => out.push(c),
        }
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// Form/query encoding of a value (as Python's quote_plus).
pub fn quote_plus(s: &str) -> String {
    let mut out = String::new();
    for &c in s.as_bytes() {
        match c {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'_' | b'.' | b'-' | b'~' => out.push(c as char),
            b' ' => out.push('+'),
            _ => out.push_str(&format!("%{c:02X}")),
        }
    }
    out
}

/// A query string or form body as name -> value (the last of each name
/// wins; empty values are left out, as Python's dict(parse_qsl(...))).
pub fn parse_qs(s: &str) -> std::collections::HashMap<String, String> {
    let mut m = std::collections::HashMap::new();
    for part in s.split('&') {
        let Some((k, v)) = part.split_once('=') else { continue };
        if v.is_empty() {
            continue;
        }
        m.insert(unquote(k, true), unquote(v, true));
    }
    m
}
