//! A stand-in for GitHub's release downloads, over HTTPS. Like GitHub, the
//! "latest" address redirects to the release's own, and that to where the
//! file is stored:
//!
//!   GET /latest/download/<MANIFEST>      302 to /download/<tag>/<MANIFEST>
//!                                        (tag: the manifest's version)
//!   GET /download/<tag>/<file>           302 to https://127.0.0.1:<port>/objects/<file>
//!   GET /objects/<MANIFEST>              200, the manifest (chunked)
//!   GET /objects/<file>                  200, a file set with set_file (Content-Length)
//!
//! MANIFEST and UPDATE_BIN are the names a release gives them.
//!
//! Every request is logged as one line: "LATEST", "DOWNLOAD <file>",
//! "OBJECT <file>", "FAIL <path>" (a 500 from fail_next), "LOOP <path>"
//! (redirect_loop), "MISSING <path>" (404).

use std::collections::HashMap;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};

use serde_json::json;
use sha2::{Digest, Sha256};

use super::http::{self, Body, Gate, Request, Response, Server};
use super::tls::Cert;
use super::Log;

pub const MANIFEST: &str = "manifest-m5stack-cores3.json";
pub const UPDATE_BIN: &str = "luaboard-m5stack-cores3-update.bin";

#[derive(Default)]
struct State {
    manifest: String,
    files: HashMap<String, Vec<u8>>,
    fail_next: usize,
    redirect_loop: bool,
    cut: HashMap<String, usize>,
    pause: HashMap<String, usize>,
    drip: HashMap<String, usize>,
    dripped: Arc<AtomicUsize>,
    gate: Gate,
}

pub struct FakeReleases {
    pub port: u16,
    pub log: Log,
    state: Arc<Mutex<State>>,
    server: Server,
}

impl FakeReleases {
    pub fn start(cert: &Cert) -> FakeReleases {
        let log = Log::default();
        let state = Arc::new(Mutex::new(State::default()));
        let (log2, state2) = (log.clone(), state.clone());
        // The port is known only once it listens: the handler reads it from here.
        let port = Arc::new(Mutex::new(0u16));
        let port2 = port.clone();
        let server = http::serve(
            http::Opts { tls: Some(cert.config.clone()), ..Default::default() },
            Arc::new(move |r: &Request| handle(r, &log2, &mut state2.lock().unwrap(), *port2.lock().unwrap())),
        );
        *port.lock().unwrap() = server.port;
        FakeReleases { port: server.port, log, state, server }
    }

    /// The manifest the latest release has.
    pub fn set_manifest(&self, json: &str) {
        self.state.lock().unwrap().manifest = json.to_string();
    }

    /// A file of the release (served as /objects/<name>).
    pub fn set_file(&self, name: &str, data: Vec<u8>) {
        self.state.lock().unwrap().files.insert(name.to_string(), data);
    }

    /// The next `n` requests get a 500.
    pub fn fail_next(&self, n: usize) {
        self.state.lock().unwrap().fail_next = n;
    }

    /// Every request redirects to itself, or not.
    pub fn redirect_loop(&self, on: bool) {
        self.state.lock().unwrap().redirect_loop = on;
    }

    /// /objects/<name> says its full length but sends only `at` bytes, then
    /// the connection closes.
    pub fn cut(&self, name: &str, at: usize) {
        self.state.lock().unwrap().cut.insert(name.to_string(), at);
    }

    /// /objects/<name> sends `at` bytes, then waits for resume (instead of
    /// a cut set before).
    pub fn pause(&self, name: &str, at: usize) {
        let mut st = self.state.lock().unwrap();
        st.cut.remove(name);
        st.drip.remove(name);
        st.pause.insert(name.to_string(), at);
        *st.gate.0.lock().unwrap() = false;
    }

    /// /objects/<name> sends `at` bytes, then a byte every 2 ms (instead of
    /// a cut or a pause set before).
    pub fn drip(&self, name: &str, at: usize) {
        let mut st = self.state.lock().unwrap();
        st.cut.remove(name);
        st.pause.remove(name);
        st.drip.insert(name.to_string(), at);
    }

