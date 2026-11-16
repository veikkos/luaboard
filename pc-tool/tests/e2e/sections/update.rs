//! What the update check sections (s32_update_*) share: a fake GitHub
//! release server (HTTPS, trusted through SIM_CA_FILE), the simulator booted
//! with a shortened "day" between checks (--set update_every), and waiting
//! for the checks.

use crate::fakes::releases::{FakeReleases, MANIFEST, UPDATE_BIN};
use crate::fakes::tls;
use crate::harness::T;

/// The check interval in these tests, ms of the device's time.
pub const DAY: u64 = 600_000;

/// The fake release server, trusted by the simulators this section boots,
/// and the address of its latest manifest.
pub fn releases(t: &mut T) -> (FakeReleases, String) {
    let cert = tls::self_signed();
    let rel = FakeReleases::start(&cert);
    let ca = t.write("ca.pem", &cert.pem);
    t.sim_env = vec![("SIM_CA_FILE".into(), ca)];
    let url = format!("https://127.0.0.1:{}/latest/download/{MANIFEST}", rel.port);
    (rel, url)
}

/// Where the fake server has release `v`'s image.
pub fn bin_url(rel: &FakeReleases, v: &str) -> String {
    format!("https://127.0.0.1:{}/download/{v}/{UPDATE_BIN}", rel.port)
}

/// The checks the device has finished since it booted (its log, sim.err).
pub fn checks(t: &T) -> usize {
    let log = t.read("sim.err");
    log.matches("update: check done").count() + log.matches("update: check failed").count()
}

/// The device's clock (ms since it booted) when each of its checks ended.
pub fn check_times(t: &T) -> Vec<u64> {
    let log = t.read("sim.err");
    let ended = |l: &&str| l.contains("update: check done") || l.contains("update: check failed");
    let stamp = |l: &str| l.split('(').nth(1)?.split(')').next()?.trim().parse::<u64>().ok();
    log.lines().filter(ended).filter_map(stamp).collect()
}

/// What the device said about its checks (for failure messages).
pub fn said(t: &T) -> String {
    let log = t.read("sim.err");
    let lines: Vec<&str> = log.lines().filter(|l| l.contains(" update: ")).collect();
    lines[lines.len().saturating_sub(3)..].join(" / ")
}

/// Boot as firmware `ver`, checking at `url` (None: no checks) once a DAY.
pub fn boot(t: &mut T, ver: &str, url: Option<&str>) {
    let mut args =
        vec!["--set".to_string(), format!("fw_version={ver}"), "--set".to_string(), format!("update_every={DAY}")];
    if let Some(u) = url {
        args.push("--set".into());
        args.push(format!("update_url={u}"));
    }
    let args: Vec<&str> = args.iter().map(String::as_str).collect();
    t.boot_with(&args);
}

/// Wi-Fi joins (the network was set before).
pub fn wifi_up(t: &mut T) -> bool {
    t.wifi_up()
}

/// A day passes and the check it brings is over.
pub fn next_check(t: &mut T) -> bool {
    let n = checks(t);
    t.advance(DAY);
    t.until(5.0, |t| checks(t) > n)
}
