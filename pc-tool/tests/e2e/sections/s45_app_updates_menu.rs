//! App updates on the home screen: the "update" tag on an app's row, the
//! "App updates (N)" row, and its screen (Update all, one app, a failure).

use super::market::{self, app_json, first_check, said};
use crate::harness::{ui, T};

const ACCENT: (i32, i32, i32) = (80, 180, 255);
const DIM: (i32, i32, i32) = (130, 142, 158);

/// Home rows are 30 px from y 32; the tag sits at the right, 11 px down.
fn tag_pixels(t: &T, row: usize) -> usize {
    let y = ui::value_top(row as i32) as usize;
    let near = |v: u8, w: i32| (v as i32 - w).abs() < 30;
    t.shot("home.ppm").count(230..308, y..y + 8, |r, g, b| near(r, ACCENT.0) && near(g, ACCENT.1) && near(b, ACCENT.2))
}

fn done(t: &T) -> bool {
    t.read("sim.err").contains("menu: app updates done")
}

pub fn run(t: &mut T) {
    t.say("== app updates in the menu");
    let market = market::start(t);
    let v = |n: &str, ver: &str| app_json(n, ver);
    let (hello, paint, dev, dev2) = (v("hello", "1.0.1"), v("paint", "1.0.1"), v("dev", "1.0.0"), v("dev2", "1.0.1"));
    market.add("hello", "hello", "1.0.1", 1, &[("app.json", hello.as_bytes()), ("main.lua", b"-- hello 2")], &[]);
    market.add("paint", "paint", "1.0.1", 1, &[("app.json", paint.as_bytes()), ("main.lua", b"-- paint 2")], &[]);
    market.add("dev", "dev", "1.0.0", 1, &[("app.json", dev.as_bytes()), ("main.lua", b"-- dev")], &[]);
    market.add("dev2", "dev2", "1.0.1", 1, &[("app.json", dev2.as_bytes()), ("main.lua", b"-- dev2 2")], &[]);
    market.corrupt("paint/main.lua");
    market.cut("hello/main.lua", 4); // half way through, the connection drops

    market::boot(t, &market);
    let long = r#"{ "title": "Aaa", "version": "1.0.0+a1b2c3d-dirty", "author": "Somebody With A Very Long Name Here", "lua_api": 1 }"#;
    market::install(t, "aaa", long, "-- aaa");
    market::install(t, "dev", &app_json("dev", "1.0.0+a1b2c3d-dirty"), "-- dev");
    market::install(t, "dev2", &app_json("dev2", "1.0.0+a1b2c3d"), "-- dev2");
    market::install(t, "hello", &app_json("hello", "1.0.0"), "-- hello");
    market::install(t, "paint", &app_json("paint", "1.0.0"), "-- paint");
    let ok = first_check(t) && t.until(2.0, |t| t.info("app_updates") == "3");
    t.check(ok, "three updates: hello, paint, and dev2 (a dev build of 1.0.0)", |t| {
        format!("app_updates={} / {}", t.info("app_updates"), said(t))
    });

    // Rows: Aaa, dev, dev2, hello (paint below them); App updates, Settings.
    t.advance(300);
    let (on_dev, on_dev2, on_hello) = (tag_pixels(t, 1), tag_pixels(t, 2), tag_pixels(t, 3));
    t.check(on_dev == 0 && on_dev2 > 20 && on_hello > 20, "the update tag is on the apps with an update", |_| {
        format!("tag pixels: dev {on_dev}, dev2 {on_dev2}, hello {on_hello}")
    });

    t.tap_at(60, ui::fixed(1)); // App updates
    t.tap_at(60, ui::row(0)); // Update all
    let finished = t.until(5.0, done);
    let log = t.read("sim.err");
    let line = log.lines().find(|l| l.contains("menu: app updates done")).unwrap_or("").to_string();
    t.check(finished && line.ends_with("1 updated, 2 failed"), "Update all: one updated, two failed", |t| {
        format!("{line:?} / {}", said(t))
    });
    let kept = market::flash(t, "hello/main.lua") == b"-- hello" && market::flash(t, "paint/main.lua") == b"-- paint";
    t.check(kept && market::flash(t, "dev2/main.lua") == b"-- dev2 2", "the failed ones keep their old files", |t| {
        let f = |p: &str| String::from_utf8_lossy(&market::flash(t, p)).into_owned();
        format!("hello {:?}, paint {:?}, dev2 {:?}", f("hello/main.lua"), f("paint/main.lua"), f("dev2/main.lua"))
    });
    // The log has the whole story (the menu's line is the short "download cut short").
    let cut = t.read("sim.err").contains("apps: install hello: failed: connection closed early");
    t.check(cut, "a download cut half way says so", |t| said(t));
    let ok = t.until(2.0, |t| t.info("app_updates") == "2");
    t.check(ok, "hello and paint are still offered", |t| t.info("app_updates"));

    // The options screen of an app with a long author and a dev stamp: the
    // "version ..., by ..." line stays on the screen.
    t.ctl(&["button=B", "advance=900", "button=-", "advance=300"]); // back home (B held)
    t.ctl(&[&format!("touch=60,{}", ui::row(0)), "advance=900", "touch=-", "advance=300"]); // hold Aaa: its options
    let near = |v: u8, w: i32| (v as i32 - w).abs() < 12;
    let past =
        t.shot("opts.ppm").count(304..320, 32..218, |r, g, b| near(r, DIM.0) && near(g, DIM.1) && near(b, DIM.2));
    t.check(past == 0, "the version line is cut to the screen", |_| format!("{past} dim pixels past x 304"));
    t.stop();
}
