//! A WebSocket server: it answers the upgrade and echoes every message, text
//! or binary. Some text messages make it do more first: "ping" a ping
//! (payload "p1"), "pieces" its answer in three frames with a ping between
//! them, "big N" a message of N bytes, "slow" the first frame of a message
//! whose last one it sends when "more" comes, "bye" a close. The path "/badkey"
//! gets a wrong answer to the key and "/plain" an ordinary 200. Logged:
//! "OPEN <path>", "HDR <name>: <value>", "MSG <text or n bytes>",
//! "UNMASKED" for a frame a client didn't mask, "PONG <payload>", "CLOSE".

use std::io::{Read, Write};
use std::net::TcpListener;
use std::sync::Arc;

use rustls::{ServerConfig, ServerConnection, StreamOwned};

use super::Log;

pub struct WsServer {
    pub port: u16,
    pub log: Log,
}

trait Stream: Read + Write {}
impl<T: Read + Write> Stream for T {}

fn sha1(data: &[u8]) -> [u8; 20] {
    let mut h: [u32; 5] = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0];
    let mut msg = data.to_vec();
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    msg.extend_from_slice(&((data.len() as u64) * 8).to_be_bytes());
    for block in msg.chunks(64) {
        let mut w = [0u32; 80];
        for i in 0..16 {
            w[i] = u32::from_be_bytes(block[4 * i..4 * i + 4].try_into().unwrap());
        }
        for i in 16..80 {
            w[i] = (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]).rotate_left(1);
        }
        let (mut a, mut b, mut c, mut d, mut e) = (h[0], h[1], h[2], h[3], h[4]);
        for (i, wi) in w.iter().enumerate() {
            let (f, k) = match i {
                0..=19 => ((b & c) | (!b & d), 0x5A827999u32),
                20..=39 => (b ^ c ^ d, 0x6ED9EBA1),
                40..=59 => ((b & c) | (b & d) | (c & d), 0x8F1BBCDC),
                _ => (b ^ c ^ d, 0xCA62C1D6),
            };
            let t = a.rotate_left(5).wrapping_add(f).wrapping_add(e).wrapping_add(k).wrapping_add(*wi);
            e = d;
            d = c;
            c = b.rotate_left(30);
            b = a;
            a = t;
        }
        for (x, y) in h.iter_mut().zip([a, b, c, d, e]) {
            *x = x.wrapping_add(y);
        }
    }
    let mut out = [0u8; 20];
    for (i, x) in h.iter().enumerate() {
        out[4 * i..4 * i + 4].copy_from_slice(&x.to_be_bytes());
    }
    out
}

fn base64(data: &[u8]) -> String {
    const A: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::new();
    for c in data.chunks(3) {
        let n = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
        for i in 0..4 {
            out.push(if i <= c.len() { A[(n >> (18 - 6 * i) & 63) as usize] as char } else { '=' });
        }
    }
    out
}

/// A frame from the server: not masked.
fn frame(fin: bool, op: u8, payload: &[u8]) -> Vec<u8> {
    let mut f = vec![if fin { 0x80 } else { 0 } | op];
    match payload.len() {
        n if n < 126 => f.push(n as u8),
        n if n < 65536 => {
            f.push(126);
            f.extend_from_slice(&(n as u16).to_be_bytes());
        }
        n => {
            f.push(127);
            f.extend_from_slice(&(n as u64).to_be_bytes());
        }
    }
    f.extend_from_slice(payload);
    f
}

