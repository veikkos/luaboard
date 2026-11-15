//! The spot_price_fi example against a fake porssisahko.net.

use crate::fakes::porssisahko::FakePorssisahko;
use crate::harness::T;

/// Chart pixels (y 80..196, x 30..313) that are the expensive (red) or
/// cheap (green) bar color, on a fresh screenshot.
fn chart_px(t: &T, color: &str) -> usize {
    let want: (i32, i32, i32) = if color == "red" { (240, 100, 50) } else { (46, 204, 113) };
    let near = |v: u8, w: i32| (v as i32 - w).abs() < 12;
    t.shot("pp.ppm").count(30..314, 80..197, |r, g, b| near(r, want.0) && near(g, want.1) && near(b, want.2))
}

pub fn run(t: &mut T) {
    t.say("== spot_price_fi against a fake porssisahko.net");
    let pp = FakePorssisahko::start();
    let log = pp.log.clone();
    let main = std::fs::read_to_string(t.root.join("examples/spot_price_fi/main.lua")).unwrap();
    let from = r#"local API = "https://api.porssisahko.net/v2/""#;
    let to = format!(r#"local API = "http://127.0.0.1:{}/v2/""#, pp.port);
    let main: String = main
        .split_inclusive('\n')
        .map(|l| match l.strip_prefix(from) {
            Some(rest) => format!("{to}{rest}"),
            None => l.to_string(),
        })
        .collect();
    if !main.contains(&format!("127.0.0.1:{}", pp.port)) {
        t.bad("spot_price_fi: API URL not replaced");
    }
    t.write("spot_price_fi/main.lua", main);
    t.wipe();
    t.boot();
    t.e(&["install", &t.path("spot_price_fi"), "--run"]);
    t.until(5.0, |_| log.count("PRICE") == 4);
    t.check(log.has("LATEST"), "fetches the latest prices", |_| "no latest-prices request".into());
    let ok = log.count("PRICE") == 4 && log.has("PRICE 2026-11-05T22:01");
    t.check(ok, "asks for today's missing first hour (4 slots)", |_| {
        let p: String = log.lines().iter().filter(|l| l.contains("PRICE")).map(|l| format!("{l} ")).collect();
        format!("fill: {p}")
    });
    let ok = t.until(3.0, |t| chart_px(t, "red") > 1000);
    let red = chart_px(t, "red");
    t.check(ok, &format!("today: expensive hours in red ({red} px)"), |_| format!("today chart: {red} red px"));
    t.press("C");
    let ok = t.until(3.0, |t| chart_px(t, "red") == 0 && chart_px(t, "green") > 1000);
    t.check(ok, "C shows tomorrow (all cheap)", |t| {
        format!("tomorrow: red {}, green {}", chart_px(t, "red"), chart_px(t, "green"))
    });
    t.advance(5000);
    t.press("B"); // B refreshes, but not within 5 s of the last fetch
    let ok = t.until(3.0, |_| log.count("LATEST") == 2);
    t.check(ok, "B refreshes", |_| format!("refresh: {} fetches", log.count("LATEST")));
    t.crash();
}
