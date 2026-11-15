//! A stand-in for GitHub: a GitHub App's browser login and the REST API the
//! github example uses.
//!
//!   GET  /login/oauth/authorize        redirects straight back with a code
//!   POST /login/oauth/access_token     authorization_code (checks PKCE) and
//!                                      refresh_token grants; needs
//!                                      client_secret=test-secret. Like GitHub,
//!                                      refresh tokens rotate and a bad one is
//!                                      200 {"error": "bad_refresh_token"}
//!   GET  /api/user                     octocat
//!   GET  /api/user/installations       one installation (7)
//!   GET  /api/user/installations/7/repositories
//!                                      hello (pushed last), widgets, docs
//!   GET  /api/repos/octocat/<name>     those three and "extra"
//!   GET  /api/repos/octocat/<name>/actions/runs
//!                                      hello passed 5 min ago, widgets is running,
//!                                      extra passed, docs has none
//!   GET  /api/search/issues?q=...      review-requested:@me: 2, repo:octocat/hello: 3
//!   POST /admin/fail?repo=<name>       its latest run fails from now on
//!
//! The API answers 401 without a valid token. Every event is logged as one
//! line ("AUTHORIZE", "TOKEN code", "RUNS hello", "SEARCH ...").

use std::collections::{HashMap, HashSet};
use std::sync::{Arc, Mutex};

use serde_json::{json, Value};

use super::http::{self, Request, Response, Server};
use super::Log;

const SECRET: &str = "test-secret";

#[derive(Default)]
struct State {
    codes: HashMap<String, (Option<String>, Option<String>)>,
    access: HashSet<String>,
    refresh: HashSet<String>,
    failed: HashSet<String>,
    search_limited: bool,
}

pub struct FakeGithub {
    pub port: u16,
    pub log: Log,
    state: Arc<Mutex<State>>,
    server: Server,
}

impl FakeGithub {
    /// `now`: the tests' fixed time (UTC seconds), runs are dated from it.
    pub fn start(now: i64) -> FakeGithub {
        let log = Log::default();
        let state = Arc::new(Mutex::new(State::default()));
        let (log2, state2) = (log.clone(), state.clone());
        let server = http::serve(
            http::Opts::default(),
            Arc::new(move |r: &Request| handle(r, &log2, &mut state2.lock().unwrap(), now)),
        );
        FakeGithub { port: server.port, log, state, server }
    }

    /// The latest run of `repo` fails from now on.
    pub fn fail(&self, repo: &str) {
        self.state.lock().unwrap().failed.insert(repo.into());
        self.log.line(format!("FAIL {repo}"));
    }

    /// Search answers 403 from now on: its rate limit is used up.
    pub fn limit_search(&self) {
        self.state.lock().unwrap().search_limited = true;
        self.log.line("LIMIT search");
    }

    pub fn stop(&self) {
        self.server.stop();
    }
}

fn repo(name: &str, pushed: i64) -> Value {
    json!({"name": name, "full_name": format!("octocat/{name}"), "owner": {"login": "octocat"},
           "private": false, "default_branch": "main", "pushed_at": format!("{}Z", super::iso(pushed))})
}

fn repos(now: i64) -> Vec<Value> {
    vec![repo("widgets", now - 86400), repo("hello", now - 600), repo("docs", now - 5 * 86400)]
}

fn run(name: &str, status: &str, conclusion: Option<&str>, started: i64) -> Value {
    json!({"name": "CI", "head_branch": "main", "status": status, "conclusion": conclusion,
           "run_started_at": format!("{}Z", super::iso(started)), "created_at": format!("{}Z", super::iso(started)),
           "actor": {"login": "octocat"},
           "html_url": format!("https://github.com/octocat/{name}/actions/runs/1")})
}

fn unauthorized() -> Response {
    Response::json(401, json!({"message": "Bad credentials", "status": "401"}).to_string())
}

fn not_found() -> Response {
    Response::json(404, json!({"message": "Not Found", "status": "404"}).to_string())
}

