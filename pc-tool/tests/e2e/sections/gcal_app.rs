//! Shared by the 14-gcal-app-* sections: the gcal app
//! installed with test credentials and pointed at a fake Google.

use std::fs;

use crate::fakes::google::FakeGoogle;
use crate::fakes::Log;
use crate::harness::{free_port, E2E_TIME, T};

pub struct GcalApp {
    pub google: FakeGoogle,
    pub gp: u16,
    pub gr: u16,
    pub log: Log,
}

impl GcalApp {
    /// The fake Google. Its meetings start 10..90 minutes after the tests'
    /// fixed time (15:30 on the device, Finland, UTC+2 in November): all of
    /// them today.
    pub fn start() -> GcalApp {
        let google = FakeGoogle::start(E2E_TIME as i64, 2);
        let (gp, log) = (google.port, google.log.clone());
        GcalApp { google, gp, gr: free_port(), log }
    }

    /// Boot, get the clock from the network, install and start the app. A
    /// login nobody completes ends after `login_timeout` ms (6000 normally; a
    /// browser login here takes well under a second).
    pub fn install(&self, t: &mut T, login_timeout: u64) {
        t.wipe();
        let wav = t.path("speaker.wav"); // what the speaker plays (the alert's chime)
        t.boot_with(&["--wav", &wav]);
        t.e(&["wifi", "set", "Home Net", "pw"]); // the network gives the clock (NTP)
        t.wait_lua(3.0, "sys.time() ~= nil");
        let src = t.root.join("marketplace/gcal");
        let mut names: Vec<String> = fs::read_dir(&src)
            .unwrap()
            .filter_map(|e| e.ok())
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .filter(|n| n.ends_with(".lua") || n == "app.json")
            .collect();
        names.sort();
        for n in &names {
            t.write(&format!("gcal/{n}"), fs::read(src.join(n)).unwrap());
        }
        let (gp, gr) = (self.gp, self.gr);
        let to = format!(
            r#"cal = gcal.new{{ api_url = "http://127.0.0.1:{gp}/calendar/v3", authorize_url = "http://127.0.0.1:{gp}/authorize", token_url = "http://127.0.0.1:{gp}/token", redirect = "http://127.0.0.1:{gr}/callback", login_timeout = {login_timeout},"#
        );
        let main: String = t
            .read("gcal/main.lua")
            .split_inclusive('\n')
            .map(|l| match l.strip_prefix("cal = gcal.new{") {
                Some(rest) => format!("{to}{rest}"),
                None => l.to_string(),
            })
            .collect();
        if !main.contains(&format!("127.0.0.1:{gp}/calendar")) {
            t.bad("gcal app: API URL not replaced");
        }
        t.write("gcal/main.lua", main);
        t.write("gcal/secrets.env", "gcal_client = test-client\ngcal_secret = test-secret\n");
        t.e(&["install", &t.path("gcal"), "--run"]);
    }

    /// Install, log in, and dismiss the alert that is due right away, so
    /// that B means "log in" or "calendars" again.
    pub fn logged_in(&self, t: &mut T, login_timeout: u64) {
        self.install(t, login_timeout);
        t.until(3.0, |t| t.eval("UI.status").contains("log in"));
        t.browse();
        if !t.wait_lua(5.0, "UI.fetches >= 1 and UI.alert ~= nil") {
            t.bad("gcal app: no fetch and alert after the login");
        }
        t.press("B");
        if !t.wait_lua(2.0, "UI.alert == nil") {
            t.bad("gcal app: alert not dismissed");
        }
    }
}
