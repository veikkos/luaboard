//! The github example against a fake GitHub: the GitHub App's browser login,
//! the board, a chime when a run fails, and a fixed list of repos.

use std::fs;

use crate::fakes::github::FakeGithub;
use crate::harness::{E2E_TIME, T};

/// Dot pixels of a colour in the rows' status column, on a fresh screenshot.
fn dots(t: &T, want: (i32, i32, i32)) -> usize {
    let near = |v: u8, w: i32| (v as i32 - w).abs() < 12;
    t.shot("gh.ppm").count(6..24, 24..210, |r, g, b| near(r, want.0) && near(g, want.1) && near(b, want.2))
}

const PASS: (i32, i32, i32) = (63, 185, 80);
const FAIL: (i32, i32, i32) = (248, 81, 73);
const RUNNING: (i32, i32, i32) = (210, 153, 34);

/// The example, its URLs pointed at the fake, in the work folder.
fn install(t: &mut T, port: u16, secrets: &str) {
    let src = t.root.join("marketplace/github");
    for n in ["github.lua", "app.json"] {
        t.write(&format!("github/{n}"), fs::read(src.join(n)).unwrap());
    }
    let base = format!("http://127.0.0.1:{port}");
    let rp = crate::harness::free_port();
    let to = format!(
        r#"gh = github.new{{ api_url = "{base}/api", oauth_url = "{base}/login/oauth", redirect = "http://127.0.0.1:{rp}/callback", login_timeout = 6000,"#
    );
    let main: String = fs::read_to_string(src.join("main.lua"))
        .unwrap()
        .split_inclusive('\n')
        .map(|l| match l.strip_prefix("gh = github.new{") {
            Some(rest) => format!("{to}{rest}"),
            None => l.to_string(),
        })
        .collect();
    if !main.contains(&format!("{base}/api")) {
        t.bad("github: API URL not replaced");
    }
    t.write("github/main.lua", main);
    t.write("github/secrets.env", secrets);
    t.e(&["install", &t.path("github"), "--run"]);
}

pub fn run(t: &mut T) {
    t.say("== github board against a fake GitHub");
    let gh = FakeGithub::start(E2E_TIME as i64);
    let log = gh.log.clone();
    t.wipe();
    t.boot();
    t.e(&["wifi", "set", "Home Net", "pw"]); // the network gives the clock (NTP): run ages
    t.wait_lua(3.0, "sys.time() ~= nil");

    install(t, gh.port, "github_client = test-client\ngithub_secret = test-secret\n");
    let out = t.browse();
    let ok = out.contains("Login handed to the device") && t.until(3.0, |_| log.has("TOKEN code"));
    t.check(ok, "logs in through the browser (PKCE, client secret)", |_| format!("login: {out} / {}", log.tail(3)));
    let ok = t.wait_lua(5.0, r#"UI.polls >= 1 and UI.status == "ok""#);
    t.check(ok, "fetches the board", |t| format!("status: {} / {}", t.eval("UI.status"), log.tail(3)));
    let v = t.eval(
        r#"local n = {} for _, r in ipairs(UI.rows) do n[#n + 1] = r.name end return UI.user .. " " .. table.concat(n, ",")"#,
    );
    t.check(v == "octocat hello,widgets,docs", "the installed repos, last pushed first", |_| format!("rows: {v}"));
    let ok = log.has("RUNS hello branch=main");
    t.check(ok, "asks for runs on the default branch", |_| format!("runs: {}", log.tail(5)));
    let v = t.eval(
        "UI.reviews .. ' ' .. UI.rows[1].prs .. ' ' .. UI.rows[1].run.conclusion .. ' ' .. tostring(UI.rows[3].run)",
    );
    t.check(v == "2 3 success false", "review requests, open PRs, the run, and none for docs", |_| format!("got: {v}"));
    let (pass, running) = (dots(t, PASS), dots(t, RUNNING));
    t.check(pass > 40 && running > 40, &format!("a green and a yellow dot ({pass}, {running} px)"), |_| {
        format!("dots: green {pass}, yellow {running}")
    });

    t.eval("tones = 0; real_tone = real_tone or sound.tone; sound.tone = function(...) tones = tones + 1 return real_tone(...) end");
    gh.fail("hello");
    t.advance(5000);
    t.press("B");
    let ok = t.wait_lua(5.0, r#"UI.polls >= 2 and UI.status == "ok""#);
    let tones = t.eval("tones");
    let red = dots(t, FAIL);
    t.check(ok && tones != "0" && red > 40, "B refreshes: hello failed, red and a chime", |_| {
        format!("red {red} px, tones {tones}")
    });

    // Search's rate limit used up: the runs still come, the PR counts stay.
    gh.limit_search();
    let searches = log.count("SEARCH ");
    t.advance(5000);
    t.press("B");
    let ok = t.wait_lua(5.0, r#"UI.polls >= 3"#);
    let v = t.eval("UI.status .. ' ' .. UI.rows[1].prs .. ' ' .. UI.reviews");
    let more = log.count("SEARCH ") - searches;
    t.check(ok && v == "ok 3 2" && more == 1, "search rate limited: runs still update, one search tried", |_| {
        format!("got: {v}, {more} searches / {}", log.tail(4))
    });

    t.stop();
    t.boot(); // the login is kept: a refresh, no new browser login
    t.e(&["run", "github"]);
    let ok = t.wait_lua(5.0, r#"UI.polls >= 1 and UI.status == "ok""#);
    let ok = ok && log.count("AUTHORIZE") == 1 && log.has("TOKEN refresh");
    t.check(ok, "after a reboot: refreshes the login, no new one", |_| log.tail(4));

    // secrets.env lists the repos: those, in that order; one that isn't there is left out
    install(
        t,
        gh.port,
        "github_client = test-client\ngithub_secret = test-secret\nrepos = octocat/extra, octocat/nope\n",
    );
    let ok = t.wait_lua(5.0, r#"UI.polls >= 1 and UI.status == "ok""#);
    let v = t.eval(r#"#UI.rows .. " " .. UI.rows[1].full_name .. " " .. UI.rows[1].run.conclusion"#);
    t.check(ok && v == "1 octocat/extra success", "repos from secrets.env", |_| format!("rows: {v}"));
    t.crash();
}
