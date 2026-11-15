//! A stand-in for api.porssisahko.net.
//!
//! The clock is fixed at Fri 6 Nov 2026 13:30 UTC (15:30 in Finland). Like the
//! real API after 14:00, latest-prices.json covers 01:00 Finnish time today to
//! 01:00 the day after tomorrow, newest first, so the app has to ask for today's
//! first hour with price.json.
//!
//! Prices: today 20 c/kWh from 08 to 18 (Finnish time) and 2 otherwise;
//! tomorrow 1.5 all day. Requests are logged ("LATEST", "PRICE <date>").

use std::sync::Arc;

use serde_json::json;

use super::http::{self, Request, Response, Server};
use super::Log;

/// 2026-11-06 13:30 UTC
const NOW: i64 = 1793971800;
/// November: no daylight saving
const FI: i64 = 2 * 3600;
/// 2026-11-05 23:00 UTC, 01:00 Finnish time
const START: i64 = NOW - 13 * 3600 - 30 * 60 - 3600;

pub struct FakePorssisahko {
    pub port: u16,
    pub log: Log,
    server: Server,
}

impl FakePorssisahko {
    pub fn start() -> FakePorssisahko {
        let log = Log::default();
        let log2 = log.clone();
        let server = http::serve(
            http::Opts { http10: true, date: Some(NOW), ..Default::default() },
            Arc::new(move |r: &Request| handle(r, &log2)),
        );
        FakePorssisahko { port: server.port, log, server }
    }

    pub fn stop(&self) {
        self.server.stop();
    }
}

fn price(t: i64) -> f64 {
    let local = t + FI;
    let (_, _, day) = super::civil(local.div_euclid(86400));
    let hour = local.rem_euclid(86400) / 3600;
    if day == 6 {
        if (8..18).contains(&hour) {
            20.0
        } else {
            2.0
        }
    } else {
        1.5
    }
}

/// 2026-11-06T13:30:00.000Z
fn iso_ms(t_ms: i64) -> String {
    format!("{}.{:03}Z", super::iso(t_ms.div_euclid(1000)), t_ms.rem_euclid(1000))
}

/// "2026-11-06T13:30:00..." (at least the first 19 characters) as UTC seconds.
fn parse_iso(s: &str) -> Option<i64> {
    let n = |a: usize, b: usize| s.get(a..b)?.parse::<i64>().ok();
    let days = super::days_from_civil(n(0, 4)?, n(5, 7)? as u32, n(8, 10)? as u32);
    Some(days * 86400 + n(11, 13)? * 3600 + n(14, 16)? * 60 + n(17, 19)?)
}

fn handle(r: &Request, log: &Log) -> Response {
    match r.path() {
        "/v2/latest-prices.json" => {
            log.line("LATEST");
            let prices: Vec<_> = (0..192)
                .rev()
                .map(|i| {
                    let t = START + 15 * 60 * i;
                    json!({"price": price(t), "startDate": iso_ms(t * 1000), "endDate": iso_ms((t + 15 * 60) * 1000 - 1)})
                })
                .collect();
            Response::json(200, json!({"prices": prices}).to_string())
        }
        "/v2/price.json" => {
            let d = r.query().get("date").cloned().unwrap_or_default();
            log.line(format!("PRICE {d}"));
            match parse_iso(&d) {
                Some(t) => Response::json(200, json!({"price": price(t)}).to_string()),
                None => Response::json(400, "{}"),
            }
        }
        _ => Response::new(404, "text/html", "<h1>404 Not Found</h1>"),
    }
}
