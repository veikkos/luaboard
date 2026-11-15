//! A small HTTP/1.1 server (plain or TLS, a thread per connection) for the
//! fakes, and a client for the tests' own requests.

use std::collections::HashMap;
use std::io::{self, BufRead, BufReader, Read, Write};
use std::net::{Shutdown, TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::time::Duration;

use rustls::{ServerConfig, ServerConnection, StreamOwned};

pub struct Request {
    pub method: String,
    /// Path and query, as sent.
    pub target: String,
    pub headers: Vec<(String, String)>,
    pub body: Vec<u8>,
}

impl Request {
    pub fn path(&self) -> &str {
        self.target.split('?').next().unwrap_or("")
    }
    pub fn query(&self) -> HashMap<String, String> {
        super::parse_qs(self.target.split_once('?').map(|(_, q)| q).unwrap_or(""))
    }
    pub fn form(&self) -> HashMap<String, String> {
        super::parse_qs(&String::from_utf8_lossy(&self.body))
    }
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers.iter().find(|(k, _)| k.eq_ignore_ascii_case(name)).map(|(_, v)| v.as_str())
    }
    /// The Bearer token of the Authorization header ("" without one).
    pub fn bearer(&self) -> &str {
        let a = self.header("Authorization").unwrap_or("");
        a.strip_prefix("Bearer ").unwrap_or(a)
    }
}

pub enum Body {
    Bytes(Vec<u8>),
    /// Transfer-Encoding: chunked, a chunk each.
    Chunked(Vec<Vec<u8>>),
    /// No length: the body ends when the connection closes.
    UntilClose(Vec<u8>),
    /// No length, and the connection closes without a TLS close_notify, as
    /// some servers do it.
    UntilCut(Vec<u8>),
    /// Content-Length says all of it, but only the first `.1` bytes come,
    /// then the connection closes (no TLS close_notify): a cut download.
    Cut(Vec<u8>, usize),
    /// Content-Length says all of it; the first `.1` bytes come, then the
    /// rest once the gate opens: a download that stalls and goes on.
    Paused(Vec<u8>, usize, Gate),
    /// Content-Length says all of it; the first `.1` bytes come, then a byte
    /// every 2 ms (for a minute at most), each counted in `.2`: a download
    /// that is never idle and never done.
    Drip(Vec<u8>, usize, Arc<AtomicUsize>),
}

/// Open (true) or shut; Body::Paused waits for it to open.
pub type Gate = Arc<(Mutex<bool>, Condvar)>;

pub struct Response {
    pub status: u16,
    pub headers: Vec<(String, String)>,
    pub body: Body,
}

impl Response {
    pub fn new(status: u16, ctype: &str, body: impl Into<Vec<u8>>) -> Response {
        Response { status, headers: vec![("Content-Type".into(), ctype.into())], body: Body::Bytes(body.into()) }
    }
    pub fn json(status: u16, body: impl Into<Vec<u8>>) -> Response {
        Response::new(status, "application/json", body)
    }
    pub fn header(mut self, k: &str, v: &str) -> Response {
        self.headers.push((k.into(), v.into()));
        self
    }
}

fn reason(status: u16) -> &'static str {
    match status {
        200 => "OK",
        204 => "No Content",
        302 => "Found",
        500 => "Internal Server Error",
        400 => "Bad Request",
        401 => "Unauthorized",
        403 => "Forbidden",
        404 => "Not Found",
        405 => "Method Not Allowed",
        _ => "Unknown",
    }
}

pub type Handler = dyn Fn(&Request) -> Response + Send + Sync;

#[derive(Default)]
pub struct Opts {
    /// Serve HTTPS with this.
    pub tls: Option<Arc<ServerConfig>>,
    /// Answer as HTTP/1.0: one request per connection.
    pub http10: bool,
    /// The Date header's time (UTC seconds), else the real time.
    pub date: Option<i64>,
}

enum Conn {
    Plain(TcpStream),
    Tls(Box<StreamOwned<ServerConnection, TcpStream>>),
}

