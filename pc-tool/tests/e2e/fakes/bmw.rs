//! A stand-in for BMW's CarData login (the device-code flow):
//!
//!   POST /gcdm/oauth/device/code   a code for client_id "test-client" and the
//!                                  CarData scopes: user_code "BMWT-1234",
//!                                  interval 5 s, valid 300 s
//!   POST /gcdm/oauth/token         device_code grant: authorization_pending
//!                                  until approve(), slow_down once on the
//!                                  second poll, access_denied after deny();
//!                                  checks PKCE. refresh_token grant: a valid
//!                                  refresh token gives a new set (it stays
//!                                  good until a newer one is used), else
//!                                  invalid_grant. Tokens are acc-N, id-N and
//!                                  ref-N, the GCID "gcid-test".
//!
//! and its REST API (Bearer acc-N and `x-version: v1` required):
//!
//!   GET  /customers/vehicles/mappings              one car, VIN, PRIMARY
//!   GET  /customers/vehicles/<vin>/basicData       modelName "iX xDrive40"
//!   GET  /customers/containers                     the containers made so far
//!   POST /customers/containers                     a new one
//!   DELETE /customers/containers/<id>              one of them
//!   GET  /customers/vehicles/<vin>/image           a car-like PNG (labelled
//!                                                  image/jpeg, as BMW does)
//!   GET  /customers/vehicles/<vin>/telematicData   the values as strings
//!                                                  (charging at 78 %), dated
//!                                                  half an hour before `now`
//!
//! Every event is logged as one line: DEVICECODE, POLL pending|slow_down|ok|
//! denied, TOKEN refresh ok|invalid_grant, REVOKED, REST <method> <path>,
//! CONTAINER new <name>.

use std::collections::{HashMap, HashSet};
use std::sync::{Arc, Mutex};

use serde_json::json;

use super::http::{self, Request, Response, Server};
use super::Log;

pub const CLIENT: &str = "test-client";
pub const GCID: &str = "gcid-test";
pub const VIN: &str = "WBA0TEST000000001";
const SCOPE: &str = "authenticate_user openid cardata:streaming:read cardata:api:read";

#[derive(Clone, Copy, PartialEq)]
enum Wait {
    Pending,
    Approved,
    Denied,
}

struct Code {
    challenge: String,
    polls: u32,
}

struct State {
    codes: HashMap<String, Code>,
    wait: Wait,
    n: u32,
    expires_in: i64,
    code_expires_in: i64,
    access: HashSet<String>,
    id: HashSet<String>,
    refresh: HashSet<String>,
    /// (containerId, name)
    containers: Vec<(String, String)>,
    /// What /image answers with: None is a 404.
    image: Option<Vec<u8>>,
}

pub struct FakeBmw {
    pub port: u16,
    pub log: Log,
    state: Arc<Mutex<State>>,
    server: Server,
}

impl FakeBmw {
    /// `now`: the fixture's current time (UTC seconds; the tests' E2E_TIME).
    pub fn start(now: i64) -> FakeBmw {
        let log = Log::default();
        let state = Arc::new(Mutex::new(State {
            codes: HashMap::new(),
            wait: Wait::Pending,
            n: 0,
            expires_in: 3600,
            code_expires_in: 300,
            access: HashSet::new(),
            id: HashSet::new(),
            refresh: HashSet::new(),
            containers: Vec::new(),
            image: Some(car_picture()),
        }));
        let (log2, state2) = (log.clone(), state.clone());
        let server = http::serve(
            http::Opts::default(),
            Arc::new(move |r: &Request| handle(r, &log2, &mut state2.lock().unwrap(), now)),
        );
        FakeBmw { port: server.port, log, state, server }
    }

    pub fn url(&self) -> String {
        format!("http://127.0.0.1:{}", self.port)
    }

    /// The user approves the code on their phone.
    pub fn approve(&self) {
        self.state.lock().unwrap().wait = Wait::Approved;
    }

    /// The user declines it.
    pub fn deny(&self) {
        self.state.lock().unwrap().wait = Wait::Denied;
    }

    /// A container made earlier (by an older version of the app, say).
    pub fn add_container(&self, name: &str) {
        let mut st = self.state.lock().unwrap();
        let id = format!("cont-{}", st.containers.len() + 1);
        st.containers.push((id, name.to_string()));
    }

    /// What /image answers with from now on (None: 404).
    pub fn set_image(&self, image: Option<Vec<u8>>) {
        self.state.lock().unwrap().image = image;
    }

    /// The tokens' lifetime from now on (seconds).
    pub fn set_expires_in(&self, s: i64) {
        self.state.lock().unwrap().expires_in = s;
    }

    /// How long new device codes are valid (seconds, default 300).
    pub fn set_code_expires_in(&self, s: i64) {
        self.state.lock().unwrap().code_expires_in = s;
    }

