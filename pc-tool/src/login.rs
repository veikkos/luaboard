//! Browser-assisted login for scripts (protocol: `BROWSER`, `EVT browser`).
//!
//! A script on the device (usually through `oauth.lua`) asks for a login: the
//! tool opens the URL in this computer's browser, catches the redirect to the
//! loopback address the script named, and hands the redirect's query string
//! back to the device. Generic: the tool knows nothing about the service, and
//! never sees passwords or tokens (the one-time code it passes on is useless
//! without the PKCE verifier that stays on the device).

use anyhow::{anyhow, bail, Context, Result};
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Request {
    pub id: u32,
    /// The app on the device that asks, as the firmware reports it (not the
    /// script's own words): show it, so a login can't pass for another app's.
    pub app: String,
    /// Where the browser is sent after the login, e.g. `http://127.0.0.1:8888/callback`.
    pub redirect: String,
    pub url: String,
    /// What the login is for, as the script named it ("Spotify").
    pub title: String,
}

impl Request {
    /// `<id> <app> <redirect> <url> <title...>`
    pub fn parse(s: &str) -> Option<Request> {
        let mut it = s.trim().splitn(5, ' ');
        let id = it.next()?.parse().ok()?;
        let app = it.next()?.to_string();
        let redirect = it.next()?.to_string();
        let url = it.next()?.to_string();
        let title = it.next().unwrap_or("login").trim().to_string();
        Some(Request { id, app, redirect, url, title })
    }

    /// Host name of the login page, to show the user where they're logging in.
    pub fn host(&self) -> &str {
        let rest = self.url.split_once("://").map(|x| x.1).unwrap_or(&self.url);
        let authority = rest.split(['/', '?', '#', '\\']).next().unwrap_or(rest);
        // "accounts.google.com@evil.example": the browser goes to evil.example.
        authority.rsplit_once('@').map(|x| x.1).unwrap_or(authority)
    }
}

/// `EVT browser <id> <app> <redirect> <url> <title>` -> the request.
pub fn parse_event(evt: &str) -> Option<Request> {
    Request::parse(evt.strip_prefix("browser ")?)
}

/// `EVT browser-done <id> ok|cancelled` -> the id.
pub fn done_event(evt: &str) -> Option<u32> {
    evt.strip_prefix("browser-done ")?.split_whitespace().next()?.parse().ok()
}

/// Host, port and path of a loopback redirect URL.
fn parse_redirect(r: &str) -> Result<(String, u16, String)> {
    let rest = r.strip_prefix("http://").ok_or_else(|| anyhow!("redirect must be http://: {r}"))?;
    let (hostport, path) = match rest.find('/') {
        Some(i) => (&rest[..i], rest[i..].to_string()),
        None => (rest, "/".to_string()),
    };
    let (host, port) = hostport.rsplit_once(':').ok_or_else(|| anyhow!("redirect needs a port: {r}"))?;
    // "127.0.0.1:80@evil.example" has a user-info part: the host is evil.example.
    if hostport.contains('@') {
        bail!("redirect must be on this computer (127.0.0.1 or localhost): {r}");
    }
    if host != "127.0.0.1" && host != "localhost" {
        bail!("redirect must be on this computer (127.0.0.1 or localhost): {r}");
    }
    let host = if host == "localhost" { "127.0.0.1" } else { host };
    Ok((host.to_string(), port.parse().context("redirect port")?, path))
}

/// Is this a web address that is safe to hand to the system's URL opener?
/// The URL comes from the device (or anything answering on its port), so
/// only http(s) is allowed: `open`/`xdg-open` would also launch `file:` paths
/// and other handlers. No whitespace, control characters or quotes.
pub fn safe_web_url(url: &str) -> bool {
    let lower = url.get(..8).unwrap_or(url).to_ascii_lowercase();
    (lower.starts_with("https://") || lower.starts_with("http://"))
        && url.len() < 4096
        && !url.chars().any(|c| c.is_whitespace() || c.is_control() || c == '"' || c == '\\')
}

/// Open a URL in the browser. `LUABOARD_BROWSER` replaces the browser with a
/// command (the URL is appended), e.g. for tests: `LUABOARD_BROWSER="curl -sL"`.
pub fn open_browser(url: &str) -> Result<()> {
    if !safe_web_url(url) {
        bail!("not opening {url:?}: only http:// and https:// addresses");
    }
    let (prog, mut args): (String, Vec<String>) = match std::env::var("LUABOARD_BROWSER") {
        Ok(cmd) if !cmd.trim().is_empty() => {
            let mut it = cmd.split_whitespace().map(String::from);
            (it.next().unwrap(), it.collect())
        }
        _ if cfg!(target_os = "macos") => ("open".into(), vec![]),
        // Not `cmd /C start`: cmd.exe would treat & | ^ in the URL's query
        // string as command separators.
        _ if cfg!(windows) => ("rundll32".into(), vec!["url.dll,FileProtocolHandler".into()]),
        _ => ("xdg-open".into(), vec![]),
    };
    args.push(url.to_string());
    std::process::Command::new(&prog)
        .args(&args)
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .spawn()
        .with_context(|| format!("opening the browser ({prog})"))?;
    Ok(())
}

const PAGE: &str = "<!doctype html><meta charset=utf-8><title>luaboard</title>\
<body style=\"font-family:system-ui;margin:3em;max-width:36em\"><h2>luaboard</h2><p>";

fn respond(mut s: &TcpStream, status: &str, text: &str) {
    let body = format!("{PAGE}{text}</p></body>");
    let _ = write!(
        s,
        "HTTP/1.1 {status}\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
        body.len()
    );
}