fn serve(mut s: Box<dyn Stream>, log: &Log) -> std::io::Result<()> {
    let mut head = Vec::new();
    let mut b = [0u8; 1];
    while !head.ends_with(b"\r\n\r\n") {
        s.read_exact(&mut b)?;
        head.push(b[0]);
    }
    let head = String::from_utf8_lossy(&head).into_owned();
    let mut lines = head.split("\r\n");
    let path = lines.next().unwrap_or("").split(' ').nth(1).unwrap_or("").to_string();
    log.line(format!("OPEN {path}"));
    let mut key = String::new();
    for l in lines {
        let Some((name, value)) = l.split_once(':') else { continue };
        let (name, value) = (name.trim().to_lowercase(), value.trim());
        if name == "sec-websocket-key" {
            key = value.to_string();
        } else if name.starts_with("x-") {
            log.line(format!("HDR {name}: {value}"));
        }
    }
    if path == "/plain" {
        return s.write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi");
    }
    let accept = if path == "/badkey" {
        base64(&sha1(b"something else"))
    } else {
        base64(&sha1(format!("{key}258EAFA5-E914-47DA-95CA-C5AB0DC85B11").as_bytes()))
    };
    s.write_all(
        format!("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n\r\n")
            .as_bytes(),
    )?;
    loop {
        let mut h = [0u8; 2];
        s.read_exact(&mut h)?;
        let (op, masked) = (h[0] & 15, h[1] & 128 != 0);
        let mut n = (h[1] & 127) as usize;
        if n == 126 {
            let mut e = [0u8; 2];
            s.read_exact(&mut e)?;
            n = u16::from_be_bytes(e) as usize;
        } else if n == 127 {
            let mut e = [0u8; 8];
            s.read_exact(&mut e)?;
            n = u64::from_be_bytes(e) as usize;
        }
        let mut mask = [0u8; 4];
        if masked {
            s.read_exact(&mut mask)?;
        } else {
            log.line("UNMASKED");
        }
        let mut p = vec![0u8; n];
        s.read_exact(&mut p)?;
        for (i, x) in p.iter_mut().enumerate() {
            *x ^= mask[i % 4];
        }
        match op {
            8 => {
                log.line("CLOSE");
                return Ok(());
            }
            9 => s.write_all(&frame(true, 10, &p))?,
            10 => log.line(format!("PONG {}", String::from_utf8_lossy(&p))),
            1 | 2 => {
                let text = String::from_utf8_lossy(&p).into_owned();
                log.line(if n > 60 || op == 2 { format!("MSG {n} bytes") } else { format!("MSG {text}") });
                if op == 1 && text == "ping" {
                    s.write_all(&frame(true, 9, b"p1"))?;
                    s.write_all(&frame(true, 1, &p))?;
                } else if op == 1 && text == "slow" {
                    s.write_all(&frame(false, 1, b"first "))?;
                } else if op == 1 && text == "more" {
                    s.write_all(&frame(true, 0, b"last"))?;
                } else if op == 1 && text == "pieces" {
                    s.write_all(&frame(false, 1, b"one "))?;
                    s.write_all(&frame(true, 9, b"p2"))?;
                    s.write_all(&frame(false, 0, b"two "))?;
                    s.write_all(&frame(true, 0, b"three"))?;
                } else if let Some(count) =
                    text.strip_prefix("big ").and_then(|c| c.parse::<usize>().ok()).filter(|_| op == 1)
                {
                    s.write_all(&frame(true, 1, &vec![b'x'; count]))?;
                } else if op == 1 && text == "bye" {
                    s.write_all(&frame(true, 8, &[]))?;
                    return Ok(());
                } else {
                    s.write_all(&frame(true, op, &p))?;
                }
            }
            _ => {}
        }
    }
}

/// Start it on a free port, with TLS if a setup is given.
pub fn start(tls: Option<Arc<ServerConfig>>) -> WsServer {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    let log = Log::default();
    let l = log.clone();
    std::thread::spawn(move || {
        for s in listener.incoming().flatten() {
            let (l, tls) = (l.clone(), tls.clone());
            std::thread::spawn(move || {
                let stream: Box<dyn Stream> = match tls {
                    Some(cfg) => match ServerConnection::new(cfg) {
                        Ok(c) => Box::new(StreamOwned::new(c, s)),
                        Err(_) => return,
                    },
                    None => Box::new(s),
                };
                let _ = serve(stream, &l);
            });
        }
    });
    WsServer { port, log }
}
