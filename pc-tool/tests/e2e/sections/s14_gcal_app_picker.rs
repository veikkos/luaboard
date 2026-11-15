//! The gcal app against a fake Google: the calendar picker.

use super::gcal_app::GcalApp;
use crate::harness::{grep, T};

pub fn run(t: &mut T) {
    t.say("== google calendar app against a fake Google: calendar picker");
    let g = GcalApp::start();
    let log = g.log.clone();
    g.logged_in(t, 6000);
    t.press("B"); // no alert: B opens the calendar picker
    let ok = t.eval("UI.mode") == "picker" && t.eval("UI.cursor") == "1";
    t.check(ok, "gcal app: B opens the calendar picker", |t| {
        format!("gcal picker: mode {} cursor {}", t.eval("UI.mode"), t.eval("UI.cursor"))
    });
    t.press("C");
    let v = t.eval("UI.cursor");
    t.check(v == "2", "gcal picker: C moves down", |_| format!("gcal picker cursor: {v}"));
    t.press("B"); // tick the team calendar, down to Done
    t.press("C");
    let team_before = log.count("EVENTS team#x");
    if t.eval("UI.cursor") == "3" {
        t.press("B");
    }
    let ok = t.eval("UI.mode") == "calm" && grep("^gcal  *gcal_calendars ", &t.e(&["secrets"]));
    t.check(ok, "gcal picker: Done saves the choice", |t| {
        format!("gcal picker done: mode {}, cursor {}", t.eval("UI.mode"), t.eval("UI.cursor"))
    });
    let ok = t.until(3.0, |_| log.count("EVENTS team#x") > team_before);
    t.check(ok, "gcal picker: the ticked calendar is fetched", |_| "gcal picker: team calendar not fetched".into());
    t.eval(r#"real_set = secrets.set; secrets.set = function() error("secret too long") end"#); // a full store
    t.press("B"); // picker, up to Done, save
    t.press("A");
    t.press("B");
    t.eval("secrets.set = real_set");
    let ok = t.e(&["info"]).contains("running: gcal/main.lua") && t.eval("UI.status").contains("too many");
    t.check(ok, "gcal picker: a choice too big to save is reported, the app keeps running", |t| {
        let running: Vec<String> = t.e(&["info"]).lines().filter(|l| l.contains("running")).map(String::from).collect();
        format!("gcal picker save error: {}, status {}", running.join("\n"), t.eval("UI.status"))
    });
    t.crash();
}