    /// Invalidate every token (access removed in the portal, or the 14 days
    /// passed). The next code waits for approval again.
    pub fn revoke(&self) {
        let mut st = self.state.lock().unwrap();
        st.access.clear();
        st.id.clear();
        st.refresh.clear();
        st.wait = Wait::Pending;
        self.log.line("REVOKED");
    }

    /// Is this a current id token (the stream's password)?
    pub fn id_valid(&self, id: &str) -> bool {
        self.state.lock().unwrap().id.contains(id)
    }

    /// The same check for a broker's auth callback.
    pub fn id_check(&self) -> impl Fn(&str) -> bool + Send + Sync + 'static {
        let st = self.state.clone();
        move |id: &str| st.lock().unwrap().id.contains(id)
    }

    pub fn stop(&self) {
        self.server.stop();
    }
}

fn error(status: u16, code: &str) -> Response {
    Response::json(status, json!({"error": code, "error_description": code}).to_string())
}

fn issue(st: &mut State) -> Response {
    st.n += 1;
    let (a, i, r) = (format!("acc-{}", st.n), format!("id-{}", st.n), format!("ref-{}", st.n));
    st.access.insert(a.clone());
    st.id.insert(i.clone());
    st.refresh.insert(r.clone());
    Response::json(
        200,
        json!({"access_token": a, "id_token": i, "refresh_token": r, "expires_in": st.expires_in,
               "token_type": "Bearer", "scope": SCOPE, "gcid": GCID})
        .to_string(),
    )
}

/// A car-like PNG as BMW's /image gives one: transparent around a body in
/// a mid grey (0x5A6470) with two dark wheels, 400x240 RGBA.
pub fn car_picture() -> Vec<u8> {
    crate::pngs::rgba(400, 240, |x, y| {
        let (fx, fy) = (x as f32, y as f32);
        let wheel = |cx: f32| (fx - cx).powi(2) + (fy - 180.0).powi(2) < 30.0 * 30.0;
        if wheel(110.0) || wheel(290.0) {
            [20, 20, 22, 255]
        } else if (60..340).contains(&x) && (110..180).contains(&y) || (130..280).contains(&x) && (70..110).contains(&y)
        {
            [0x5A, 0x64, 0x70, 255]
        } else {
            [0, 0, 0, 0]
        }
    })
}

fn telematic(now: i64) -> serde_json::Value {
    let ts = format!("{}.000Z", super::iso(now - 1800));
    let mut data = serde_json::Map::new();
    for (k, v, unit) in [
        ("vehicle.drivetrain.batteryManagement.header", "78", "percent"),
        ("vehicle.drivetrain.electricEngine.kombiRemainingElectricRange", "312", "km"),
        ("vehicle.powertrain.electric.battery.stateOfCharge.target", "80", "percent"),
        ("vehicle.drivetrain.electricEngine.charging.status", "CHARGINGACTIVE", ""),
        ("vehicle.powertrain.electric.battery.charging.power", "7200", "W"),
        ("vehicle.drivetrain.electricEngine.charging.timeRemaining", "130", "min"),
        ("vehicle.body.chargingPort.status", "CONNECTED", ""),
        ("vehicle.cabin.door.lock.status", "LOCKED", ""),
        ("vehicle.vehicle.travelledDistance", "12345", "km"),
    ] {
        data.insert(k.into(), json!({"value": v, "unit": unit, "timestamp": ts}));
    }
    json!({"telematicData": data})
}

fn rest(r: &Request, path: &str, st: &mut State, log: &Log, now: i64) -> Response {
    log.line(format!("REST {} {path}", r.method));
    if !st.access.contains(r.bearer()) {
        return Response::json(401, json!({"exveErrorMsg": "unauthorized"}).to_string());
    }
    if r.header("x-version") != Some("v1") {
        return Response::json(400, json!({"exveErrorMsg": "x-version missing"}).to_string());
    }
    let car = format!("/customers/vehicles/{VIN}/");
    match (r.method.as_str(), path) {
        ("GET", "/customers/vehicles/mappings") => Response::json(
            200,
            json!([{"vin": VIN, "mappedSince": "2025-10-01T10:00:00.000Z", "mappingType": "PRIMARY"}]).to_string(),
        ),
        ("GET", p) if p == format!("{car}basicData") => {
            Response::json(200, json!({"vin": VIN, "brand": "BMW", "modelName": "iX xDrive40"}).to_string())
        }
        ("GET", "/customers/containers") => {
            let list: Vec<_> = st
                .containers
                .iter()
                .map(|(id, name)| json!({"containerId": id, "name": name, "purpose": "test", "state": "ACTIVE"}))
                .collect();
            Response::json(200, json!({"containers": list}).to_string())
        }
        ("POST", "/customers/containers") => {
            let body: serde_json::Value = serde_json::from_slice(&r.body).unwrap_or_default();
            let name = body["name"].as_str().unwrap_or("").to_string();
            let n = body["technicalDescriptors"].as_array().map_or(0, |a| a.len());
            if name.is_empty() || n == 0 || body["purpose"].as_str().is_none() {
                return Response::json(400, json!({"exveErrorMsg": "bad container"}).to_string());
            }
            let id = format!("cont-{}", st.containers.len() + 1);
            st.containers.push((id.clone(), name.clone()));
            log.line(format!("CONTAINER new {name} ({n} descriptors)"));
            Response::json(201, json!({"containerId": id, "name": name}).to_string())
        }
        ("DELETE", p) if p.starts_with("/customers/containers/") => {
            let id = &p["/customers/containers/".len()..];
            match st.containers.iter().position(|(c, _)| c == id) {
                Some(i) => {
                    let (_, name) = st.containers.remove(i);
                    log.line(format!("CONTAINER deleted {name} ({id})"));
                    Response::new(204, "text/plain", "")
                }
                None => Response::json(404, json!({"exveErrorMsg": "no such container"}).to_string()),
            }
        }
        ("GET", p) if p == format!("{car}image") => match &st.image {
            // BMW labels its PNG as a JPEG
            Some(b) => Response::new(200, "image/jpeg", b.clone()),
            None => Response::json(404, json!({"exveErrorMsg": "no image"}).to_string()),
        },
        ("GET", p) if p == format!("{car}telematicData") => {
            let q = r.query();
            if !st.containers.iter().any(|(id, _)| Some(id) == q.get("containerId")) {
                return Response::json(404, json!({"exveErrorMsg": "no such container"}).to_string());
            }
            Response::json(200, telematic(now).to_string())
        }
        _ => Response::json(404, json!({"exveErrorMsg": "not found"}).to_string()),
    }
}

