//! The weather example against a fake Open-Meteo.

use crate::fakes::open_meteo::FakeOpenMeteo;
use crate::harness::{E2E_TIME, T};

/// `sed 's|^FROM|TO|'`: FROM at the start of a line becomes TO.
fn replace_line_start(text: &str, from: &str, to: &str) -> String {
    text.split_inclusive('\n')
        .map(|l| match l.strip_prefix(from) {
            Some(rest) => format!("{to}{rest}"),
            None => l.to_string(),
        })
        .collect()
}

/// Pixels near `want` in x0..x1, y0..y1 of a fresh screenshot.
fn px(t: &T, xs: std::ops::Range<usize>, ys: std::ops::Range<usize>, want: (i32, i32, i32)) -> usize {
    let near = |v: u8, w: i32| (v as i32 - w).abs() < 12;
    t.shot("w.ppm").count(xs, ys, |r, g, b| near(r, want.0) && near(g, want.1) && near(b, want.2))
}

const SUN: (i32, i32, i32) = (255, 205, 50);
const RAIN: (i32, i32, i32) = (70, 140, 255);

pub fn run(t: &mut T) {
    t.say("== weather against a fake Open-Meteo");
    let om = FakeOpenMeteo::start(E2E_TIME as i64);
    let log = om.log.clone();
    let base = format!("http://127.0.0.1:{}/v1/", om.port);
    let main = std::fs::read_to_string(t.root.join("examples/weather/main.lua")).unwrap();
    let main = replace_line_start(
        &main,
        r#"local API = "https://api.open-meteo.com/v1/""#,
        &format!(r#"local API = "{base}""#),
    );
    let main = replace_line_start(
        &main,
        r#"local GEO = "https://geocoding-api.open-meteo.com/v1/""#,
        &format!(r#"local GEO = "{base}""#),
    );
    if main.matches(&base).count() != 2 {
        t.bad("weather: API URLs not replaced");
    }
    t.write("weather/main.lua", main);
    t.write("weather/app.json", std::fs::read(t.root.join("examples/weather/app.json")).unwrap());
    t.write("weather/secrets.env", "place = Tampere\n");
    t.wipe();
    t.boot();
    t.e(&["install", &t.path("weather"), "--run"]);
    let ok = t.wait_lua(5.0, r#"UI.fetches >= 1 and UI.status == "ok""#);
    t.check(ok, "fetches the forecast", |t| format!("status: {} / {}", t.eval("UI.status"), log.tail(3)));
    t.check(log.has("SEARCH Tampere"), "looks up the place by name", |_| format!("no search: {}", log.tail(3)));
    t.check(log.has("FORECAST 61.4991 23.7871"), "asks for the place's coordinates", |_| {
        format!("forecast: {}", log.tail(3))
    });
    let sun = px(t, 0..80, 20..110, SUN);
    t.check(sun > 500, &format!("clear sky by day: a sun ({sun} px)"), |_| format!("sun: {sun} px"));
    let rain = px(t, 30..315, 128..197, RAIN);
    t.check(rain > 100, &format!("rain in the next hours: blue bars ({rain} px)"), |_| format!("rain bars: {rain} px"));

    t.press("C");
    let ok = t.wait_lua(2.0, r#"UI.view == "days""#);
    let sun = px(t, 0..80, 20..110, SUN);
    t.check(ok && sun < 100, "C shows the 7 days", |t| format!("view {}, sun {sun} px", t.eval("UI.view")));
    t.press("A");
    let ok = t.wait_lua(2.0, r#"UI.view == "now""#);
    t.check(ok, "A goes back to now", |t| format!("view {}", t.eval("UI.view")));

    t.advance(5000);
    t.press("B");
    let ok = t.until(3.0, |_| log.count("FORECAST") == 2);
    t.check(ok, "B refreshes", |_| format!("{} forecasts", log.count("FORECAST")));

    t.stop();
    t.boot(); // the place is kept: no second lookup
    t.e(&["run", "weather"]);
    let ok = t.until(5.0, |_| log.count("FORECAST") == 3);
    t.check(ok && log.count("SEARCH") == 1, "after a restart the place isn't looked up again", |_| {
        format!("searches {}, forecasts {}", log.count("SEARCH"), log.count("FORECAST"))
    });
    t.crash();
}
