//! A stand-in for Google's OAuth and Calendar API:
//!
//!   GET  /authorize            redirects straight back with a code (no login page)
//!   POST /token                authorization_code (checks PKCE) and refresh_token
//!                              grants; requires client_secret=test-secret like a
//!                              Google "Desktop app" client. Refreshes don't return
//!                              a new refresh token (Google keeps the old one).
//!   GET  /calendar/v3/users/me/calendarList
//!                              two calendars: me@example.com (primary, popup 10 by
//!                              default) and team#x@group.calendar.google.com
//!   GET  /calendar/v3/calendars/<id>/events
//!                              events at fixed times from `now` (like real ones,
//!                              they don't move between requests): on the primary
//!                              "Design review" at +10 min (popup 15, so it is due
//!                              at once), "1:1 with Anna" at +60 min (calendar
//!                              defaults), a declined one and an all-day one; on
//!                              team "Team sync" a day later
//!   POST /admin/revoke         invalidate every token (forces a new login)
//!
//! Every event is logged as one line ("AUTHORIZE ...", "TOKEN code", ...).

use std::collections::{HashMap, HashSet};
use std::sync::{Arc, Mutex};

use serde_json::{json, Value};

use super::http::{self, Request, Response, Server};
use super::Log;

const SECRET: &str = "test-secret";
const PRIMARY: &str = "me@example.com";
const TEAM: &str = "team#x@group.calendar.google.com";

#[derive(Default)]
struct State {
    codes: HashMap<String, (Option<String>, Option<String>)>,
    access: HashSet<String>,
    refresh: HashSet<String>,
}

pub struct FakeGoogle {
    pub port: u16,
    pub log: Log,
    state: Arc<Mutex<State>>,
    server: Server,
}

impl FakeGoogle {
    /// `now`: the fixture's current time (UTC seconds; the tests' E2E_TIME);
    /// `utc_offset`: the device's offset from UTC in hours (all-day events
    /// are on its local date).
    pub fn start(now: i64, utc_offset: i64) -> FakeGoogle {
        let log = Log::default();
        let state = Arc::new(Mutex::new(State::default()));
        let (log2, state2) = (log.clone(), state.clone());
        let server = http::serve(
            http::Opts::default(),
            Arc::new(move |r: &Request| handle(r, &log2, &mut state2.lock().unwrap(), now, utc_offset)),
        );
        FakeGoogle { port: server.port, log, state, server }
    }

    pub fn url(&self) -> String {
        format!("http://127.0.0.1:{}", self.port)
    }

    /// Invalidate every token (access removed, or the weekly expiry).
    pub fn revoke(&self) {
        let mut st = self.state.lock().unwrap();
        st.access.clear();
        st.refresh.clear();
        self.log.line("REVOKED");
    }

    pub fn stop(&self) {
        self.server.stop();
    }
}

fn z(t: i64) -> Value {
    json!({"dateTime": format!("{}Z", super::iso(t))})
}

fn calendars() -> Value {
    json!([
        {"id": PRIMARY, "summary": "Veikko", "primary": true,
         "defaultReminders": [{"method": "popup", "minutes": 10}, {"method": "email", "minutes": 60}]},
        {"id": TEAM, "summary": "Tiimi \u{c4}\u{e4}ni", "defaultReminders": []},
    ])
}

fn events(cal: &str, now: i64, utc_offset: i64) -> Option<Value> {
    let m = |n: i64| now + n * 60;
    let day = now + utc_offset * 3600;
    match cal {
        PRIMARY => Some(json!([
            {"id": "review", "summary": "Design review", "start": z(m(10)), "end": z(m(40)),
             "reminders": {"useDefault": false, "overrides": [{"method": "popup", "minutes": 15}]}},
            {"id": "anna", "summary": "1:1 with Anna", "start": z(m(60)), "end": z(m(90)),
             "reminders": {"useDefault": true}},
            {"id": "nope", "summary": "Declined thing", "start": z(m(90)), "end": z(m(120)),
             "attendees": [{"email": "me@example.com", "self": true, "responseStatus": "declined"}],
             "reminders": {"useDefault": true}},
            {"id": "holiday", "summary": "Holiday", "start": {"date": super::date(day)},
             "end": {"date": super::date(day + 86400)}},
        ])),
        TEAM => Some(json!([
            {"id": "sync", "summary": "Team sync", "start": z(m(24 * 60)), "end": z(m(24 * 60 + 30)),
             "reminders": {"useDefault": false, "overrides": [{"method": "popup", "minutes": 15}]}},
        ])),
        _ => None,
    }
}