/// Read one HTTP request; returns the request target ("/callback?code=...").
fn read_target(mut s: &TcpStream) -> Option<String> {
    s.set_nonblocking(false).ok()?;
    s.set_read_timeout(Some(Duration::from_secs(5))).ok()?;
    let mut buf = Vec::new();
    let mut chunk = [0u8; 2048];
    while !buf.windows(4).any(|w| w == b"\r\n\r\n") && buf.len() < 16384 {
        let n = s.read(&mut chunk).ok()?;
        if n == 0 {
            break;
        }
        buf.extend_from_slice(&chunk[..n]);
    }
    let text = String::from_utf8_lossy(&buf);
    let mut it = text.lines().next()?.split_whitespace();
    if it.next()? != "GET" {
        return None;
    }
    it.next().map(String::from)
}

/// Run the login: listen on the redirect address, open the browser (unless
/// `open` is false: the caller shows the URL), wait for the redirect and
/// return its query string. Stops early when `cancel` is set.
pub fn catch_redirect(req: &Request, open: bool, cancel: &AtomicBool, timeout: Duration) -> Result<String> {
    let (host, port, path) = parse_redirect(&req.redirect)?;
    let listener = TcpListener::bind((host.as_str(), port)).with_context(|| {
        format!("can't listen on {host}:{port} for the login redirect (is another program using port {port}?)")
    })?;
    listener.set_nonblocking(true)?;
    if open {
        open_browser(&req.url)?;
    }
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        if cancel.load(Ordering::Relaxed) {
            bail!("login cancelled");
        }
        match listener.accept() {
            Ok((stream, _)) => {
                let Some(target) = read_target(&stream) else { continue };
                let (p, q) = target.split_once('?').unwrap_or((&target, ""));
                if p != path {
                    respond(&stream, "404 Not Found", "Not the login page.");
                    continue;
                }
                if q.split('&').any(|kv| kv.starts_with("error=")) {
                    respond(&stream, "200 OK", "The login was not completed. You can close this tab.");
                } else {
                    respond(&stream, "200 OK", "Logged in. The device takes it from here; you can close this tab.");
                }
                return Ok(q.to_string());
            }
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => std::thread::sleep(Duration::from_millis(10)),
            Err(e) => return Err(e.into()),
        }
    }
    bail!("no login within {} s", timeout.as_secs())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_request_and_events() {
        let r = parse_event(
            "browser 3 spotify http://127.0.0.1:8888/callback https://accounts.example.com/authorize?a=1 My Service",
        )
        .unwrap();
        assert_eq!(r.id, 3);
        assert_eq!(r.app, "spotify");
        assert_eq!(r.redirect, "http://127.0.0.1:8888/callback");
        assert_eq!(r.host(), "accounts.example.com");
        assert_eq!(r.title, "My Service");
        assert_eq!(done_event("browser-done 3 ok"), Some(3));
        assert_eq!(parse_event("wifi connected"), None);
    }

    #[test]
    fn host_is_where_the_browser_goes() {
        let host =
            |url: &str| parse_event(&format!("browser 1 a http://127.0.0.1:1/ {url} x")).unwrap().host().to_string();
        assert_eq!(host("https://accounts.google.com@evil.example/o/oauth2"), "evil.example");
        assert_eq!(host("https://a:b@evil.example:8443/x"), "evil.example:8443");
        assert_eq!(host("https://evil.example\\@accounts.google.com/"), "evil.example");
        assert_eq!(host("https://accounts.example.com/x?u=a@b"), "accounts.example.com");
    }

    #[test]
    fn only_web_urls_are_opened() {
        assert!(safe_web_url("https://accounts.example.com/authorize?a=1&b=2"));
        assert!(safe_web_url("HTTP://example.com/"));
        assert!(!safe_web_url("file:///Applications/Calculator.app"));
        assert!(!safe_web_url("x-apple.systempreferences:"));
        assert!(!safe_web_url("https://example.com/\"&calc"));
        assert!(!safe_web_url("https://example.com/ x"));
        assert!(open_browser("file:///etc/passwd").is_err());
    }

    #[test]
    fn redirect_must_be_loopback() {
        assert!(parse_redirect("http://192.168.1.2:8888/cb").is_err());
        assert_eq!(parse_redirect("http://localhost:9000/cb").unwrap(), ("127.0.0.1".into(), 9000, "/cb".into()));
    }

    #[test]
    fn catches_the_redirect() {
        let req = Request {
            id: 1,
            app: "a".into(),
            redirect: "http://127.0.0.1:18765/cb".into(),
            url: "x".into(),
            title: "t".into(),
        };
        let t =
            std::thread::spawn(move || catch_redirect(&req, false, &AtomicBool::new(false), Duration::from_secs(5)));
        std::thread::sleep(Duration::from_millis(200));
        let mut s = TcpStream::connect("127.0.0.1:18765").unwrap();
        write!(s, "GET /favicon.ico HTTP/1.1\r\n\r\n").unwrap();
        drop(s);
        let mut s = TcpStream::connect("127.0.0.1:18765").unwrap();
        write!(s, "GET /cb?code=abc&state=xyz HTTP/1.1\r\nHost: x\r\n\r\n").unwrap();
        let mut resp = String::new();
        s.read_to_string(&mut resp).unwrap();
        assert!(resp.contains("Logged in"));
        assert_eq!(t.join().unwrap().unwrap(), "code=abc&state=xyz");
    }
}
