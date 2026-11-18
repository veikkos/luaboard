//! A fake OAuth 2.0 provider and API, for oauth.lua and http.lua:
//!
//!   GET  /authorize?...        redirects straight back to redirect_uri with a code
//!                              (a real provider shows a login page first)
//!   POST /token                authorization_code (checks PKCE) and refresh_token
//!                              grants; rotates refresh tokens as some providers do
//!   GET  /api/me               200 with a valid Bearer token, else 401
//!   GET  /chunked              a chunked response; /close: body until close
//!                              (/close?cut=1: with no TLS close_notify)
//!   GET  /big?n=N[&chunked=1]  N bytes of body
//!   GET  /cover/...            the JPEG it was started with
//!   POST /admin/revoke         invalidate every token (forces a new login)
//!   POST /admin/expire         invalidate access tokens only (forces a refresh)
//!
//! Every event is logged as one line ("AUTHORIZE ...", "TOKEN code", ...).

use std::collections::{HashMap, HashSet};
use std::sync::{Arc, Mutex};

use rustls::ServerConfig;
use serde_json::json;

use super::http::{self, Body, Request, Response, Server};
use super::Log;

#[derive(Default)]
struct State {
    /// code -> (challenge, redirect)
    codes: HashMap<String, (Option<String>, Option<String>)>,
    access: HashSet<String>,
    refresh: HashSet<String>,
}

pub struct FakeOauth {
    pub port: u16,
    pub log: Log,
    state: Arc<Mutex<State>>,
    server: Server,
}

impl FakeOauth {
    /// `cover`: the JPEG served as every cover; `tls`: serve HTTPS.
    pub fn start(cover: Vec<u8>, tls: Option<Arc<ServerConfig>>) -> FakeOauth {
        let log = Log::default();
        let state = Arc::new(Mutex::new(State::default()));
        let (log2, state2) = (log.clone(), state.clone());
        let server = http::serve(
            http::Opts { tls, ..Default::default() },
            Arc::new(move |r: &Request| handle(r, &log2, &mut state2.lock().unwrap(), &cover)),
        );
        FakeOauth { port: server.port, log, state, server }
    }

    /// http(s)://127.0.0.1:PORT
    pub fn url(&self) -> String {
        format!("http://127.0.0.1:{}", self.port)
    }

    /// Invalidate every token (the user removed the app's access).
    pub fn revoke(&self) {
        let mut st = self.state.lock().unwrap();
        st.access.clear();
        st.refresh.clear();
        self.log.line("REVOKED");
    }

    /// Invalidate the access tokens only (forces a refresh).
    pub fn expire(&self) {
        self.state.lock().unwrap().access.clear();
        self.log.line("EXPIRED");
    }

    pub fn stop(&self) {
        self.server.stop();
    }
}

fn expired() -> Response {
    Response::json(401, json!({"error": {"status": 401, "message": "The access token expired"}}).to_string())
}

fn tokens(st: &mut State) -> String {
    let (a, r) = (super::token(16), super::token(16));
    st.access.insert(a.clone());
    st.refresh.insert(r.clone());
    json!({"access_token": a, "refresh_token": r, "expires_in": 3600, "token_type": "Bearer"}).to_string()
}

/// `/redir?to=URL&code=N`: a redirect (302 unless `code` says otherwise) to `to`.
fn redirect(r: &Request) -> Response {
    let q = r.query();
    let code = q.get("code").and_then(|c| c.parse().ok()).unwrap_or(302);
    Response::new(code, "text/plain", "").header("Location", q.get("to").map(String::as_str).unwrap_or("/"))
}

/// `/echo`: what came, as "METHOD authorization-or-- body", and
/// " key=<X-Api-Key>" when that header came.
fn echo(r: &Request) -> Response {
    let auth = r.header("Authorization").unwrap_or("-");
    let key = r.header("X-Api-Key").map(|k| format!(" key={k}")).unwrap_or_default();
    Response::new(200, "text/plain", format!("{} {} {}{key}", r.method, auth, String::from_utf8_lossy(&r.body)))
}