    /// The bytes sent one at a time so far (drip).
    pub fn dripped(&self) -> usize {
        self.state.lock().unwrap().dripped.load(Ordering::SeqCst)
    }

    /// The paused downloads go on (and later ones aren't paused).
    pub fn resume(&self) {
        let mut st = self.state.lock().unwrap();
        st.pause.clear();
        let (open, cv) = &*st.gate;
        *open.lock().unwrap() = true;
        cv.notify_all();
    }

    pub fn stop(&self) {
        self.resume();
        self.server.stop();
    }
}

impl Drop for FakeReleases {
    fn drop(&mut self) {
        self.resume(); // no connection thread left waiting
    }
}

/// A valid format 1 manifest for the CoreS3 with this `image`.
pub fn manifest(ver: &str, url: &str, image: &[u8], lua_api: i32, lua_api_min: i32) -> String {
    let sha: String = Sha256::digest(image).iter().map(|b| format!("{b:02x}")).collect();
    json!({
        "format": 1,
        "version": ver,
        "board": "m5stack-cores3",
        "url": url,
        "size": image.len(),
        "sha256": sha,
        "lua_api": lua_api,
        "lua_api_min": lua_api_min,
        "notes": format!("Release {ver}"),
    })
    .to_string()
}

fn redirect(to: &str) -> Response {
    Response::new(302, "text/html", "").header("Location", to)
}

fn handle(r: &Request, log: &Log, st: &mut State, port: u16) -> Response {
    let path = r.path().to_string();
    if st.fail_next > 0 {
        st.fail_next -= 1;
        log.line(format!("FAIL {path}"));
        return Response::new(500, "text/plain", "server error");
    }
    if st.redirect_loop {
        log.line(format!("LOOP {path}"));
        return redirect(&path);
    }
    let parts: Vec<&str> = path.trim_start_matches('/').split('/').collect();
    match parts.as_slice() {
        ["latest", "download", MANIFEST] => {
            log.line("LATEST");
            let tag = serde_json::from_str::<serde_json::Value>(&st.manifest)
                .ok()
                .and_then(|v| v["version"].as_str().map(String::from))
                .unwrap_or_else(|| "v0".into());
            redirect(&format!("/download/{tag}/{MANIFEST}")) // relative, as the next is absolute
        }
        ["download", _tag, file] => {
            log.line(format!("DOWNLOAD {file}"));
            redirect(&format!("https://127.0.0.1:{port}/objects/{file}"))
        }
        ["objects", MANIFEST] => {
            log.line(format!("OBJECT {MANIFEST}"));
            // In pieces of 100 bytes: the device puts the chunks together.
            let parts = st.manifest.as_bytes().chunks(100).map(|c| c.to_vec()).collect();
            let headers = vec![
                ("Content-Type".into(), "application/json".into()),
                ("Transfer-Encoding".into(), "chunked".into()),
            ];
            Response { status: 200, headers, body: Body::Chunked(parts) }
        }
        ["objects", file] => {
            let Some(data) = st.files.get(*file) else {
                log.line(format!("MISSING {path}"));
                return Response::new(404, "text/plain", "not found");
            };
            log.line(format!("OBJECT {file}"));
            let body = match (st.pause.get(*file), st.cut.get(*file), st.drip.get(*file)) {
                (Some(&at), _, _) => Body::Paused(data.clone(), at, st.gate.clone()),
                (_, Some(&at), _) => Body::Cut(data.clone(), at),
                (_, _, Some(&at)) => Body::Drip(data.clone(), at, st.dripped.clone()),
                _ => Body::Bytes(data.clone()),
            };
            Response { status: 200, headers: vec![("Content-Type".into(), "application/octet-stream".into())], body }
        }
        _ => {
            log.line(format!("MISSING {path}"));
            Response::new(404, "text/plain", "not found")
        }
    }
}
