//! The bmw example's parts: car.lua's own checks, the phone login
//! (bmw_login.lua) and CarData's REST and stream (cardata.lua) against a
//! fake BMW.

use std::sync::Arc;

use crate::fakes::bmw::{FakeBmw, GCID, VIN};
use crate::fakes::mqtt::{self, Broker};
use crate::fakes::tls;
use crate::harness::{E2E_TIME, T};

pub fn run(t: &mut T) {
    t.say("== bmw example: car state");
    t.boot();
    car_data(t);
    t.say("== bmw example: phone login");
    login(t);
    t.say("== bmw example: CarData REST and stream");
    cardata(t);
}

const CD_LUA: &str = r#"CA = [[
$CA]]
login, car, cardata, json = require("bmw_login"), require("car"), require("cardata"), require("json")
R = {}
S = {}
L = login.new{ client_id = "test-client", auth_url = "http://127.0.0.1:$P/gcdm/oauth" }
C = cardata.new{ api_url = "http://127.0.0.1:$P", stream_url = "mqtts://127.0.0.1:$B", ca = CA }
function go(f)
  R.done = false
  task(function() local ok, err = pcall(f) R.err = not ok and tostring(err) or nil R.done = true end)
end
go(function()
  TOK = L:tokens()
  local a = TOK.access
  R.vin = C:vin(a)
  R.model = C:model(a, R.vin)
  R.cid = C:container(a)
  local snap = C:snapshot(a, R.vin, R.cid)
  car.merge(S, snap)
  R.soc = car.view(S, sys.time()).soc
end)
"#;

fn battery(soc: u32) -> String {
    format!(
        r#"{{"vin":"{VIN}","data":{{"vehicle.drivetrain.batteryManagement.header":{{"value":{soc},"unit":"percent","timestamp":"2026-11-06T13:29:00.000Z"}}}}}}"#
    )
}

