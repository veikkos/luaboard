//! A stand-in for luaboard-marketplace as raw.githubusercontent.com serves
//! it, over HTTPS:
//!
//!   GET /main/index.json          200, the index (built from the versions added, or set_raw)
//!   GET /<commit>/<app>/<path>    200, that file of the version published at that commit
//!
//! Every request is logged as one line: "INDEX", "FILE <app>/<path>",
//! "FAIL <path>" (fail_once), "MISSING <path>" (404). cut sends part of a
//! file and closes the connection.

use std::collections::{BTreeMap, HashMap, HashSet};
use std::sync::{Arc, Mutex};

use serde_json::json;
use sha2::{Digest, Sha256};

use super::http::{self, Body, Request, Response, Server};
use super::tls::Cert;
use super::Log;

struct Version {
    version: String,
    lua_api: i32,
    commit: String,
    files: Vec<(String, Vec<u8>)>,
    other: Vec<String>,
}

#[derive(Default)]
struct State {
    apps: BTreeMap<String, (String, Vec<Version>)>,
    raw: Option<String>,
    fail_once: HashSet<String>,
    corrupt: HashSet<String>,
    cut: HashMap<String, usize>,
    commits: usize,
}

pub struct FakeMarket {
    pub port: u16,
    pub log: Log,
    state: Arc<Mutex<State>>,
    server: Server,
}

fn sha(data: &[u8]) -> String {
    Sha256::digest(data).iter().map(|b| format!("{b:02x}")).collect()
}

impl FakeMarket {
    pub fn start(cert: &Cert) -> FakeMarket {
        let log = Log::default();
        let state = Arc::new(Mutex::new(State::default()));
        let (log2, state2) = (log.clone(), state.clone());
        let server = http::serve(
            http::Opts { tls: Some(cert.config.clone()), ..Default::default() },
            Arc::new(move |r: &Request| handle(r, &log2, &mut state2.lock().unwrap())),
        );
        FakeMarket { port: server.port, log, state, server }
    }

    /// The base the device takes as --set apps_url.
    pub fn url(&self) -> String {
        format!("https://127.0.0.1:{}", self.port)
    }

    /// Publish a version of `app` (titled `title`) with these board files
    /// and other files' names; returns its commit.
    pub fn add(
        &self,
        app: &str,
        title: &str,
        version: &str,
        lua_api: i32,
        files: &[(&str, &[u8])],
        other: &[&str],
    ) -> String {
        let mut st = self.state.lock().unwrap();
        st.commits += 1;
        let commit = format!("{:040x}", st.commits);
        let v = Version {
            version: version.into(),
            lua_api,
            commit: commit.clone(),
            files: files.iter().map(|(n, d)| (n.to_string(), d.to_vec())).collect(),
            other: other.iter().map(|s| s.to_string()).collect(),
        };
        st.apps.entry(app.into()).or_insert_with(|| (title.into(), vec![])).1.push(v);
        commit
    }

    /// Serve this text as the index instead of the one built (None: built again).
    pub fn set_raw(&self, text: Option<String>) {
        self.state.lock().unwrap().raw = text;
    }

    /// The next request for <app>/<path> (any commit) gets a 500.
    pub fn fail_once(&self, app_path: &str) {
        self.state.lock().unwrap().fail_once.insert(app_path.into());
    }

    /// <app>/<path> says its full length but sends only `at` bytes, then the
    /// connection closes (once).
    pub fn cut(&self, app_path: &str, at: usize) {
        self.state.lock().unwrap().cut.insert(app_path.into(), at);
    }

    /// <app>/<path> is served with other bytes than the index says, from now on.
    pub fn corrupt(&self, app_path: &str) {
        self.state.lock().unwrap().corrupt.insert(app_path.into());
    }

    pub fn stop(&self) {
        self.server.stop();
    }
}

fn index(st: &State) -> String {
    let mut apps = serde_json::Map::new();
    for (name, (title, versions)) in &st.apps {
        let entry = |list: &[(String, Vec<u8>)]| -> Vec<serde_json::Value> {
            list.iter().map(|(p, d)| json!({"path": p, "size": d.len(), "sha256": sha(d)})).collect()
        };
        let vs: Vec<serde_json::Value> = versions
            .iter()
            .rev()
            .map(|v| {
                let other: Vec<(String, Vec<u8>)> = v.other.iter().map(|p| (p.clone(), b"x".to_vec())).collect();
                json!({"version": v.version, "lua_api": v.lua_api, "commit": v.commit,
                       "files": entry(&v.files), "other_files": entry(&other)})
            })
            .collect();
        // As the real index says for an app whose secrets.env.example has a key left empty.
        let setup = versions.last().is_some_and(|v| v.other.iter().any(|p| p == "secrets.env.example"));
        apps.insert(
            name.clone(),
            json!({"title": title, "description": format!("About {title}"), "author": "Tester",
                   "license": "MIT", "needs_setup": setup, "versions": vs}),
        );
    }
    serde_json::to_string_pretty(&json!({"format": 1, "apps": apps})).unwrap()
}

fn handle(r: &Request, log: &Log, st: &mut State) -> Response {
    let path = r.path().to_string();
    let parts: Vec<&str> = path.trim_start_matches('/').split('/').collect();
    match parts.as_slice() {
        ["main", "index.json"] => {
            log.line("INDEX");
            let text = st.raw.clone().unwrap_or_else(|| index(st));
            Response::new(200, "application/json", text)
        }
        [commit, app, file] => {
            let key = format!("{app}/{file}");
            if st.fail_once.remove(&key) {
                log.line(format!("FAIL {key}"));
                return Response::new(500, "text/plain", "server error");
            }
            let data = st.apps.get(*app).and_then(|(_, vs)| {
                let v = vs.iter().find(|v| v.commit == *commit)?;
                v.files.iter().find(|(n, _)| n == file).map(|(_, d)| d.clone())
            });
            let Some(mut data) = data else {
                log.line(format!("MISSING {key}"));
                return Response::new(404, "text/plain", "not found");
            };
            log.line(format!("FILE {key}"));
            if st.corrupt.contains(&key) {
                data.iter_mut().for_each(|b| *b ^= 0x20);
            }
            if let Some(at) = st.cut.remove(&key) {
                let headers = vec![("Content-Type".into(), "application/octet-stream".into())];
                return Response { status: 200, headers, body: Body::Cut(data, at) };
            }
            Response::new(200, "application/octet-stream", data)
        }
        _ => {
            log.line(format!("MISSING {path}"));
            Response::new(404, "text/plain", "not found")
        }
    }
}