fn handle(r: &Request, log: &Log, st: &mut State, now: i64) -> Response {
    let path = r.path();
    if path.starts_with("/customers/") {
        return rest(r, path, st, log, now);
    }
    match (r.method.as_str(), path) {
        ("POST", "/gcdm/oauth/device/code") => {
            let f = r.form();
            let g = |k: &str| f.get(k).map(String::as_str).unwrap_or("");
            if g("client_id") != CLIENT {
                log.line("DEVICECODE invalid_client");
                return error(400, "invalid_client");
            }
            if g("response_type") != "device_code" || g("scope") != SCOPE || g("code_challenge_method") != "S256" {
                log.line(format!("DEVICECODE bad request: {:?}", f));
                return error(400, "invalid_request");
            }
            let dc = super::token(16);
            st.codes.insert(dc.clone(), Code { challenge: g("code_challenge").to_string(), polls: 0 });
            log.line("DEVICECODE");
            Response::json(
                200,
                json!({"user_code": "BMWT-1234", "device_code": dc, "interval": 5, "expires_in": st.code_expires_in,
                       "verification_uri": "https://example.invalid/device",
                       "verification_uri_complete": "https://example.invalid/device?user_code=BMWT-1234"})
                .to_string(),
            )
        }
        ("POST", "/gcdm/oauth/token") => {
            let f = r.form();
            let g = |k: &str| f.get(k).map(String::as_str).unwrap_or("");
            if g("client_id") != CLIENT {
                return error(401, "invalid_client");
            }
            match g("grant_type") {
                "urn:ietf:params:oauth:grant-type:device_code" => {
                    let wait = st.wait;
                    let Some(code) = st.codes.get_mut(g("device_code")) else {
                        log.line("POLL unknown code");
                        return error(400, "invalid_grant");
                    };
                    code.polls += 1;
                    if code.polls == 2 {
                        log.line("POLL slow_down");
                        return error(400, "slow_down");
                    }
                    match wait {
                        Wait::Pending => {
                            log.line("POLL pending");
                            error(400, "authorization_pending")
                        }
                        Wait::Denied => {
                            log.line("POLL denied");
                            error(400, "access_denied")
                        }
                        Wait::Approved => {
                            if super::pkce_challenge(g("code_verifier")) != code.challenge {
                                log.line("POLL bad verifier");
                                return error(400, "invalid_grant");
                            }
                            st.codes.remove(g("device_code"));
                            log.line("POLL ok");
                            issue(st)
                        }
                    }
                }
                "refresh_token" => {
                    // A used refresh token stays good until a newer one is
                    // used: an app stopped while its refresh was answered
                    // still has a working one (many providers do this).
                    let t = g("refresh_token").to_string();
                    if !st.refresh.contains(&t) {
                        log.line("TOKEN refresh invalid_grant");
                        return error(400, "invalid_grant");
                    }
                    let n: u32 = t.trim_start_matches("ref-").parse().unwrap_or(0);
                    st.refresh.retain(|r| r.trim_start_matches("ref-").parse::<u32>().unwrap_or(0) >= n);
                    log.line("TOKEN refresh ok");
                    issue(st)
                }
                other => {
                    log.line(format!("TOKEN unsupported {other}"));
                    error(400, "unsupported_grant_type")
                }
            }
        }
        _ => Response::json(404, json!({"error": "not found"}).to_string()),
    }
}
