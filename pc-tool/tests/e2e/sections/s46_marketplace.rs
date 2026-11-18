//! Marketplace: the last row of the home list opens the marketplace's apps,
//! each with its tag (update, installed, (i) for setup needed); an app's
//! screen shows the QR code of its setup steps and installs it. With all
//! three fixed rows at home (App updates, the firmware's update, Settings),
//! the list is dragged up to reach Marketplace.

use super::market::{self, app_json, first_check, said};
use super::update::bin_url;
use crate::fakes::marketplace::FakeMarket;
use crate::fakes::releases::{manifest, FakeReleases, MANIFEST};
use crate::fakes::tls;
use crate::harness::{ui, T};

type Rgb = (i32, i32, i32);
const ACCENT: Rgb = (80, 180, 255);
const DIM: Rgb = (130, 142, 158);
const WARN: Rgb = (255, 190, 90);
const TINT: Rgb = (24, 44, 66);

fn near(c: Rgb, d: i32) -> impl Fn(u8, u8, u8) -> bool {
    move |r, g, b| (r as i32 - c.0).abs() < d && (g as i32 - c.1).abs() < d && (b as i32 - c.2).abs() < d
}

/// Pixels of colour c in the tag place of list row `row` (30 px rows from y 32).
fn tag(t: &T, row: usize, c: Rgb) -> usize {
    let y = ui::value_top(row as i32) as usize;
    t.shot("s.ppm").count(200..308, y..y + 8, near(c, 30))
}

fn store_log(t: &T) -> String {
    let log = t.read("sim.err");
    log.lines().rfind(|l| l.contains("menu: store")).unwrap_or("").to_string()
}

pub fn run(t: &mut T) {
    t.say("== Marketplace");
    let market = market::start(t);
    let j = |n: &str, v: &str| app_json(n, v);
    let (alpha, hello, stocks, upd) =
        (j("alpha", "1.0.0"), j("hello", "1.0.0"), j("stocks", "1.0.0"), j("upd", "1.0.1"));
    market.add("alpha", "Alpha", "1.0.0", 1, &[("app.json", alpha.as_bytes()), ("main.lua", b"-- alpha")], &[]);
    market.add("hello", "Hello", "1.0.0", 1, &[("app.json", hello.as_bytes()), ("main.lua", b"-- hello")], &[]);
    let files: &[(&str, &[u8])] = &[("app.json", stocks.as_bytes()), ("main.lua", b"-- stocks")];
    market.add("stocks", "Stocks", "1.0.0", 1, files, &["README.md", "secrets.env.example"]);
    market.add("upd", "Upd", "1.0.1", 1, &[("app.json", upd.as_bytes()), ("main.lua", b"-- upd 2")], &[]);

    market::boot(t, &market);
    market::install(t, "hello", &app_json("Hello", "1.0.0"), "-- hello");
    market::install(t, "upd", &app_json("Upd", "1.0.0"), "-- upd");
    let ok = first_check(t);
    t.check(ok, "the first check is done", |t| said(t));

    // Home: Hello, Upd, then "+ Marketplace" (tinted), App updates (1), Settings.
    t.advance(300);
    let (top, bottom) = (ui::row_top(2) as usize, (ui::row_top(3) - ui::ROW_GAP) as usize);
    let tint = t.shot("home.ppm").count(20..300, top..bottom, near(TINT, 6));
    t.check(tint > 1000, "Marketplace is the last row, set apart", |_| format!("{tint} tinted pixels"));
    t.tap_at(60, ui::row(2));
    t.advance(300);
    let opened = store_log(t);
    t.check(opened.ends_with("4 apps"), "the store lists the marketplace's apps", |_| opened.clone());
    // Rows: Alpha, Hello, Stocks, Upd, Back.
    let (a, h, s, u) = (tag(t, 0, DIM) + tag(t, 0, ACCENT), tag(t, 1, DIM), tag(t, 2, WARN), tag(t, 3, ACCENT));
    t.check(a == 0 && h > 10 && s > 5 && u > 10, "tags: none, installed, (i), update", |_| {
        format!("alpha {a}, hello {h}, stocks {s}, upd {u}")
    });

    t.tap_at(60, ui::row(2)); // Stocks
    t.advance(300);
    let shot = t.shot("app.ppm");
    let (black, white) =
        (shot.count(170..320, 70..218, near((0, 0, 0), 20)), shot.count(170..320, 70..218, near((255, 255, 255), 20)));
    t.check(black > 300 && white > 300, "Stocks needs setup: a QR code to its README", |_| {
        format!("{black} black, {white} white pixels")
    });
    t.advance(5200); // the header's refresh: battery, Wi-Fi
    let later = t.shot("app-later.ppm");
    let changed = (0..ui::HEAD_H as usize)
        .flat_map(|y| (0..150).map(move |x| (x, y)))
        .filter(|&(x, y)| shot.rgb(x, y) != later.rgb(x, y))
        .count();
    t.check(changed == 0, "the header's refresh keeps the app's title", |_| format!("{changed} pixels changed"));
    t.tap_at(60, ui::row(0)); // Install
    let ok = t.until(5.0, |t| t.read("sim.err").contains("apps: install stocks: ok"));
    t.check(ok && market::flash(t, "stocks/main.lua") == b"-- stocks", "Install installs it", |t| said(t));
    t.ctl(&["button=B", "advance=900", "button=-", "advance=300"]); // back to the store
    t.ctl(&["button=B", "advance=900", "button=-", "advance=300"]); // home
    let apps = t.info("apps");
    t.check(apps == "3", "it is in the home list", |_| format!("apps={apps}"));
    t.stop();

    drag_to_marketplace(t);

    // No marketplace address (or no Wi-Fi): the store says so.
    t.wipe();
    t.boot();
    t.tap_at(60, ui::row(0)); // the only row: Marketplace
    t.advance(300);
    let said = store_log(t);
    t.check(said.ends_with("no marketplace address"), "without a marketplace the store says why", |_| said.clone());
    t.stop();
}

