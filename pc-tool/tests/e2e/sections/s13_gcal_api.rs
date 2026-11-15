//! Google Calendar: oauth.lua with a client_secret, the agenda module's own
//! checks, and the gcal API client against a fake Google.

use crate::fakes::google::FakeGoogle;
use crate::harness::{free_port, E2E_TIME, T};

const GO_LUA: &str = r#"local oauth = require("oauth")
R = {}
task(function()
  local l = oauth.new{ name = "gtest", client_id = "test-client", client_secret = $SECRET,
    authorize_url = "http://127.0.0.1:$GP/authorize", token_url = "http://127.0.0.1:$GP/token",
    redirect = "http://127.0.0.1:$GR/callback", params = { access_type = "offline", prompt = "consent" } }
  R.tok = l:token() ~= nil
  R.done = true
end)
"#;

const GC_LUA: &str = r#"local gcal, agenda = require("gcal"), require("agenda")
R = {}
task(function()
  local cal = gcal.new{ client_id = "test-client", client_secret = "test-secret",
    api_url = "http://127.0.0.1:$GP/calendar/v3", authorize_url = "http://127.0.0.1:$GP/authorize",
    token_url = "http://127.0.0.1:$GP/token", redirect = "http://127.0.0.1:$GR/callback" }
  local cals, err = cal:calendars()
  if not cals then R.err = err; R.done = true; return end
  R.n = #cals
  local lists, now = {}, sys.time()
  for _, c in ipairs(cals) do
    if c.primary then R.primary = c.id; R.defaults = table.concat(c.defaults, ",") else R.team = c.name end
    lists[#lists + 1] = assert(cal:events(c, agenda.day_start(now, 0), agenda.day_start(now, 2)))
  end
  local titles = {}
  for i, e in ipairs(agenda.merge(lists)) do titles[i] = e.title end
  R.titles = table.concat(titles, ",")
  R.done = true
end)
"#;

/// A login with oauth.lua against the fake (client_secret = the Lua
/// expression `secret`); R.tok = got a token.
fn oauth_try(t: &mut T, secret: &str, gp: u16, gr: u16) {
    let lua = GO_LUA.replace("$SECRET", secret).replace("$GP", &gp.to_string()).replace("$GR", &gr.to_string());
    t.write("go.lua", lua);
    t.e(&["put", &t.path("go.lua"), "--run", "go.lua"]);
    t.browse();
    t.wait_lua(5.0, "R.done");
}

pub fn run(t: &mut T) {
    t.say("== google calendar: oauth client_secret, agenda, API client (fake Google)");
    let google = FakeGoogle::start(E2E_TIME as i64, 0);
    let (gp, gr, glog) = (google.port, free_port(), google.log.clone());
    t.wipe();
    t.boot();
    t.e(&["wifi", "set", "Home Net", "pw"]); // the network gives the clock (NTP)
    t.wait_lua(3.0, "sys.time() ~= nil");
    oauth_try(t, r#""test-secret""#, gp, gr);
    let ok = glog.has("AUTHORIZE access_type=offline prompt=consent");
    t.check(ok, "google: authorize asks for offline access", |_| {
        let a: Vec<String> = glog.lines().into_iter().filter(|l| l.contains("AUTHORIZE")).collect();
        format!("google authorize: {}", a.join("\n"))
    });
    let ok = glog.grep("TOKEN code$") && t.eval("R.tok") == "true";
    t.check(ok, "oauth.lua sends client_secret (token exchange ok)", |t| {
        format!("client_secret: {}, R.tok={}", glog.tail(2), t.eval("R.tok"))
    });
    let wapp = t.wapp.clone();
    t.e(&["secrets", "rm", &wapp, "gtest"]);
    oauth_try(t, "nil", gp, gr);
    let ok = glog.has("TOKEN invalid_client");
    t.check(ok, "fake Google refuses a token request without the secret", |_| "no invalid_client".into());
    t.e(&["secrets", "rm", &wapp, "gtest"]);
    let agenda = t.root.join("examples/gcal/agenda.lua").to_string_lossy().into_owned();
    let test = t.root.join("pc-tool/tests/e2e/data/gcal_agenda_test.lua").to_string_lossy().into_owned();
    let agenda_out = t.e(&["put", &agenda, &test, "--run", "gcal_agenda_test.lua"]);
    let lines: Vec<&str> = agenda_out.lines().map(|l| l.trim_end_matches('\r')).collect();
    for l in &lines {
        if let Some(rest) = l.strip_prefix("PASS ") {
            t.ok(&format!("agenda: {rest}"));
        }
    }
    for l in &lines {
        if let Some(rest) = l.strip_prefix("FAIL ") {
            t.bad(&format!("agenda: {rest}"));
        }
    }
    if !lines.iter().any(|l| l.starts_with("DONE")) {
        let errs: Vec<&str> = lines
            .iter()
            .copied()
            .filter(|l| {
                let l = l.to_lowercase();
                l.contains("error") || l.contains("not found")
            })
            .take(2)
            .collect();
        t.bad(&format!("agenda checks didn't finish: {}", errs.join("\n")));
    }

    t.write("gc.lua", GC_LUA.replace("$GP", &gp.to_string()).replace("$GR", &gr.to_string()));
    let gcal = t.root.join("examples/gcal/gcal.lua").to_string_lossy().into_owned();
    t.e(&["put", &agenda, &gcal, &t.path("gc.lua"), "--run", "gc.lua"]);
    t.browse();
    t.wait_lua(5.0, "R.done");
    let ok = t.eval("R.n") == "2" && t.eval("R.primary") == "me@example.com" && t.eval("R.defaults") == "10";
    t.check(ok, "gcal: calendar list (primary, popup defaults only)", |t| {
        format!(
            "gcal calendars: n={} primary={} defaults={} err={}",
            t.eval("R.n"),
            t.eval("R.primary"),
            t.eval("R.defaults"),
            t.eval("R.err")
        )
    });
    let v = t.eval("R.team");
    t.check(v == "Tiimi Ääni", "gcal: calendar names as given (UTF-8)", |_| format!("gcal team name: {v}"));
    let v = t.eval("R.titles");
    t.check(
        v == "Design review,1:1 with Anna,Team sync",
        "gcal: events of both calendars, skipped ones left out",
        |_| format!("gcal titles: {v}"),
    );
    let ok = glog.has("EVENTS team#x@group.calendar.google.com singleEvents=true maxAttendees=1");
    t.check(ok, "gcal: calendar id URL-encoded, single events, own attendee entry only", |_| {
        let a: Vec<String> = glog.lines().into_iter().filter(|l| l.contains("EVENTS")).collect();
        format!("gcal events request: {}", a.join("\n"))
    });
    t.crash();
}
