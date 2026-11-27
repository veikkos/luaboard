//! Web from Lua: https with a custom CA, http bodies (chunked, until close,
//! big), json, crypto, jpeg and raw images; secrets from the CLI.

use crate::fakes::oauth::FakeOauth;
use crate::fakes::tls;
use crate::harness::{grep, T};
use std::io::{Read, Write};

const WEB_LUA: &str = r#"local http, json = require("http"), require("json")
R = {}
task(function()
  local st, body = http.get("https://127.0.0.1:$TP/chunked", nil, { ca = CA })
  R.tls = st == 200 and body
  local st2, err2 = http.get("https://127.0.0.1:$TP/chunked")
  R.untrusted = st2 == nil and err2:find("not trusted") and "rejected" or tostring(err2)
  R.chunked = select(2, http.get("http://127.0.0.1:$WP/chunked"))
  R.close = #select(2, http.get("http://127.0.0.1:$WP/close"))
  R.tlsclose = #select(2, http.get("https://127.0.0.1:$TP/close", nil, { ca = CA })) .. " " ..
    #select(2, http.get("https://127.0.0.1:$TP/close?cut=1", nil, { ca = CA }))
  R.bad = select(2, http.get("http://127.0.0.1:$RP/")) .. ", " .. select(2, http.get("http://127.0.0.1:$RP/"))
  local st3, err3 = http.get("http://127.0.0.1:$RP/", nil, { timeout = 300 })
  R.stall = tostring(st3) .. ", " .. tostring(err3)
  local function size(url) local ok, st, b = pcall(http.get, url) collectgarbage() return ok and st and #b or tostring(ok and b or st) end
  R.big = size("http://127.0.0.1:$WP/big?n=250000") .. " " .. size("http://127.0.0.1:$WP/big?n=250000&chunked=1")
  local t = json.decode('{"a":[1,2.5,null],"s":"\\u00e4"}')
  R.json = json.encode({ t.a[1], t.a[2], t.a[3] == json.null, t.s }) .. " " ..
    table.concat({ tostring(json.decode('{"a":')), select(2, json.decode('{"a":')) }, " ")
  R.sha = crypto.hex(crypto.sha256("abc")):sub(1, 16)
  R.b64 = crypto.unbase64(crypto.base64("luaboard?", true))
  -- cert_name: the certificate is for 127.0.0.1, so by another name it needs the name said.
  local L = "https://localhost:$TP"
  local function outcome(st, b) return st == nil and (b:find("not trusted") and "rejected" or b) or tostring(b) end
  R.named = outcome(http.get(L .. "/chunked", nil, { ca = CA, cert_name = "127.0.0.1" }))
  R.unnamed = outcome(http.get(L .. "/chunked", nil, { ca = CA }))      -- (not on the connection kept just now)
  local function redir(to) return "/redir?to=" .. http.urlencode(to) end
  R.same = outcome(http.get(L .. redir("/chunked"), nil, { ca = CA, cert_name = "127.0.0.1" }))
  -- Redirected to another server, that one is checked as itself, not as the name given for the first.
  R.elsewhere = outcome(http.get("https://127.0.0.1:$TP" .. redir(L .. "/chunked"), nil, { ca = CA, cert_name = "127.0.0.1" }))
  R.badname = select(2, pcall(http.get, L .. "/chunked", nil, { cert_name = true })) .. " | " ..
    select(2, pcall(net.connect, "127.0.0.1", $TP, { cert_name = "a\0b" }))
  local _, jpeg = http.get("http://127.0.0.1:$WP/cover/0-300.jpg")
  R.jpeg = table.concat({ disp.jpeg(0, 0, jpeg, 2) }, "x")
  R.done = true
end)
"#;