/// Five apps, an app update and a firmware update: home has room for four
/// rows above its three fixed ones, so Marketplace (the list's last row) is
/// out of sight until the list is dragged up.
fn drag_to_marketplace(t: &mut T) {
    let cert = tls::self_signed();
    let market = FakeMarket::start(&cert);
    let rel = FakeReleases::start(&cert);
    let ca = t.write("ca.pem", &cert.pem);
    t.sim_env = vec![("SIM_CA_FILE".into(), ca)];
    let upd = app_json("Upd", "1.0.1");
    market.add("upd", "Upd", "1.0.1", 1, &[("app.json", upd.as_bytes()), ("main.lua", b"-- upd 2")], &[]);
    rel.set_manifest(&manifest("v1.1.0", &bin_url(&rel, "v1.1.0"), b"not really firmware", 1, 1));
    t.wipe();
    let (apps_url, update_url) = (market.url(), format!("https://127.0.0.1:{}/latest/download/{MANIFEST}", rel.port));
    t.boot_with(&[
        "--set",
        "fw_version=v1.0.0",
        "--set",
        &format!("update_every={}", market::DAY),
        "--set",
        &format!("apps_url={apps_url}"),
        "--set",
        &format!("update_url={update_url}"),
    ]);
    for name in ["a1", "a2", "a3", "a4"] {
        market::install(t, name, &app_json(name, "1.0.0"), "-- app");
    }
    market::install(t, "upd", &app_json("Upd", "1.0.0"), "-- upd");
    let ok = first_check(t) && t.until(5.0, |t| t.info("update") == "v1.1.0");
    t.check(ok, "an app update and a firmware update are known", |t| format!("{} / {}", said(t), t.info("update")));

    // Rows a1..a4 show; a drag of three rows up scrolls to the end, where
    // Marketplace is the last of the four.
    t.advance(300);
    let (from, to) = (ui::row(3), ui::row(3) - 3 * ui::ROW_H);
    t.ctl(&[
        &format!("touch=60,{from}"),
        "advance=50",
        &format!("touch=60,{to}"),
        "advance=50",
        "touch=-",
        "advance=250",
    ]);
    t.tap_at(60, ui::row(3));
    t.advance(300);
    let opened = store_log(t);
    t.check(opened.ends_with("1 apps"), "dragging the home list up reaches Marketplace", |_| {
        format!("the store's line in the log: '{opened}'")
    });
    t.stop();
}
