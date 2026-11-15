//! The gcal app against a fake Google: login, alerts.

use std::fs;
use std::path::Path;

use super::gcal_app::GcalApp;
use crate::harness::{grep, Ppm, T};

pub fn run(t: &mut T) {
    t.say("== google calendar app against a fake Google: login, alerts");
    let g = GcalApp::start();
    g.install(t, 6000);
    let ok = t.until(3.0, |t| t.eval("UI.status").contains("log in"));
    t.check(ok, "gcal app: asks for a login", |t| format!("gcal app login status: {}", t.eval("UI.status")));
    t.browse();
    // UI.* is set as the app draws, just after a fetch: wait for the drawn state itself
    let ok = t.until(5.0, |t| t.eval("UI.title") == "Design review" && grep("^next - in", &t.eval("UI.main")));
    t.check(ok, "gcal app: next meeting in the main slot", |t| {
        format!("gcal app main: {} / {}", t.eval("UI.title"), t.eval("UI.main"))
    });
    g.google.revoke(); // access removed (or the weekly expiry)
    t.eval("refresh()");
    let ok = t.until(3.0, |t| t.eval("UI.status").contains("log in"));
    t.check(ok, "gcal app: expired login asks again", |t| format!("gcal app after revoke: {}", t.eval("UI.status")));
    let f = t.eval("UI.fetches");
    t.browse();
    let more = format!("UI.fetches > {f}");
    let ok = t.until(5.0, |t| t.eval(&more) == "true" && t.eval("UI.status").is_empty());
    t.check(ok, "gcal app: fetches again after the new login", |t| {
        format!("gcal app re-login: fetches {}, status {}", t.eval("UI.fetches"), t.eval("UI.status"))
    });

    // "Design review" starts within 10 min with a 15 min popup reminder: due right away.
    let ok = t.eval(r#"UI.alert and UI.alert:match("@15$") ~= nil"#) == "true";
    t.check(ok, "gcal app: the event's popup reminder raises an alert", |t| {
        format!("gcal alert: {}", t.eval("UI.alert"))
    });
    // the chime: three notes of 140 ms, recorded as 44.1 kHz stereo while the speaker is on
    let wav = t.path("speaker.wav");
    let ok = t.until_sim(3.0, |_| fs::metadata(&wav).map_or(0, |m| m.len()) > 44 + 3 * 6174 * 4);
    t.check(ok, "gcal app: the alert chimes", |_| {
        format!("speaker.wav: {} bytes", fs::metadata(&wav).map_or(0, |m| m.len()))
    });
    // the banner pulses (500 ms bright, 500 ms dark): a few shots catch a bright one
    let ok = t.until(2.0, |t| t.shot("screen.ppm").red_pixels() > 5000);
    t.check(ok, "gcal app: red alert banner on the screen", |t| {
        format!("gcal banner: {} red px", Ppm::load(Path::new(&t.path("screen.ppm"))).red_pixels())
    });
    t.press("B");
    let ok = t.until(2.0, |t| t.eval("UI.alert") == "nil" && grep("^gcal  *gcal_dismissed ", &t.e(&["secrets"])));
    t.check(ok, "gcal app: B dismisses the alert and remembers it", |t| {
        let s: String = t.e(&["secrets"]).lines().map(|l| format!("{l} ")).collect();
        format!("gcal dismiss: alert {}, secrets: {s}", t.eval("UI.alert"))
    });
    t.e(&["install", &t.path("gcal"), "--run"]); // restart the app: the dismissal must hold
    t.wait_lua(5.0, "UI.fetches >= 1");
    t.advance(100); // (and draws)
    let ok = t.eval("UI.fetches >= 1") == "true" && t.eval("UI.alert") == "nil";
    t.check(ok, "gcal app: a dismissed alert stays dismissed after a restart", |t| {
        format!("gcal after restart: fetches {}, alert {}", t.eval("UI.fetches"), t.eval("UI.alert"))
    });
    t.crash();
}