impl Read for Conn {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        match self {
            Conn::Plain(s) => s.read(buf),
            Conn::Tls(s) => s.read(buf),
        }
    }
}

impl Write for Conn {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        match self {
            Conn::Plain(s) => s.write(buf),
            Conn::Tls(s) => s.write(buf),
        }
    }
    fn flush(&mut self) -> io::Result<()> {
        match self {
            Conn::Plain(s) => s.flush(),
            Conn::Tls(s) => s.flush(),
        }
    }
}

/// The open connections of a server, so that stopping it closes them.
#[derive(Clone, Default)]
pub struct Conns(Arc<Mutex<ConnList>>);

/// The last id given, and (id, connection) for each open one.
type ConnList = (usize, Vec<(usize, TcpStream)>);

/// A connection in Conns until dropped (when its thread is done with it).
pub struct Open(Conns, usize);

impl Conns {
    pub fn add(&self, s: &TcpStream) -> Open {
        let mut g = self.0.lock().unwrap();
        g.0 += 1;
        let id = g.0;
        if let Ok(c) = s.try_clone() {
            g.1.push((id, c));
        }
        Open(self.clone(), id)
    }
    pub fn close_all(&self) {
        for (_, c) in self.0.lock().unwrap().1.drain(..) {
            let _ = c.shutdown(Shutdown::Both);
        }
    }
}

impl Drop for Open {
    fn drop(&mut self) {
        self.0 .0.lock().unwrap().1.retain(|(id, _)| *id != self.1);
    }
}

/// A server on 127.0.0.1; it stops when dropped.
pub struct Server {
    pub port: u16,
    stopping: Arc<AtomicBool>,
    conns: Conns,
}

impl Server {
    /// Close the listener and every connection.
    pub fn stop(&self) {
        if self.stopping.swap(true, Ordering::SeqCst) {
            return;
        }
        let _ = TcpStream::connect(("127.0.0.1", self.port)); // wakes up accept
        self.conns.close_all();
    }
}

impl Drop for Server {
    fn drop(&mut self) {
        self.stop();
    }
}

/// Listen on 127.0.0.1:`port` (0: any free port); the accepting thread
/// runs until the server is stopped.
pub fn listen(port: u16) -> (TcpListener, u16, Arc<AtomicBool>, Conns) {
    // A given port (a server restarted on the port it had) may take a moment to free up.
    let deadline = std::time::Instant::now() + Duration::from_secs(3);
    let l = loop {
        match TcpListener::bind(("127.0.0.1", port)) {
            Ok(l) => break l,
            Err(_) if port != 0 && std::time::Instant::now() < deadline => {
                std::thread::sleep(Duration::from_millis(20))
            }
            Err(e) => panic!("listen on {port}: {e}"),
        }
    };
    let port = l.local_addr().unwrap().port();
    (l, port, Arc::new(AtomicBool::new(false)), Conns::default())
}

pub fn serve(opts: Opts, handler: Arc<Handler>) -> Server {
    let (l, port, stopping, conns) = listen(0);
    let (stop2, conns2) = (stopping.clone(), conns.clone());
    let opts = Arc::new(opts);
    std::thread::spawn(move || {
        for s in l.incoming() {
            if stop2.load(Ordering::SeqCst) {
                break;
            }
            let Ok(s) = s else { continue };
            // Small writes go out at once: otherwise Linux holds them back
            // until the last one is acknowledged, up to 40 ms later, and a
            // body sent a byte at a time comes in bursts.
            let _ = s.set_nodelay(true);
            let open = conns2.add(&s);
            let (opts, handler) = (opts.clone(), handler.clone());
            std::thread::spawn(move || {
                let _open = open;
                let conn = match &opts.tls {
                    Some(cfg) => match ServerConnection::new(cfg.clone()) {
                        Ok(c) => Conn::Tls(Box::new(StreamOwned::new(c, s))),
                        Err(_) => return,
                    },
                    None => Conn::Plain(s),
                };
                let _ = handle(conn, &opts, &*handler);
            });
        }
    });
    Server { port, stopping, conns }
}

