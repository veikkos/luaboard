//! A stand-in for Alpaca's market data API (data.alpaca.markets/v2).
//!
//!   GET /v2/stocks/snapshots?symbols=..&feed=iex    the latest trade and the
//!                                                   last two daily bars
//!   GET /v2/stocks/bars?symbols=..&timeframe=5Min.. 5-minute bars, a page per
//!                                                   symbol (next_page_token)
//!
//! `now` is the market's clock, also in the Date header: a Friday, in the
//! test 10:30 in New York and (another fake) 17:00, after the close. SPY rose from 580.00
//! (Thursday's close) to 585.20, QQQ fell from 505.00 to 500.00, and after
//! the close each traded a dollar lower; other symbols are unknown. Both want the keys "key1" / "secret1"
//! (401 otherwise). Requests are logged ("SNAP SPY,QQQ", "BARS SPY,QQQ 5Min
//! <start> <end> <page>").

use std::sync::Arc;

use serde_json::{json, Map, Value};

use super::http::{self, Request, Response, Server};
use super::Log;

pub struct FakeAlpaca {
    pub port: u16,
    pub log: Log,
    server: Server,
}

impl FakeAlpaca {
    pub fn start(now: i64) -> FakeAlpaca {
        let log = Log::default();
        let log2 = log.clone();
        let server = http::serve(
            http::Opts { date: Some(now), ..Default::default() },
            Arc::new(move |r: &Request| handle(r, &log2, now)),
        );
        FakeAlpaca { port: server.port, log, server }
    }

    pub fn stop(&self) {
        self.server.stop();
    }
}

/// UTC seconds -> "2026-11-06T14:30:00Z"
fn iso(t: i64) -> String {
    let (days, s) = (t.div_euclid(86400), t.rem_euclid(86400));
    // civil_from_days
    let z = days + 719468;
    let era = z.div_euclid(146097);
    let doe = z - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + if m <= 2 { 1 } else { 0 };
    format!("{y:04}-{m:02}-{d:02}T{:02}:{:02}:{:02}Z", s / 3600, s % 3600 / 60, s % 60)
}

/// (symbol, previous close, the price now)
const STOCKS: [(&str, f64, f64); 2] = [("SPY", 580.0, 585.2), ("QQQ", 505.0, 500.0)];

/// New York midnight (EST in November) of the day of `t`, in UTC seconds.
fn et_midnight(t: i64) -> i64 {
    t - (t - 5 * 3600).rem_euclid(86400)
}

/// 16:00 in New York on the day of `t`.
fn et_close(t: i64) -> i64 {
    et_midnight(t) + 16 * 3600
}

fn symbols(r: &Request) -> Vec<String> {
    r.query().get("symbols").cloned().unwrap_or_default().split(',').map(str::to_string).collect()
}

fn snapshots(r: &Request, now: i64) -> Value {
    let today = et_midnight(now);
    let mut out = Map::new();
    for s in symbols(r) {
        let Some(&(_, prev, price)) = STOCKS.iter().find(|x| x.0 == s) else { continue };
        let bar = |t: i64, c: f64| json!({"t": iso(t), "o": c, "h": c, "l": c, "c": c, "v": 1000});
        let trade = if now >= et_close(now) { price - 1.0 } else { price };
        out.insert(
            s,
            json!({
                "latestTrade": {"t": iso(now - 2).replace('Z', ".123456789Z"), "p": trade, "s": 100, "x": "V"},
                "dailyBar": bar(today, price),
                "prevDailyBar": bar(today - 86400, prev),
            }),
        );
    }
    Value::Object(out)
}

/// One symbol a page: its 5-minute bars from 9:30 to `now` or the close, a
/// straight line from the previous close to the price now.
fn bars(r: &Request, now: i64) -> Value {
    let q = r.query();
    let wanted = symbols(r);
    let page: usize = q.get("page_token").and_then(|p| p.parse().ok()).unwrap_or(0);
    let open = et_midnight(now) + 9 * 3600 + 30 * 60; // 9:30 in New York
    let n = (now.min(et_close(now)) - open) / 300;
    let mut out = Map::new();
    if let Some(s) = wanted.get(page) {
        if let Some(&(_, prev, price)) = STOCKS.iter().find(|x| x.0 == s) {
            let at = |i: i64| prev + (price - prev) * i as f64 / n as f64;
            let list: Vec<Value> = (0..n)
                .map(|i| json!({"t": iso(open + i * 300), "o": at(i), "h": at(i + 1), "l": at(i), "c": at(i + 1), "v": 500}))
                .collect();
            out.insert(s.clone(), Value::Array(list));
        }
    }
    let next = if page + 1 < wanted.len() { json!((page + 1).to_string()) } else { Value::Null };
    json!({"bars": out, "next_page_token": next})
}

fn handle(r: &Request, log: &Log, now: i64) -> Response {
    if r.header("APCA-API-KEY-ID") != Some("key1") || r.header("APCA-API-SECRET-KEY") != Some("secret1") {
        log.line(format!("UNAUTHORIZED {}", r.path()));
        return Response::json(401, r#"{"message": "unauthorized."}"#);
    }
    let q = r.query();
    let get = |k: &str| q.get(k).cloned().unwrap_or_default();
    if get("feed") != "iex" {
        log.line(format!("FEED {}", get("feed")));
        return Response::json(403, r#"{"message": "subscription does not permit querying recent SIP data"}"#);
    }
    match r.path() {
        "/v2/stocks/snapshots" => {
            log.line(format!("SNAP {}", get("symbols")));
            Response::json(200, snapshots(r, now).to_string())
        }
        "/v2/stocks/bars" => {
            let page = q.get("page_token").cloned().unwrap_or_else(|| "-".into());
            log.line(format!("BARS {} {} {} {} {page}", get("symbols"), get("timeframe"), get("start"), get("end")));
            Response::json(200, bars(r, now).to_string())
        }
        _ => Response::json(404, r#"{"message": "Not Found"}"#),
    }
}
