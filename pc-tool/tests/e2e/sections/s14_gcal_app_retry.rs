//! The gcal app against a fake Google: a login nobody does.

use super::gcal_app::GcalApp;
use crate::harness::T;

pub fn run(t: &mut T) {
    t.say("== google calendar app against a fake Google: a login nobody does");
    let g = GcalApp::start();
    g.logged_in(t, 3000);
    g.google.revoke(); // expired again, and nobody logs in (3 s timeout here)
    t.eval("refresh()");
    t.until(3.0, |t| t.eval("UI.status").contains("log in from")); // asking for a login...
    t.advance(3000); // ...that nobody does
    let ok = t.until(2.0, |t| t.eval("UI.status").contains("press o to log in"));
    t.check(ok, "gcal app: a login nobody did says how to retry", |t| {
        format!("gcal login timeout: {}", t.eval("UI.status"))
    });
    let logins = t.eval("UI.logins");
    t.advance(1000);
    let v = t.eval("UI.logins");
    t.check(v == logins, "gcal app: no new login right after one timed out", |_| {
        format!("gcal: logins {logins} -> {v}")
    });
    t.press("B");
    let next = (logins.parse::<i64>().unwrap_or(0) + 1).to_string();
    let ok = t.until(2.0, |t| t.eval("UI.logins") == next);
    t.check(ok, "gcal app: o starts the login again", |t| format!("gcal: o didn't log in: {}", t.eval("UI.logins")));
    let f = t.eval("UI.fetches");
    t.browse();
    if !t.wait_lua(5.0, &format!("UI.fetches > {f}")) {
        t.bad("gcal: no fetch after the retried login");
    }
    t.crash();
}
