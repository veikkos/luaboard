//! The stocks example against a fake Alpaca: with the market open, then
//! after the close.

use crate::fakes::alpaca::FakeAlpaca;
use crate::harness::{E2E_TIME, T};

const UP: (i32, i32, i32) = (46, 204, 113);
const DOWN: (i32, i32, i32) = (240, 84, 84);

/// Chart pixels (x 6..268, y 66..165) near `want`, on a fresh screenshot.
/// The line from 9:30 to 10:30 is about 120 px.
fn chart_px(t: &T, want: (i32, i32, i32)) -> usize {
    let near = |v: u8, w: i32| (v as i32 - w).abs() < 12;
    t.shot("st.ppm").count(6..269, 66..165, |r, g, b| near(r, want.0) && near(g, want.1) && near(b, want.2))
}

/// A fake Alpaca whose clock is `now`, and the app installed and running on
/// a fresh board on the same clock (a wiped one: its clock chip would keep
/// the time of a boot before).
fn start(t: &mut T, now: i64) -> FakeAlpaca {
    let fake = FakeAlpaca::start(now);
    let main = std::fs::read_to_string(t.root.join("marketplace/stocks/main.lua")).unwrap();
    let from = r#"local API = "https://data.alpaca.markets/v2/""#;
    let to = format!(r#"local API = "http://127.0.0.1:{}/v2/""#, fake.port);
    let main: String = main
        .split_inclusive('\n')
        .map(|l| match l.strip_prefix(from) {
            Some(rest) => format!("{to}{rest}"),
            None => l.to_string(),
        })
        .collect();
    if !main.contains(&format!("127.0.0.1:{}", fake.port)) {
        t.bad("stocks: API URL not replaced");
    }
    t.write("stocks/main.lua", main);
    t.write("stocks/app.json", std::fs::read(t.root.join("marketplace/stocks/app.json")).unwrap());
    t.write("stocks/secrets.env", "alpaca_key=key1\nalpaca_secret=secret1\nsymbols=spy, QQQ\n");
    t.wipe();
    t.boot_with(&["--set", &format!("time={now}")]);
    t.e(&["install", &t.path("stocks"), "--run"]);
    fake
}

/// The price and the after-hours line at the top, as drawn.
fn top(t: &T) -> String {
    t.eval("UI.top.price .. ' ' .. tostring(UI.top.after)")
}

pub fn run(t: &mut T) {
    t.say("== stocks against a fake Alpaca");
    let fake = start(t, E2E_TIME as i64 + 2 * 3600); // Friday 15:30 UTC, 10:30 in New York
    let log = fake.log.clone();
    let ok = t.wait_lua(5.0, r#"UI.fetches >= 1 and UI.status == "ok""#);
    t.check(ok, "fetches prices and bars", |t| format!("status: {} / {}", t.eval("UI.status"), log.tail(3)));
    t.check(log.has("SNAP SPY,QQQ"), "asks for its symbols' snapshots, upper case", |_| log.tail(3));
    let session = "2026-11-06T14:30:00Z 2026-11-06T15:30:00Z";
    let ok = log.has(&format!("BARS SPY,QQQ 5Min {session} -")) && log.has(&format!("BARS SPY,QQQ 5Min {session} 1"));
    t.check(ok, "asks for today's session from 9:30, both pages", |_| log.tail(3));
    t.check(t.eval("UI.state") == "open", "the market is open", |t| format!("state {}", t.eval("UI.state")));
    let top_now = top(t);
    t.check(top_now == "585.20 nil", "the latest trade at the top", |_| format!("top: {top_now}"));
    let (up, down) = (chart_px(t, UP), chart_px(t, DOWN));
    t.check(up > 80 && down == 0, &format!("SPY is up: a green line ({up} px)"), |_| {
        format!("SPY chart: green {up}, red {down} px")
    });

    t.press("C");
    let ok = t.wait_lua(2.0, "UI.sel == 2");
    let (up, down) = (chart_px(t, UP), chart_px(t, DOWN));
    t.check(ok && down > 80 && up == 0, "C shows QQQ, down: a red line", |_| {
        format!("QQQ chart: green {up}, red {down} px")
    });
    t.tap_at(80, 210);
    let ok = t.wait_lua(2.0, "UI.sel == 1");
    t.check(ok, "tapping a tile picks it", |t| format!("sel {}", t.eval("UI.sel")));

    t.advance(15000);
    let ok = t.until_net(3.0, |_| log.count("SNAP") == 2);
    t.check(ok && log.count("BARS") == 2, "prices again after 15 s, bars not yet", |_| {
        format!("{} snapshots, {} bars requests", log.count("SNAP"), log.count("BARS"))
    });
    t.check(!log.has("UNAUTHORIZED") && !log.has("FEED"), "sends the keys and asks for IEX", |_| log.tail(3));
    t.stop();
    fake.stop();

    let fake = start(t, E2E_TIME as i64 + 8 * 3600 + 30 * 60); // Friday 22:00 UTC, 17:00 in New York
    let log = fake.log.clone();
    let ok = t.wait_lua(5.0, r#"UI.fetches >= 1 and UI.status == "ok""#);
    t.check(ok && t.eval("UI.state") == "closed", "after 16:00 the market is closed", |t| {
        format!("status {}, state {}", t.eval("UI.status"), t.eval("UI.state"))
    });
    let ok = log.has("BARS SPY,QQQ 5Min 2026-11-06T14:30:00Z 2026-11-06T21:00:00Z -");
    t.check(ok, "the chart is the whole session, to 16:00", |_| log.tail(3));
    let top_now = top(t);
    t.check(top_now == "585.20 After hours 584.20 -0.17%", "the close at the top, after hours under it", |_| {
        format!("top: {top_now}")
    });
    t.advance(15000);
    t.check(log.count("SNAP") == 1, "no 15 s polling while closed", |_| format!("{} snapshots", log.count("SNAP")));
    t.crash();
}