fn handle(r: &Request, log: &Log, st: &mut State, now: i64) -> Response {
    let path = r.path().to_string();
    let q = r.query();
    if let Some(api) = path.strip_prefix("/api") {
        if !st.access.contains(r.bearer()) {
            log.line(format!("401 {api}"));
            return unauthorized();
        }
        let parts: Vec<&str> = api.trim_start_matches('/').split('/').collect();
        return match parts.as_slice() {
            ["user"] => Response::json(200, json!({"login": "octocat", "id": 1}).to_string()),
            ["user", "installations"] => Response::json(
                200,
                json!({"total_count": 1, "installations": [{"id": 7, "account": {"login": "octocat"}}]}).to_string(),
            ),
            ["user", "installations", "7", "repositories"] => {
                log.line("REPOS");
                Response::json(200, json!({"total_count": 3, "repositories": repos(now)}).to_string())
            }
            ["repos", "octocat", name] => match *name {
                "hello" | "widgets" | "docs" | "extra" => Response::json(200, repo(name, now - 3600).to_string()),
                _ => not_found(),
            },
            ["repos", "octocat", name, "actions", "runs"] => {
                log.line(format!("RUNS {name} branch={}", q.get("branch").map(String::as_str).unwrap_or("-")));
                let runs = match *name {
                    _ if st.failed.contains(*name) => vec![run(name, "completed", Some("failure"), now - 60)],
                    "hello" | "extra" => vec![run(name, "completed", Some("success"), now - 300)],
                    "widgets" => vec![run(name, "in_progress", None, now - 7200)],
                    "docs" => vec![],
                    _ => return not_found(),
                };
                Response::json(200, json!({"total_count": runs.len(), "workflow_runs": runs}).to_string())
            }
            ["search", "issues"] => {
                let query = q.get("q").cloned().unwrap_or_default();
                log.line(format!("SEARCH {query}"));
                if st.search_limited {
                    return Response::json(403, r#"{"message": "API rate limit exceeded"}"#)
                        .header("x-ratelimit-remaining", "0")
                        .header("x-ratelimit-resource", "search")
                        .header("x-ratelimit-reset", &(now + 60).to_string());
                }
                let n = if query.contains("review-requested:@me") {
                    2
                } else if query.contains("repo:octocat/hello ") {
                    3
                } else {
                    0
                };
                Response::json(200, json!({"total_count": n, "incomplete_results": false, "items": []}).to_string())
            }
            _ => not_found(),
        };
    }
    match (r.method.as_str(), path.as_str()) {
        ("GET", "/login/oauth/authorize") => {
            let code = super::token(24);
            st.codes.insert(code.clone(), (q.get("code_challenge").cloned(), q.get("redirect_uri").cloned()));
            log.line("AUTHORIZE");
            let back = format!(
                "{}?code={}&state={}",
                q.get("redirect_uri").cloned().unwrap_or_default(),
                super::quote_plus(&code),
                super::quote_plus(q.get("state").map(String::as_str).unwrap_or(""))
            );
            Response::new(302, "text/plain", "").header("Location", &back)
        }
        ("POST", "/login/oauth/access_token") => {
            let f = r.form();
            if f.get("client_secret").map(String::as_str) != Some(SECRET) {
                log.line("TOKEN bad secret");
                return Response::json(
                    200,
                    json!({"error": "incorrect_client_credentials", "error_description": "The client_id and/or client_secret passed are incorrect."}).to_string(),
                );
            }
            let ok = match f.get("grant_type").map(String::as_str) {
                Some("refresh_token") => {
                    let ok = f.get("refresh_token").is_some_and(|t| st.refresh.remove(t));
                    log.line(if ok { "TOKEN refresh" } else { "TOKEN refresh rejected" });
                    ok
                }
                _ => {
                    let (ch, redirect) = f.get("code").and_then(|c| st.codes.remove(c)).unwrap_or((None, None));
                    let calc = super::pkce_challenge(f.get("code_verifier").map(String::as_str).unwrap_or(""));
                    let ok = ch.as_deref() == Some(calc.as_str()) && redirect.as_ref() == f.get("redirect_uri");
                    log.line(if ok { "TOKEN code" } else { "TOKEN code rejected" });
                    ok
                }
            };
            if !ok {
                return Response::json(200, json!({"error": "bad_refresh_token", "error_description": "The refresh token passed is incorrect or expired."}).to_string());
            }
            let (a, rt) = (super::token(16), super::token(16));
            st.access.insert(a.clone());
            st.refresh.insert(rt.clone());
            Response::json(
                200,
                json!({"access_token": a, "expires_in": 28800, "refresh_token": rt,
                       "refresh_token_expires_in": 15897600, "token_type": "bearer", "scope": ""})
                .to_string(),
            )
        }
        ("POST", "/admin/fail") => {
            let name = q.get("repo").cloned().unwrap_or_default();
            st.failed.insert(name.clone());
            log.line(format!("FAIL {name}"));
            Response::json(200, "{}")
        }
        _ => Response::json(404, "{}"),
    }
}