const REDIR_LUA: &str = r#"local http = require("http")
R = {}
local W = "http://127.0.0.1:$WP"
local function redir(to, code) return "/redir?to=" .. http.urlencode(to) .. (code and "&code=" .. code or "") end
local function get(url, h, o) local st, b = http.get(url, h, o) return tostring(st) .. " " .. tostring(b) end
task(function()
  R.follow = get(W .. redir(redir("/chunked")))
  R.abs = get(W .. redir(W .. "/chunked", 301))
  R.off = get(W .. redir("/chunked"), nil, { redirects = 0 })
  R.loop = get(W .. redir(redir(redir("/chunked"))), nil, { redirects = 1 })
  R.post303 = select(2, http.post(W .. redir("/echo", 303), "data"))
  R.post307 = select(2, http.post(W .. redir("/echo", 307), "data"))
  R.auth = select(2, http.get(W .. redir("/echo"), { Authorization = "Bearer x" })) .. "|" ..
    select(2, http.get(W .. redir("http://localhost:$WP/echo"), { Authorization = "Bearer x" }))
  local key = { ["X-Api-Key"] = "k1" }
  R.key = select(2, http.get(W .. redir("/echo"), key, { private_headers = { "x-api-key" } })) .. "|" ..
    select(2, http.get(W .. redir("http://localhost:$WP/echo"), key, { private_headers = { "X-API-KEY" } })) .. "|" ..
    select(2, http.get(W .. redir("http://localhost:$WP/echo"), key))
  R.badkey = select(2, pcall(http.get, W .. "/echo", key, { private_headers = "X-Api-Key" }))
  R.done = true
end)
function loop() end
"#;

/// Out of TLS sockets: http lets go of the connection it keeps, and says so
/// with nil, err when it keeps none.
const SOCKS_LUA: &str = r#"local http = require("http")
R = {}
local function both(a, b) return tostring(a) .. " " .. tostring(b) end
task(function()
  local held = {}
  for i = 1, 3 do held[i] = assert(net.connect("127.0.0.1", $TP, { ca = CA })) end
  for i = 1, 3 do assert(held[i]:wait_connected()) end
  R.kept = select(2, http.get("https://127.0.0.1:$TP/chunked", nil, { ca = CA }))
  R.other = select(2, http.get("https://127.0.0.1:$T2/chunked", nil, { ca = CA }))
  R.raw = both(net.connect("127.0.0.1", $TP, { ca = CA }))
  http.close_idle()
  held[4] = assert(net.connect("127.0.0.1", $TP, { ca = CA }))
  R.full = both(http.get("https://127.0.0.1:$T2/chunked", nil, { ca = CA }))
  R.done = true
end)
function loop() end
"#;

/// Kept connections the server closes: a GET is sent again, a POST isn't
/// (it may have arrived), and one closed while idle isn't used.
const KEEP_LUA: &str = r#"local http = require("http")
R = {}
local B = "http://127.0.0.1:$KP"
local function both(a, b) return tostring(a) .. " " .. tostring(b) end
task(function()
  http.get(B .. "/ok")
  R.get = both(http.get(B .. "/drop"))
  http.post(B .. "/ok", "x")
  R.post = both(http.post(B .. "/drop", "x"))
  http.post(B .. "/ok", "x")
  http.get("http://localhost:$KP/closeothers")
  R.idle = both(http.post(B .. "/drop", "x"))
  R.done = true
end)
function loop() end
"#;

/// An app started at boot, before Wi-Fi has joined, that goes online at once.
const BOOT_LUA: &str = r#"local http = require("http")
R = { early = net.wifi() }
task(function()
  R.got = select(2, http.get("http://127.0.0.1:$KP/ok"))
  R.wifi = net.wifi()
  R.done = true
end)
function loop() end
"#;

/// A keep-alive server: "/ok" answers "ok"; "/drop" on a connection that
/// already had a request reads it and closes without an answer (on a new
/// one it answers "ok"); "/closeothers" closes every other connection
/// first. Returns the port and the request lines it got.
fn keep_server() -> (u16, std::sync::Arc<std::sync::Mutex<Vec<String>>>) {
    use std::sync::{Arc, Mutex};
    let l = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let port = l.local_addr().unwrap().port();
    let log = Arc::new(Mutex::new(Vec::new()));
    let conns: Arc<Mutex<Vec<(usize, std::net::TcpStream)>>> = Arc::new(Mutex::new(Vec::new()));
    let log2 = log.clone();
    std::thread::spawn(move || {
        for (id, conn) in l.incoming().enumerate() {
            let Ok(mut c) = conn else { return };
            conns.lock().unwrap().push((id, c.try_clone().unwrap()));
            let (log, conns) = (log2.clone(), conns.clone());
            std::thread::spawn(move || {
                let mut buf = Vec::new();
                for n in 0.. {
                    let mut b = [0u8; 512];
                    let head = loop {
                        if let Some(i) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
                            break i + 4;
                        }
                        match c.read(&mut b) {
                            Ok(k) if k > 0 => buf.extend_from_slice(&b[..k]),
                            _ => return,
                        }
                    };
                    let text = String::from_utf8_lossy(&buf[..head]).to_string();
                    let len: usize = text
                        .lines()
                        .find_map(|l| {
                            l.to_ascii_lowercase().strip_prefix("content-length:").map(|v| v.trim().to_string())
                        })
                        .and_then(|v| v.parse().ok())
                        .unwrap_or(0);
                    while buf.len() < head + len {
                        match c.read(&mut b) {
                            Ok(k) if k > 0 => buf.extend_from_slice(&b[..k]),
                            _ => return,
                        }
                    }
                    buf.drain(..head + len);
                    let line = text.lines().next().unwrap_or("").rsplit_once(' ').map_or("", |x| x.0).to_string();
                    log.lock().unwrap().push(line.clone());
                    if line.ends_with(" /drop") && n > 0 {
                        let _ = c.shutdown(std::net::Shutdown::Both); // (conns holds a copy)
                        return;
                    }
                    if line.ends_with(" /closeothers") {
                        for (other, s) in conns.lock().unwrap().iter() {
                            if *other != id {
                                let _ = s.shutdown(std::net::Shutdown::Both);
                            }
                        }
                    }
                    if c.write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok").is_err() {
                        return;
                    }
                }
            });
        }
    });
    (port, log)
}