fn handle(conn: Conn, opts: &Opts, handler: &Handler) -> io::Result<()> {
    let mut r = BufReader::new(conn);
    loop {
        let mut line = String::new();
        if r.read_line(&mut line)? == 0 {
            return Ok(());
        }
        let line = line.trim_end();
        if line.is_empty() {
            continue;
        }
        let mut parts = line.split_whitespace();
        let method = parts.next().unwrap_or("").to_string();
        let target = parts.next().unwrap_or("/").to_string();
        let version = parts.next().unwrap_or("HTTP/1.0").to_string();
        let mut headers = Vec::new();
        loop {
            let mut h = String::new();
            if r.read_line(&mut h)? == 0 {
                return Ok(());
            }
            let h = h.trim_end();
            if h.is_empty() {
                break;
            }
            if let Some((k, v)) = h.split_once(':') {
                headers.push((k.trim().to_string(), v.trim().to_string()));
            }
        }
        let mut req = Request { method, target, headers, body: Vec::new() };
        let n: usize = req.header("Content-Length").and_then(|v| v.parse().ok()).unwrap_or(0);
        let mut body = vec![0u8; n];
        r.read_exact(&mut body)?;
        req.body = body;
        let conn_hdr = req.header("Connection").unwrap_or("").to_ascii_lowercase();
        let mut close = opts.http10 || conn_hdr == "close" || (version == "HTTP/1.0" && conn_hdr != "keep-alive");

        let resp = handler(&req);
        let date = super::http_date(opts.date.unwrap_or_else(super::now_secs));
        let mut out = format!(
            "{} {} {}\r\nServer: luaboard-e2e\r\nDate: {date}\r\n",
            if opts.http10 { "HTTP/1.0" } else { "HTTP/1.1" },
            resp.status,
            reason(resp.status)
        );
        for (k, v) in &resp.headers {
            out.push_str(&format!("{k}: {v}\r\n"));
        }
        let w = r.get_mut();
        match resp.body {
            Body::Bytes(b) => {
                out.push_str(&format!("Content-Length: {}\r\n\r\n", b.len()));
                let mut all = out.into_bytes();
                all.extend_from_slice(&b);
                w.write_all(&all)?;
            }
            Body::Chunked(parts) => {
                out.push_str("\r\n");
                let mut all = out.into_bytes();
                for p in parts {
                    all.extend_from_slice(format!("{:x}\r\n", p.len()).as_bytes());
                    all.extend_from_slice(&p);
                    all.extend_from_slice(b"\r\n");
                }
                all.extend_from_slice(b"0\r\n\r\n");
                w.write_all(&all)?;
            }
            Body::UntilClose(b) => {
                out.push_str("\r\n");
                let mut all = out.into_bytes();
                all.extend_from_slice(&b);
                w.write_all(&all)?;
                close = true;
            }
            Body::UntilCut(b) => {
                out.push_str("\r\n");
                let mut all = out.into_bytes();
                all.extend_from_slice(&b);
                w.write_all(&all)?;
                w.flush()?;
                let sock = match w {
                    Conn::Plain(s) => &*s,
                    Conn::Tls(s) => s.get_ref(),
                };
                let _ = sock.shutdown(Shutdown::Both);
                return Ok(());
            }
            Body::Cut(b, at) => {
                out.push_str(&format!("Content-Length: {}\r\n\r\n", b.len()));
                let mut all = out.into_bytes();
                all.extend_from_slice(&b[..at.min(b.len())]);
                w.write_all(&all)?;
                w.flush()?;
                let sock = match w {
                    Conn::Plain(s) => &*s,
                    Conn::Tls(s) => s.get_ref(),
                };
                let _ = sock.shutdown(Shutdown::Both);
                return Ok(());
            }
            Body::Paused(b, at, gate) => {
                let at = at.min(b.len());
                out.push_str(&format!("Content-Length: {}\r\n\r\n", b.len()));
                let mut all = out.into_bytes();
                all.extend_from_slice(&b[..at]);
                w.write_all(&all)?;
                w.flush()?;
                let (open, cv) = &*gate;
                let mut g = open.lock().unwrap();
                while !*g {
                    g = cv.wait(g).unwrap();
                }
                drop(g);
                w.write_all(&b[at..])?;
            }
            Body::Drip(b, at, sent) => {
                let at = at.min(b.len());
                out.push_str(&format!("Content-Length: {}\r\n\r\n", b.len()));
                let mut all = out.into_bytes();
                all.extend_from_slice(&b[..at]);
                w.write_all(&all)?;
                w.flush()?;
                for byte in b[at..].iter().take(30_000) {
                    std::thread::sleep(std::time::Duration::from_millis(2));
                    w.write_all(&[*byte])?;
                    w.flush()?;
                    sent.fetch_add(1, Ordering::SeqCst);
                }
                return Ok(());
            }
        }
        w.flush()?;
        if close {
            if let Conn::Tls(s) = w {
                s.conn.send_close_notify();
                let _ = s.flush();
            }
            let sock = match w {
                Conn::Plain(s) => &*s,
                Conn::Tls(s) => s.get_ref(),
            };
            let _ = sock.shutdown(Shutdown::Write);
            return Ok(());
        }
    }
}