/// cardata.lua: the first run's REST calls, the daily budget, and the
/// stream over TLS 1.3 with the GCID and id token as its login.
fn cardata(t: &mut T) {
    let bmw = FakeBmw::start(E2E_TIME as i64);
    let log = bmw.log.clone();
    let cert = tls::self_signed_tls13();
    let check = bmw.id_check();
    let broker = Broker::start(mqtt::Opts {
        tls: Some(cert.config.clone()),
        auth: Some(Arc::new(move |_id: &str, user: &str, pw: &str| if user == GCID && check(pw) { 0 } else { 4 })),
        ..Default::default()
    });
    let blog = broker.log.clone();
    bmw.approve(); // (the login part ended with no stored login)
    bmw.add_container("luaboard"); // one an older version of the app made
    let script =
        CD_LUA.replace("$CA", &cert.pem).replace("$P", &bmw.port.to_string()).replace("$B", &broker.port.to_string());
    t.write("cd.lua", script);
    let libs: Vec<String> = ["car.lua", "bmw_login.lua", "cardata.lua"]
        .iter()
        .map(|f| t.root.join("marketplace/bmw_cardata").join(f).to_string_lossy().into_owned())
        .collect();
    let mut args = vec!["put"];
    args.extend(libs.iter().map(String::as_str));
    let cd = t.path("cd.lua");
    args.extend([cd.as_str(), "--run", "cd.lua"]);
    t.e(&args);
    t.advance(5100); // the first poll: approved
    t.wait_lua(3.0, "R.done");
    let v = t.eval(r#"tostring(R.vin) .. " " .. tostring(R.model) .. " " .. tostring(R.cid ~= nil)"#);
    let ok =
        v == format!("{VIN} iX xDrive40 true") && log.count("CONTAINER new luaboard-") == 1 && log.count("REST ") == 6;
    t.check(log.has("CONTAINER deleted luaboard ("), "bmw rest: the older version's container is deleted", |_| {
        format!("bmw rest old container: {}", log.text())
    });
    t.check(ok, "bmw rest: first run", |t| format!("bmw rest first run: {v} {} / {}", t.eval("R.err"), log.text()));
    let v = t.eval("R.soc");
    t.check(v == "78", "bmw rest: snapshot", |_| format!("bmw rest snapshot: soc {v}"));

    t.eval(r#"store.set("container", nil) go(function() R.cid2 = C:container(TOK.access) end)"#);
    t.wait_lua(2.0, "R.done");
    let ok = t.eval("R.cid2 == R.cid") == "true" && log.count("CONTAINER new") == 1;
    t.check(ok, "bmw rest: reuses its container", |t| format!("bmw rest reuse: {} / {}", t.eval("R.err"), log.text()));

    let n = log.count("REST ");
    t.eval(
        r#"store.set("rest_budget", json.encode({ day = "2026-11-06", n = 10 }))
go(function() R.st, R.lim = C:rest(TOK.access, "GET", "/customers/vehicles/mappings") end)"#,
    );
    t.wait_lua(2.0, "R.done");
    let v = t.eval("tostring(R.st) .. ' ' .. tostring(R.lim)");
    t.check(v == "nil daily limit" && log.count("REST ") == n, "bmw rest: daily limit", |_| {
        format!("bmw rest limit: {v} / {}", log.tail(2))
    });

    t.eval(
        r#"store.set("rest_budget", json.encode({ day = "2026-11-06", n = 9 }))
C2 = cardata.new{ api_url = "http://127.0.0.1:1" }
go(function() R.st2, R.e2 = C2:rest(TOK.access, "GET", "/customers/vehicles/mappings") end)"#,
    );
    t.wait_lua(5.0, "R.done"); // (Windows says a closed port is closed only after 2 s of retries)
    let v = t.eval(r#"tostring(R.st2) .. " " .. json.decode(store.get("rest_budget")).n"#);
    t.check(v == "nil 9", "bmw rest: a call BMW never answered isn't counted", |t| {
        format!("bmw rest unanswered: {v} {}", t.eval("R.e2"))
    });

    t.eval(
        r#"store.set("rest_budget", nil)
go(function() local p, e = C:picture(TOK.access, R.vin) R.pic = p and p:sub(1, 8) == "\137PNG\r\n\26\n" R.pic_err = e end)"#,
    );
    t.wait_lua(2.0, "R.done");
    let v = t.eval("tostring(R.pic) .. ' ' .. tostring(R.pic_err)");
    t.check(
        v == "true nil" && log.has(&format!("REST GET /customers/vehicles/{VIN}/image")),
        "bmw rest: picture",
        |_| format!("bmw rest picture: {v} / {}", log.tail(2)),
    );
    bmw.set_image(Some(br#"{"exveErrorMsg":"x"}"#.to_vec()));
    t.eval(r#"go(function() R.pic, R.pic_err = C:picture(TOK.access, R.vin) end)"#);
    t.wait_lua(2.0, "R.done");
    let v = t.eval("tostring(R.pic) .. ' ' .. tostring(R.pic_err)");
    t.check(v == "nil not a PNG", "bmw rest: a picture that isn't a PNG", |_| format!("bmw rest not a PNG: {v}"));

    t.eval(
        r#"go(function()
  C1, R.serr = C:stream(TOK, R.vin, function(d) car.merge(S, d) R.soc = car.view(S, sys.time()).soc end)
end)"#,
    );
    t.wait_lua(3.0, "R.done");
    let topic = format!("{GCID}/{VIN}");
    broker.push(&topic, &battery(81));
    let ok = t.until_net(2.0, |t| t.eval("R.soc") == "81");
    t.check(ok && blog.has(&format!("CONNECT {GCID}")), "bmw stream: values arrive", |t| {
        format!("bmw stream: soc {} err {} / {}", t.eval("R.soc"), t.eval("R.serr"), blog.text())
    });

    broker.push(&topic, "not json");
    broker.push(&topic, r#"{"vin":"OTHER","data":{"vehicle.drivetrain.batteryManagement.header":{"value":5}}}"#);
    broker.push(&topic, r#"{"vin":"WBA0TEST000000001","data":5}"#);
    broker.push(&topic, &battery(82));
    let ok = t.until_net(2.0, |t| t.eval("R.soc") == "82") && t.eval("C1.connected") == "true";
    t.check(ok, "bmw stream: bad messages ignored", |t| {
        format!("bmw stream bad: soc {} connected {} / {}", t.eval("R.soc"), t.eval("C1.connected"), blog.tail(4))
    });
}

const LOGIN_LUA: &str = r#"login = require("bmw_login")
R = { n = 0 }
L = login.new{ client_id = "test-client", auth_url = "http://127.0.0.1:$P/gcdm/oauth",
  on_code = function(c) if c then R.code = c.user_code end R.shown = c ~= nil end }
function go(l)
  R.done = false
  task(function()
    local t, err, kind = (l or L):tokens()
    R.id, R.err, R.kind = t and t.id, err, kind
    R.n = R.n + 1
    R.done = true
  end)
end
go()
"#;

/// bmw_login.lua: the phone login (device code), refresh, and what a
/// refused refresh, a network error and a declined code do.
fn login(t: &mut T) {
    let bmw = FakeBmw::start(E2E_TIME as i64);
    let log = bmw.log.clone();
    t.write("bl.lua", LOGIN_LUA.replace("$P", &bmw.port.to_string()));
    let lib = t.root.join("marketplace/bmw_cardata/bmw_login.lua").to_string_lossy().into_owned();
    t.e(&["put", &lib, &t.path("bl.lua"), "--run", "bl.lua"]);
    t.until_net(2.0, |t| t.eval("R.code") == "BMWT-1234"); // the fake logs before the device has the answer
    let v = t.eval("R.code");
    t.check(v == "BMWT-1234", "bmw login: code shown", |_| format!("bmw login code: {v} / {}", log.text()));

    t.advance(5100); // first poll: pending
    t.advance(4900);
    // The second: slow down. Waited for, not timed to the millisecond: each
    // poll's own time on the network moves the next.
    let slowed = t.until_net(3.0, |_| log.has("POLL slow_down"));
    bmw.approve();
    // The interval is 10 s now: no poll 8 s on, where 5 s had one.
    t.advance(8000);
    let quiet = log.count("POLL") == 2;
    t.advance(2500);
    t.wait_lua(2.0, "R.done");
    let v = t.eval("R.id");
    t.check(slowed && quiet && v == "id-1", "bmw login: polls slow down", |_| {
        format!("bmw login: slowed={slowed} quiet={quiet} id={v} / {}", log.text())
    });

    let v = t.eval(r#"tostring(secrets.get("bmw_refresh") ~= nil) .. " " .. tostring(store.get("gcid"))"#);
    t.check(v == "true gcid-test", "bmw login: refresh token kept as a secret", |_| format!("bmw login kept: {v}"));

    t.eval("L.refresh_at = 0 go()");
    t.wait_lua(2.0, "R.done");
    let v = t.eval("R.id");
    let ok = v == "id-2" && log.has("TOKEN refresh ok") && log.count("DEVICECODE") == 1;
    t.check(ok, "bmw login: refresh", |_| format!("bmw login refresh: {v} / {}", log.text()));

    t.eval(r#"L2 = login.new{ client_id = "test-client", auth_url = "http://127.0.0.1:1/gcdm/oauth" } go(L2)"#);
    t.wait_lua(5.0, "R.done"); // (a closed port: 2 s on Windows)
    let v = t.eval(r#"tostring(R.kind) .. " " .. tostring(secrets.get("bmw_refresh") ~= nil)"#);
    t.check(v == "network true", "bmw login: network error keeps the login", |t| {
        format!("bmw login offline: {v} {}", t.eval("R.err"))
    });

    bmw.revoke();
    t.eval("L.refresh_at = 0 go()");
    let gone_lua = r#"secrets.get("bmw_refresh") == nil"#;
    t.until_net(2.0, |t| log.count("DEVICECODE") == 2 && t.eval(gone_lua) == "true");
    let gone = t.eval(gone_lua) == "true";
    let ok = log.has("TOKEN refresh invalid_grant") && log.count("DEVICECODE") == 2 && gone;
    t.check(ok, "bmw login: refused refresh logs in again", |_| {
        format!("bmw login relogin: gone={gone} / {}", log.text())
    });

    bmw.deny();
    t.advance(5100);
    t.wait_lua(2.0, "R.done");
    let v = t.eval("R.kind");
    t.check(v == "code", "bmw login: declined code", |t| format!("bmw login declined: {v} {}", t.eval("R.err")));
}

/// car.lua's checks (data/bmw_car_test.lua) on the device: PASS/FAIL lines.
fn car_data(t: &mut T) {
    let car = t.root.join("marketplace/bmw_cardata/car.lua").to_string_lossy().into_owned();
    let test = t.root.join("pc-tool/tests/e2e/data/bmw_car_test.lua").to_string_lossy().into_owned();
    let out = t.e(&["put", &car, &test, "--run", "bmw_car_test.lua"]);
    let lines: Vec<&str> = out.lines().map(|l| l.trim_end_matches('\r')).collect();
    for l in &lines {
        if let Some(rest) = l.strip_prefix("PASS ") {
            t.ok(&format!("bmw car: {rest}"));
        } else if let Some(rest) = l.strip_prefix("FAIL ") {
            t.bad(&format!("bmw car: {rest}"));
        }
    }
    if !lines.iter().any(|l| l.starts_with("DONE")) {
        let errs: Vec<&str> = lines
            .iter()
            .copied()
            .filter(|l| {
                let l = l.to_lowercase();
                l.contains("error") || l.contains("not found")
            })
            .take(2)
            .collect();
        t.bad(&format!("bmw car checks didn't finish: {}", errs.join("\n")));
    }
}