fn invalid() -> Response {
    Response::json(401, json!({"error": {"code": 401, "message": "Invalid Credentials"}}).to_string())
}

fn handle(r: &Request, log: &Log, st: &mut State, now: i64, utc_offset: i64) -> Response {
    let path = r.path();
    let q = r.query();
    let g = |k: &str| q.get(k).cloned().unwrap_or_else(|| "None".into());
    match r.method.as_str() {
        "GET" if path == "/authorize" => {
            let code = super::token(24);
            st.codes.insert(code.clone(), (q.get("code_challenge").cloned(), q.get("redirect_uri").cloned()));
            log.line(format!("AUTHORIZE access_type={} prompt={}", g("access_type"), g("prompt")));
            let back = format!(
                "{}?code={}&state={}",
                q.get("redirect_uri").cloned().unwrap_or_default(),
                super::quote_plus(&code),
                super::quote_plus(q.get("state").map(String::as_str).unwrap_or(""))
            );
            Response::new(302, "text/plain", "").header("Location", &back)
        }
        "GET" if path == "/calendar/v3/users/me/calendarList" => {
            if !st.access.contains(r.bearer()) {
                return invalid();
            }
            log.line("CALENDARS");
            Response::json(200, json!({"items": calendars()}).to_string())
        }
        "GET" if path.starts_with("/calendar/v3/calendars/") && path.ends_with("/events") => {
            if !st.access.contains(r.bearer()) {
                return invalid();
            }
            let cal = super::unquote(&path["/calendar/v3/calendars/".len()..path.len() - "/events".len()], false);
            log.line(format!("EVENTS {cal} singleEvents={} maxAttendees={}", g("singleEvents"), g("maxAttendees")));
            match events(&cal, now, utc_offset) {
                Some(items) => Response::json(200, json!({"items": items}).to_string()),
                None => Response::json(404, json!({"error": {"code": 404, "message": "Not Found"}}).to_string()),
            }
        }
        "POST" if path == "/token" => {
            let f = r.form();
            if f.get("client_secret").map(String::as_str) != Some(SECRET) {
                log.line("TOKEN invalid_client");
                return Response::json(
                    401,
                    json!({"error": "invalid_client", "error_description": "Unauthorized"}).to_string(),
                );
            }
            match f.get("grant_type").map(String::as_str) {
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
                            json!({"error": "invalid_grant", "error_description": "Bad Request"}).to_string(),
                        );
                    }
                    let (a, rt) = (super::token(16), super::token(16));
                    st.access.insert(a.clone());
                    st.refresh.insert(rt.clone());
                    log.line("TOKEN code");
                    Response::json(
                        200,
                        json!({"access_token": a, "refresh_token": rt, "expires_in": 3599, "token_type": "Bearer"})
                            .to_string(),
                    )
                }
                Some("refresh_token") => {
                    if !f.get("refresh_token").is_some_and(|t| st.refresh.contains(t)) {
                        log.line("TOKEN refresh rejected");
                        return Response::json(
                            400,
                            json!({"error": "invalid_grant", "error_description": "Token has been expired or revoked."}).to_string(),
                        );
                    }
                    let a = super::token(16);
                    st.access.insert(a.clone());
                    log.line("TOKEN refresh");
                    Response::json(
                        200,
                        json!({"access_token": a, "expires_in": 3599, "token_type": "Bearer"}).to_string(),
                    )
                }
                _ => Response::json(400, json!({"error": "unsupported_grant_type"}).to_string()),
            }
        }
        "POST" if path == "/admin/revoke" => {
            st.access.clear();
            st.refresh.clear();
            log.line("REVOKED");
            Response::json(200, "{}")
        }
        _ => Response::json(404, "{}"),
    }
}
