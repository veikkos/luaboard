//! What the marketplace sections (s44, s45) share: a fake marketplace
//! (HTTPS, trusted through SIM_CA_FILE), the simulator booted with it and a
//! shortened "day" between checks, apps installed from the work folder, and
//! waiting for the checks.

use crate::fakes::marketplace::FakeMarket;
use crate::fakes::tls;
use crate::harness::T;

/// The check interval in these tests, ms of the device's time.
pub const DAY: u64 = 600_000;

/// The fake marketplace, trusted by the simulators this section boots.
pub fn start(t: &mut T) -> FakeMarket {
    let cert = tls::self_signed();
    let market = FakeMarket::start(&cert);
    let ca = t.write("ca.pem", &cert.pem);
    t.sim_env = vec![("SIM_CA_FILE".into(), ca)];
    market
}

/// A fresh board that checks `market` once a DAY.
pub fn boot(t: &mut T, market: &FakeMarket) {
    t.wipe();
    let url = market.url();
    t.boot_with(&["--set", &format!("update_every={DAY}"), "--set", &format!("apps_url={url}")]);
}

/// Wi-Fi joins and the first check (a minute after) is over.
pub fn first_check(t: &mut T) -> bool {
    t.e(&["wifi", "set", "Home Net", "pw"]);
    let n = checks(t);
    t.wifi_up() && {
        t.advance(65_000);
        t.until(5.0, |t| checks(t) > n)
    }
}

pub fn app_json(title: &str, version: &str) -> String {
    format!(r#"{{ "title": "{title}", "version": "{version}", "lua_api": 1 }}"#)
}

/// Install an app from the work folder (not in git: no dev stamp).
pub fn install(t: &T, name: &str, json: &str, main: &str) {
    t.write(&format!("{name}/main.lua"), main);
    t.write(&format!("{name}/app.json"), json);
    t.e(&["install", &t.path(name)]);
}

/// The checks the device has finished since it booted (its log).
pub fn checks(t: &T) -> usize {
    let log = t.read("sim.err");
    log.matches("apps: check done").count() + log.matches("apps: check failed").count()
}

/// What the device said about apps lately (for failure messages).
pub fn said(t: &T) -> String {
    let log = t.read("sim.err");
    let lines: Vec<&str> = log.lines().filter(|l| l.contains(" apps: ") || l.contains(" menu: ")).collect();
    lines[lines.len().saturating_sub(3)..].join(" / ")
}

/// A day passes and the check it brings is over.
pub fn next_check(t: &mut T) -> bool {
    let n = checks(t);
    t.advance(DAY);
    t.until(5.0, |t| checks(t) > n)
}

/// An installed file's bytes.
pub fn flash(t: &T, path: &str) -> Vec<u8> {
    std::fs::read(t.path(&format!("nvs.bin.flash/{path}"))).unwrap_or_default()
}