fn handle(r: &Request, log: &Log, st: &mut State, cover: &[u8]) -> Response {
    let path = r.path();
    match path {
        "/redir" => return redirect(r),
        "/echo" => return echo(r),
        _ => {}
    }
    match r.method.as_str() {
        "GET" => match path {
            "/authorize" => {
                let q = r.query();
                let code = super::token(24);
                st.codes.insert(code.clone(), (q.get("code_challenge").cloned(), q.get("redirect_uri").cloned()));
                let g = |k: &str| q.get(k).cloned().unwrap_or_else(|| "None".into());
                log.line(format!("AUTHORIZE {} {} {}", g("client_id"), g("scope"), g("code_challenge_method")));
                let back = format!(
                    "{}?code={}&state={}",
                    q.get("redirect_uri").cloned().unwrap_or_default(),
                    super::quote_plus(&code),
                    super::quote_plus(q.get("state").map(String::as_str).unwrap_or(""))
                );
                Response::new(302, "text/plain", "").header("Location", &back)
            }
            "/api/me" => {
                if st.access.contains(r.bearer()) {
                    log.line("API ok");
                    Response::json(200, json!({"name": "tester", "list": [1, 2, null]}).to_string())
                } else {
                    log.line("API 401");
                    expired()
                }
            }
            p if p.starts_with("/cover/") => {
                log.line(format!("COVER {p}"));
                Response::new(200, "image/jpeg", cover.to_vec())
            }
            "/big" => {
                let q = r.query();
                let n: usize = q.get("n").and_then(|v| v.parse().ok()).unwrap_or(0);
                if q.contains_key("chunked") {
                    let parts = (0..n).step_by(4000).map(|i| vec![b'x'; 4000.min(n - i)]).collect();
                    Response {
                        status: 200,
                        headers: vec![("Transfer-Encoding".into(), "chunked".into())],
                        body: Body::Chunked(parts),
                    }
                } else {
                    Response::new(200, "text/plain", vec![b'x'; n])
                }
            }
            "/chunked" => Response {
                status: 200,
                headers: vec![
                    ("Content-Type".into(), "text/plain".into()),
                    ("Transfer-Encoding".into(), "chunked".into()),
                ],
                body: Body::Chunked(vec![b"hello ".to_vec(), b"chunked ".to_vec(), b"world".to_vec()]),
            },
            "/close" => {
                let body = b"until close ".repeat(100);
                Response {
                    status: 200,
                    headers: vec![("Connection".into(), "close".into())],
                    body: if r.query().contains_key("cut") { Body::UntilCut(body) } else { Body::UntilClose(body) },
                }
            }
            _ => Response::json(404, "{}"),
        },
        "POST" => {
            let f = r.form();
            match path {
                "/token" | "/api/token" => match f.get("grant_type").map(String::as_str) {
                    Some("authorization_code") => {
                        let (ch, redirect) = f.get("code").and_then(|c| st.codes.remove(c)).unwrap_or((None, None));
                        let calc = super::pkce_challenge(f.get("code_verifier").map(String::as_str).unwrap_or(""));
                        if ch.is_none()
                            || ch.as_deref() != Some(calc.as_str())
                            || redirect.as_ref() != f.get("redirect_uri")
                        {
                            log.line("TOKEN code rejected");
                            return Response::json(
                                400,
                                json!({"error": "invalid_grant", "error_description": "bad code or verifier"})
                                    .to_string(),
                            );
                        }
                        log.line("TOKEN code");
                        Response::json(200, tokens(st))
                    }
                    Some("refresh_token") => {
                        let rt = f.get("refresh_token").cloned().unwrap_or_default();
                        if !st.refresh.remove(&rt) {
                            log.line("TOKEN refresh rejected");
                            return Response::json(
                                400,
                                json!({"error": "invalid_grant", "error_description": "Refresh token revoked"})
                                    .to_string(),
                            );
                        }
                        log.line("TOKEN refresh");
                        Response::json(200, tokens(st))
                    }
                    _ => Response::json(400, json!({"error": "unsupported_grant_type"}).to_string()),
                },
                "/admin/revoke" => {
                    st.access.clear();
                    st.refresh.clear();
                    log.line("REVOKED");
                    Response::json(200, "{}")
                }
                "/admin/expire" => {
                    st.access.clear();
                    log.line("EXPIRED");
                    Response::json(200, "{}")
                }
                _ => Response::json(404, "{}"),
            }
        }
        _ => Response::json(404, "{}"),
    }
}