/// A server that answers each connection with the next of `replies`, raw,
/// and leaves it open.
fn raw_server(replies: &'static [&'static str]) -> u16 {
    let l = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let port = l.local_addr().unwrap().port();
    std::thread::spawn(move || {
        let mut open = Vec::new();
        for (reply, conn) in replies.iter().zip(l.incoming()) {
            let Ok(mut c) = conn else { return };
            let mut req = Vec::new();
            let mut b = [0u8; 512];
            while !req.windows(4).any(|w| w == b"\r\n\r\n") {
                match c.read(&mut b) {
                    Ok(n) if n > 0 => req.extend_from_slice(&b[..n]),
                    _ => break,
                }
            }
            let _ = c.write_all(reply.as_bytes());
            open.push(c);
        }
        loop {
            std::thread::park();
        }
    });
    port
}

pub fn run(t: &mut T) {
    t.say("== web: TLS, http, json, crypto, jpeg");
    let cert = tls::self_signed();
    t.wipe();
    t.boot();
    let cover = std::fs::read(t.root.join("pc-tool/tests/e2e/data/cover.jpg")).unwrap();
    let web = FakeOauth::start(cover, None);
    let webtls = FakeOauth::start(Vec::new(), Some(cert.config.clone()));
    let raw = raw_server(&[
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 1.5\r\n\r\nx",
        "HTTP/1.1 200 OK\r\n\r\npart of a body",
    ]);
    let lua = WEB_LUA
        .replace("$TP", &webtls.port.to_string())
        .replace("$WP", &web.port.to_string())
        .replace("$RP", &raw.to_string());
    t.write("web.lua", format!("CA = [[\n{}]]\n{lua}", cert.pem));
    t.e(&["put", &t.path("web.lua"), "--run", "web.lua"]);
    t.wait_lua(10.0, "R.done");
    let v = t.eval("R.tls");
    t.check(v == "hello chunked world", "https with a custom CA (opts.ca)", |_| format!("tls: {v}"));
    let v = t.eval("R.untrusted");
    t.check(v == "rejected", "unknown certificate rejected", |_| format!("untrusted: {v}"));
    let v = t.eval("R.named, R.unnamed, R.same, R.elsewhere");
    t.check(
        v == "hello chunked world\trejected\thello chunked world\trejected",
        "https with cert_name: for that server only, not on a connection kept for it, not for where it redirects",
        |_| format!("with cert_name / without / redirected to itself / redirected elsewhere: {v}"),
    );
    let v = t.eval("R.badname");
    let ok = v.matches("cert_name must be a name").count() == 2 && v.contains("web.lua:");
    t.check(ok, "a cert_name that isn't a name is a wrong call, at the app's line", |_| format!("bad names: {v}"));
    let v = t.eval("R.chunked");
    t.check(v == "hello chunked world", "http chunked body", |_| format!("chunked: {v}"));
    let v = t.eval("R.close");
    t.check(v == "1200", "http body until close", |_| format!("close: {v}"));
    let v = t.eval("R.tlsclose");
    t.check(v == "1200 1200", "https body until close, with and without close_notify", |_| format!("tls close: {v}"));
    let v = t.eval("R.stall");
    t.check(v == "nil, timeout", "a body until close that stalls is a timeout, not a short body", |_| {
        format!("stall: {v}")
    });
    let v = t.eval("R.big");
    t.check(v == "250000 250000", "http bodies close to the 256 KiB limit fit in the Lua heap", |_| {
        format!("big bodies: {v}")
    });
    let v = t.eval("R.bad");
    t.check(v == "bad chunk header, bad Content-Length", "a broken response is an error value, not a crash", |_| {
        format!("bad: {v}")
    });
    let v = t.eval("R.json");
    let ok = v == r#"[1,2.5,true,"ä"] nil json: unexpected end at line 1 (position 6)"#;
    t.check(ok, "json decode/encode, bad text is nil, err", |_| format!("json: {v}"));
    let ok = t.eval("R.sha") == "ba7816bf8f01cfea" && t.eval("R.b64") == "luaboard?";
    t.check(ok, "crypto sha256/base64", |_| "crypto".into());
    let v = t.eval("R.jpeg");
    t.check(v == "150x150", "disp.jpeg decodes at 1/2", |_| format!("jpeg: {v}"));
    let v = t.eval(r#"disp.jpeg(0, 0, "nope")"#);
    let ok = v.starts_with("nil\t") && v.contains("not a JPEG");
    t.check(ok, "disp.jpeg turns garbage down with an error value", |_| format!("jpeg garbage: {v}"));
    let v = t.eval(r#"disp.jpeg_size("nope")"#);
    t.check(v.starts_with("nil\t"), "... and so does disp.jpeg_size", |_| format!("jpeg_size garbage: {v}"));
    let ok = t.eval(r#"disp.image(0, 0, 2, 1, "\xF8\x00\xF8\x00") return disp.get(1, 0) >> 16 > 200"#) == "true";
    t.check(ok, "disp.image draws RGB565", |t| format!("disp.image: {}", t.eval("return disp.get(1, 0)")));
    let ok = t.eval(r#"disp.image(0, 0, 2, 2, "abc")"#).contains("needs 8 bytes");
    t.check(ok, "disp.image checks the data size", |_| "disp.image size check".into());
    let v = t
        .eval(r#"disp.text_width("Jyväskylä"), disp.text_width("…"), disp.text_width("\xC3x"), disp.text_width("😀")"#);
    t.check(v == "53\t17\t11\t5", "UTF-8 text: a character each, ... for an ellipsis, ? for the rest", |_| {
        format!("text widths: {v}")
    });
    // CELL: the 5x7 cell of a character as 35 X/_ (rows top to bottom)
    t.eval(
        r#"function CELL(s) disp.clear() disp.text(0, 0, s) local a = {}
        for y = 0, 6 do for x = 0, 4 do a[#a + 1] = disp.get(x, y) ~= 0 and "X" or "_" end end
        return table.concat(a) end"#,
    );
    let v = t.eval(r#"local a = CELL("a"):gsub("^_____", "_X_X_") return a == CELL("ä")"#);
    t.check(v == "true", "ä is drawn as a with two dots", |t| format!("ä: {}", t.eval(r#"CELL("ä")"#)));
    let v = t.eval(
        r#"CELL("Ä") ~= CELL("ä") and CELL("Ö") ~= CELL("ö") and CELL("Å") ~= CELL("å") and CELL("Ø") ~= CELL("0")"#,
    );
    t.check(v == "true", "capital Nordic letters differ from small ones (and Ø from 0)", |_| format!("ÄÖÅØ: {v}"));
    let v = t.eval(r#"CELL("€") ~= CELL("?") and CELL("°") ~= CELL("?"), disp.text_width("€°C")"#);
    t.check(v == "true\t17", "€ and ° have glyphs, one cell each", |_| format!("€°: {v}"));
    let v = t.eval(r#"CELL("č") == CELL("c")"#);
    t.check(v == "true", "a letter the font lacks is drawn without its accent", |_| "č is not drawn as c".into());

    t.say("== secrets");
    let wapp = t.wapp.clone();
    t.e(&["secrets", "set", &wapp, "apikey", "s3cret value"]);
    let v = t.eval(r#"secrets.get("apikey")"#);
    t.check(v == "s3cret value", "secret set from the CLI, read by a script", |_| format!("secret get: {v}"));
    let list = t.e(&["secrets"]);
    let ok = grep(&format!("^{}  *apikey .*12 B", regex::escape(&wapp)), &list) && !list.contains("s3cret");
    t.check(ok, "listing shows names, never values", |_| format!("secret listing: {list}"));
    t.e(&["secrets", "rm", &wapp, "apikey"]);
    let ok = t.eval(r#"secrets.get("apikey")"#) == "nil";
    t.check(ok, "secret deleted", |_| "secret rm".into());

    t.say("== http: redirects, out of sockets");
    t.write("redir.lua", REDIR_LUA.replace("$WP", &web.port.to_string()));
    t.e(&["put", &t.path("redir.lua"), "--run", "redir.lua"]);
    t.wait_lua(10.0, "R.done");
    let v = t.eval("R.follow, R.abs");
    t.check(
        v == "200 hello chunked world\t200 hello chunked world",
        "redirects are followed, relative and absolute",
        |_| format!("follow: {v}"),
    );
    let v = t.eval("R.off, R.loop");
    t.check(
        v == "302 \tnil too many redirects",
        "redirects = 0 returns the 3xx; more than allowed is an error",
        |_| format!("off/loop: {v}"),
    );
    let v = t.eval("R.post303, R.post307");
    t.check(v == "GET - \tPOST - data", "a 303 goes on as a GET, a 307 keeps the POST and its body", |_| {
        format!("post: {v}")
    });
    let v = t.eval("R.auth");
    t.check(v == "GET Bearer x |GET - ", "Authorization goes along on the same server only", |_| format!("auth: {v}"));
    let v = t.eval("R.key");
    t.check(v == "GET -  key=k1|GET - |GET -  key=k1", "... and so do the headers in private_headers", |_| {
        format!("key: {v}")
    });
    let v = t.eval("R.badkey");
    t.check(
        v.contains("redir.lua:") && v.contains("private_headers must be a list"),
        "private_headers is a list",
        |_| format!("bad private_headers: {v}"),
    );
    let webtls2 = FakeOauth::start(Vec::new(), Some(cert.config.clone()));
    let socks = SOCKS_LUA.replace("$TP", &webtls.port.to_string()).replace("$T2", &webtls2.port.to_string());
    t.write("socks.lua", format!("CA = [[\n{}]]\n{socks}", cert.pem));
    t.e(&["put", &t.path("socks.lua"), "--run", "socks.lua"]);
    t.wait_lua(10.0, "R.done");
    let v = t.eval("R.kept, R.other");
    t.check(
        v == "hello chunked world\thello chunked world",
        "out of TLS sockets, http lets go of the one it kept",
        |t| format!("socks: {v} {}", t.eval("R.raw")),
    );
    let v = t.eval("R.raw, R.full");
    let want = "nil too many TLS connections (max 4)";
    t.check(v == format!("{want}\t{want}"), "out of sockets is nil, err from net.connect and http", |_| {
        format!("full: {v}")
    });

    let (kport, klog) = keep_server();
    t.write("keep.lua", KEEP_LUA.replace("$KP", &kport.to_string()));
    t.e(&["put", &t.path("keep.lua"), "--run", "keep.lua"]);
    t.wait_lua(10.0, "R.done");
    let v = t.eval("R.get, R.post, R.idle");
    let log = klog.lock().unwrap().clone();
    let count = |l: &str| log.iter().filter(|x| *x == l).count();
    t.check(
        v == "200 ok\tnil closed\t200 ok" && count("GET /drop") == 2 && count("POST /drop") == 2,
        "a kept connection closed under a GET sends it again, a POST isn't sent twice, a closed one isn't used",
        |_| format!("results: {v}; server got: {log:?}"),
    );

    // An app that starts at boot runs before Wi-Fi has joined. What it opens
    // then waits for the link instead of failing (or going out some other way).
    t.write("boot/main.lua", BOOT_LUA.replace("$KP", &kport.to_string()));
    t.write("boot/app.json", r#"{ "title": "Boot", "version": "1.0", "lua_api": 1 }"#);
    t.e(&["install", &t.path("boot")]);
    t.e(&["autorun", "boot"]);
    t.stop();
    t.boot_with(&["--set", "wifi_net=Simulator"]);
    t.wait_lua(10.0, "R.done");
    let v = t.eval("R.early, R.got, R.wifi");
    t.check(v == "connecting\tok\tconnected", "a connection opened while Wi-Fi is joining waits for it", |_| {
        format!("at start, answer, Wi-Fi when it came: {v}")
    });
    t.crash();
}
