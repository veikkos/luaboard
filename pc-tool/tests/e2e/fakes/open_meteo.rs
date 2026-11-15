//! A stand-in for Open-Meteo's forecast and geocoding APIs.
//!
//!   GET /v1/search?name=...      one place, Tampere (61.4991, 23.7871), for any name
//!   GET /v1/forecast?...         clear sky by day now (6.3 C), 24 hours of
//!                                temperatures with rain in the first three, and
//!                                7 days, the first rainy
//!
//! Times are around the tests' fixed clock (E2E_TIME, 13:30 UTC), as unix
//! seconds (timeformat=unixtime). Requests are logged ("SEARCH Tampere",
//! "FORECAST 61.4991 23.7871").

use std::sync::Arc;

use serde_json::json;

use super::http::{self, Request, Response, Server};
use super::Log;

pub struct FakeOpenMeteo {
    pub port: u16,
    pub log: Log,
    server: Server,
}

impl FakeOpenMeteo {
    pub fn start(now: i64) -> FakeOpenMeteo {
        let log = Log::default();
        let log2 = log.clone();
        let server = http::serve(http::Opts::default(), Arc::new(move |r: &Request| handle(r, &log2, now)));
        FakeOpenMeteo { port: server.port, log, server }
    }

    pub fn stop(&self) {
        self.server.stop();
    }
}

fn forecast(now: i64) -> serde_json::Value {
    let offset = 2 * 3600; // Finland in November
    let hour = now - now.rem_euclid(3600);
    let hours: Vec<i64> = (0..24).map(|i| hour + i * 3600).collect();
    let temps: Vec<f64> = (0..24).map(|i| 6.3 - (i as f64) * 0.25).collect();
    let rain: Vec<f64> = (0..24).map(|i| if i < 3 { 1.5 } else { 0.0 }).collect();
    let midnight = now - (now + offset).rem_euclid(86400);
    let days: Vec<i64> = (0..7).map(|i| midnight + i * 86400).collect();
    json!({
        "latitude": 61.5, "longitude": 23.75, "utc_offset_seconds": offset, "timezone": "Europe/Helsinki",
        "current": {"time": now - now.rem_euclid(900), "interval": 900, "temperature_2m": 6.3,
                    "apparent_temperature": 2.1, "weather_code": 0, "wind_speed_10m": 4.2,
                    "wind_direction_10m": 225, "is_day": 1},
        "hourly": {"time": hours, "temperature_2m": temps, "precipitation": rain},
        "daily": {"time": days, "weather_code": [63, 3, 2, 0, 71, 45, 95],
                  "temperature_2m_max": [7.0, 6.5, 5.0, 4.0, 1.0, 2.0, 8.0],
                  "temperature_2m_min": [2.0, 1.5, 0.0, -3.0, -6.0, -1.0, 3.0],
                  "precipitation_sum": [5.1, 0.0, 0.0, 0.0, 3.5, 0.2, 12.0]},
    })
}

fn handle(r: &Request, log: &Log, now: i64) -> Response {
    let q = r.query();
    let get = |k: &str| q.get(k).cloned().unwrap_or_default();
    match r.path() {
        "/v1/search" => {
            log.line(format!("SEARCH {}", get("name")));
            Response::json(
                200,
                json!({"results": [{"name": "Tampere", "latitude": 61.4991, "longitude": 23.7871,
                                    "country": "Finland"}]})
                .to_string(),
            )
        }
        "/v1/forecast" => {
            log.line(format!("FORECAST {} {}", get("latitude"), get("longitude")));
            Response::json(200, forecast(now).to_string())
        }
        _ => Response::new(404, "text/html", "<h1>404 Not Found</h1>"),
    }
}