/// (status, headers, body)
pub type Reply = (u16, Vec<(String, String)>, Vec<u8>);

/// A plain HTTP request from the tests.
pub fn request(method: &str, url: &str, body: &[u8]) -> io::Result<Reply> {
    let rest = url.strip_prefix("http://").ok_or_else(|| io::Error::other(format!("not http: {url}")))?;
    let (host, path) = match rest.find('/') {
        Some(i) => (&rest[..i], &rest[i..]),
        None => (rest, "/"),
    };
    let mut s = TcpStream::connect(host)?;
    s.set_read_timeout(Some(Duration::from_secs(10)))?;
    let mut req =
        format!("{method} {path} HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\nContent-Length: {}\r\n", body.len());
    if !body.is_empty() {
        req.push_str("Content-Type: application/x-www-form-urlencoded\r\n");
    }
    req.push_str("\r\n");
    let mut all = req.into_bytes();
    all.extend_from_slice(body);
    s.write_all(&all)?;
    let mut resp = Vec::new();
    let _ = s.read_to_end(&mut resp);
    let end = resp.windows(4).position(|w| w == b"\r\n\r\n").ok_or_else(|| io::Error::other("no response"))?;
    let head = String::from_utf8_lossy(&resp[..end]).into_owned();
    let mut lines = head.split("\r\n");
    let status = lines.next().and_then(|l| l.split_whitespace().nth(1)).and_then(|s| s.parse().ok()).unwrap_or(0);
    let headers =
        lines.filter_map(|l| l.split_once(':')).map(|(k, v)| (k.trim().to_string(), v.trim().to_string())).collect();
    Ok((status, headers, resp[end + 4..].to_vec()))
}

/// POST with an empty body; the status (0 if it failed).
pub fn post(url: &str) -> u16 {
    request("POST", url, b"").map(|r| r.0).unwrap_or(0)
}

/// What a browser does with a login address: GET it and follow the redirects.
pub fn browse(url: &str) -> io::Result<()> {
    let mut url = url.to_string();
    for _ in 0..10 {
        let (status, headers, _) = request("GET", &url, b"")?;
        let loc = headers.iter().find(|(k, _)| k.eq_ignore_ascii_case("Location")).map(|(_, v)| v.clone());
        match loc {
            Some(l) if (300..400).contains(&status) => {
                url = if l.starts_with("http") {
                    l
                } else {
                    let base = &url["http://".len()..];
                    let host = base.split('/').next().unwrap_or("");
                    format!("http://{host}{l}")
                };
            }
            _ => return Ok(()),
        }
    }
    Ok(())
}
