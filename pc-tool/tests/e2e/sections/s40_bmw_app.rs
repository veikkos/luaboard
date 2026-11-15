//! The bmw example as a whole, installed and pointed at a fake BMW and a
//! TLS 1.3-only broker: the login screen, values from REST and the stream,
//! a restart, the token refresh, a refused reconnect, a refused refresh, an
//! expired code, losing the broker, log out, and no client ID.

use std::fs;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;

use crate::fakes::bmw::{FakeBmw, GCID, VIN};
use crate::fakes::mqtt::{self, Broker};
use crate::fakes::tls;
use crate::harness::{E2E_TIME, T};

fn battery(soc: u32) -> String {
    format!(
        r#"{{"vin":"{VIN}","data":{{"vehicle.drivetrain.batteryManagement.header":{{"value":{soc},"unit":"percent","timestamp":"2026-11-06T13:31:00.000Z"}}}}}}"#
    )
}

pub fn run(t: &mut T) {
    t.say("== bmw app against a fake BMW");
    let bmw = FakeBmw::start(E2E_TIME as i64);
    let log = bmw.log.clone();
    bmw.set_expires_in(120); // a refresh 90 s after each login
    let cert = tls::self_signed_tls13();
    let refuse = Arc::new(AtomicUsize::new(0)); // CONNECTs to refuse with 5 (not authorized)
    let bad_pw = Arc::new(AtomicUsize::new(0)); // CONNECTs to refuse with 4 (bad user name or password)
    let (check, refuse2, bad_pw2) = (bmw.id_check(), refuse.clone(), bad_pw.clone());
    let broker = Broker::start(mqtt::Opts {
        tls: Some(cert.config.clone()),
        auth: Some(Arc::new(move |_id: &str, user: &str, pw: &str| {
            if refuse2.load(Ordering::SeqCst) > 0 {
                refuse2.fetch_sub(1, Ordering::SeqCst);
                return 5;
            }
            if bad_pw2.load(Ordering::SeqCst) > 0 {
                bad_pw2.fetch_sub(1, Ordering::SeqCst);
                return 4;
            }
            if user == GCID && check(pw) {
                0
            } else {
                4
            }
        })),
        ..Default::default()
    });
    let blog = broker.log.clone();

    t.wipe();
    t.boot();
    t.e(&["wifi", "set", "Home Net", "pw"]); // the network gives the clock (NTP)
    t.wait_lua(3.0, "sys.time() ~= nil");
    let src = t.root.join("marketplace/bmw_cardata");
    for e in fs::read_dir(&src).unwrap().filter_map(|e| e.ok()) {
        let n = e.file_name().to_string_lossy().into_owned();
        if n.ends_with(".lua") || n == "app.json" {
            t.write(&format!("bmw_cardata/{n}"), fs::read(e.path()).unwrap());
        }
    }
    // (the app's "bmw: " log lines are dropped: printed during an eval, they
    // would come back in its result)
    let config = format!(
        "CONFIG = {{ auth_url = \"{0}/gcdm/oauth\", api_url = \"{0}\", stream_url = \"mqtts://127.0.0.1:{1}\", ca = [[\n{2}]] }}\n\
         local print0 = print print = function(s, ...) if not tostring(s):find(\"^bmw: \") then print0(s, ...) end end\n",
        bmw.url(),
        broker.port,
        cert.pem
    );
    let main: String = t
        .read("bmw_cardata/main.lua")
        .split_inclusive('\n')
        .map(|l| if l.starts_with("CONFIG = {") { config.clone() } else { l.to_string() })
        .collect();
    t.check(main.contains(&bmw.url()), "bmw app: test CONFIG in place", |_| "CONFIG line not found".into());
    t.write("bmw_cardata/main.lua", main);
    t.write("bmw_cardata/secrets.env", "bmw_client = test-client\n");
    t.e(&["install", &t.path("bmw_cardata"), "--run"]);

    let ok = t.until_net(2.0, |t| t.eval("UI.mode == 'login' and UI.code") == "BMWT-1234");
    t.check(ok, "bmw app: login screen shows the code", |t| {
        format!("bmw app login: {} {} / {}", t.eval("UI.mode"), t.eval("UI.status"), log.text())
    });
    let v = t.eval("disp.get(20, 140) ~= 0 and disp.get(20, 140) ~= disp.get(2, 2)");
    t.check(v == "true", "bmw app: the code is on a card", |_| format!("bmw app code card: {v}"));
    t.shot("login");

    bmw.approve();
    t.advance(5100); // the first poll
    let ok = t.until_net(3.0, |t| t.eval("UI.mode == 'car' and UI.live and UI.soc") == "78");
    t.check(ok && log.count("REST ") == 6, "bmw app: values after approval", |t| {
        format!(
            "bmw app values: mode {} live {} soc {} status {} / {}",
            t.eval("UI.mode"),
            t.eval("UI.live"),
            t.eval("UI.soc"),
            t.eval("UI.status"),
            log.text()
        )
    });
    let topic = format!("{GCID}/{VIN}");
    t.eval("LOGGED, PRINT = {}, print print = function(s) LOGGED[#LOGGED + 1] = s end");
    broker.push(&topic, &battery(81));
    let ok = t.until_net(2.0, |t| t.eval("UI.soc") == "81");
    t.check(ok, "bmw app: a pushed value updates the screen", |t| format!("bmw app push: {}", t.eval("UI.soc")));
    let v = t.eval("print = PRINT return table.concat(LOGGED, '|')");
    t.check(
        v.starts_with("bmw: stream drivetrain.batteryManagement.header = 81, ") && v.ends_with(" s old"),
        "bmw app: each value is logged with its age",
        |_| format!("bmw app value log: {v}"),
    );
    let image = format!("REST GET /customers/vehicles/{VIN}/image");
    let v = t.eval(r#"tostring(UI.picture) .. " " .. tostring(store.get("picture") ~= nil)"#);
    t.check(v == "fetched true" && log.count(&image) == 1, "bmw app: the picture is fetched once and kept", |_| {
        format!("bmw app picture: {v} / {}", log.count(&image))
    });
    let v = t.eval("disp.get(2, 2) .. ' ' .. disp.get(12, 38)");
    t.check(v == "0 0", "bmw app: black background, behind the picture too", |_| format!("bmw app background: {v}"));
    let v = t.eval("tostring(UI.header)");
    t.check(v == "just now", "bmw app: the top right says how old the values are", |_| format!("bmw app header: {v}"));
    // nothing of a page reaches the bottom line, where problems show
    let footer_clear = "local bad = 0 for y = 214, 239 do for x = 0, 319 do local c = disp.get(x, y)
      if c ~= 0 and c ~= disp.get(154, 230) and c ~= disp.get(166, 230) then bad = bad + 1 end end end return bad";
    let v = t.eval(footer_clear);
    t.check(v == "0", "bmw app: page 1 leaves the bottom line free", |_| format!("bmw app footer: {v} pixels"));
    let v = t.eval("tostring(UI.line1)");
    t.check(v.contains("target 80 %") && v.contains("full at"), "bmw app: page 1 has the target and when full", |_| {
        format!("bmw app page 1 line: {v}")
    });
    t.shot("car");

    let swipe = |t: &mut T, x0: i32, y0: i32, x1: i32, y1: i32| {
        t.ctl(&[
            &format!("touch={x0},{y0}"),
            "advance=50",
            &format!("touch={},{}", (x0 + x1) / 2, (y0 + y1) / 2),
            "advance=50",
            &format!("touch={x1},{y1}"),
            "advance=50",
            "touch=-",
            "advance=200",
        ]);
    };
    // red pixels on the screen: what is open, low or wrong
    let reds = "local n = 0 for y = 30, 211 do for x = 0, 319 do local c = disp.get(x, y)
      if (c >> 16 & 255) > 150 and (c >> 8 & 255) < 120 then n = n + 1 end end end return n";
    let all = format!(
        r#"{{"vin":"{VIN}","data":{{"vehicle.status.serviceDistance.next":{{"value":12000}},"vehicle.body.trunk.isLocked":{{"value":true}},"vehicle.cabin.door.row1.driver.isOpen":{{"value":"CLOSED"}},"vehicle.chassis.axle.row1.wheel.left.tire.pressure":{{"value":250}},"vehicle.chassis.axle.row1.wheel.right.tire.pressure":{{"value":250}},"vehicle.chassis.axle.row1.wheel.left.tire.pressureTarget":{{"value":250}},"vehicle.chassis.axle.row2.wheel.left.tire.pressure":{{"value":250}},"vehicle.chassis.axle.row2.wheel.right.tire.pressure":{{"value":250}},"vehicle.vehicle.preConditioning.activity":{{"value":"HEATING"}}}}}}"#
    );
    broker.push(&format!("{GCID}/{VIN}"), &all);
    t.until_net(2.0, |t| t.eval("UI.soc") == "81");
    t.advance(500);
    swipe(t, 260, 120, 60, 120);
    let left = t.eval("UI.page");
    t.until_net(2.0, |t| t.eval("UI.below and table.concat(UI.below, '|')").contains("service"));
    let v = t.eval(footer_clear);
    t.check(v == "0", "bmw app: page 2 leaves the bottom line free", |_| format!("bmw app footer 2: {v} pixels"));
    let v = t.eval(reds);
    t.check(v == "0", "bmw app: the car page has no red when all is closed and fine", |_| {
        format!("bmw app car page reds: {v}")
    });
    let v = t.eval("table.concat(UI.below, '|')");
    t.check(
        v.contains("12 345 km") && v.contains("service in 12 000 km"),
        "bmw app: the car page has the odometer",
        |_| format!("bmw app car page lines: {v}"),
    );
    swipe(t, 260, 120, 60, 120);
    let left2 = t.eval("UI.page");
    t.press("C");
    let c2 = t.eval("UI.page");
    broker.push(
        &format!("{GCID}/{VIN}"),
        &format!(r#"{{"vin":"{VIN}","data":{{"vehicle.cabin.door.row1.driver.isOpen":{{"value":"OPEN"}}}}}}"#),
    );
    t.until_net(2.0, |t| t.eval("table.concat(UI.below, '|')").contains("door open"));
    let v = t.eval(reds);
    t.check(v != "0", "bmw app: an open door shows red on the car page", |_| format!("bmw app open door reds: {v}"));
    let v = t.eval(
        r#"local m = UI.model UI.model = "i5 eDrive40 Touring Edition" redraw()
        local s, icon = UI.name, 308 - disp.text_width(UI.header, 1) - 34
        UI.model = m redraw()
        return s .. "|" .. tostring(#s > 0 and 12 + disp.text_width(s, 2) <= icon)"#,
    );
    t.check(
        v.starts_with("i5 eDrive40") && v.ends_with("|true"),
        "bmw app: a long name is cut before the warning mark",
        |_| format!("bmw app long name: {v}"),
    );
    broker.push(
        &format!("{GCID}/{VIN}"),
        &format!(
            r#"{{"vin":"{VIN}","data":{{"vehicle.cabin.door.row1.driver.isOpen":{{"value":"CLOSED"}},"vehicle.chassis.axle.row2.wheel.left.tire.pressure":{{"value":200}},"vehicle.chassis.axle.row2.wheel.left.tire.pressureTarget":{{"value":270}}}}}}"#
        ),
    );
    t.until_net(2.0, |t| t.eval("UI.tyre_alarm") == "true");
    let v = t.eval("tostring(UI.warning) .. ' ' .. tostring(UI.tyre_alarm)");
    t.check(v == "true true", "bmw app: a low tyre raises the warning mark", |_| format!("bmw app tyre alarm: {v}"));
    let v = t.eval("UI.targets");
    t.check(v == "2.5|2.7", "bmw app: the target pressures, once per axle", |_| format!("bmw app tyre targets: {v}"));
    t.shot("status");
    t.press("A");
    let a1 = t.eval("UI.page");
    t.press("A");
    let a2 = t.eval("UI.page");
    t.press("C");
    let c = t.eval("UI.page");
    t.check(
        left == "2" && left2 == "2" && c2 == "2" && a1 == "1" && a2 == "1" && c == "2",
        "bmw app: two pages: swipe and buttons change the page",
        |_| format!("bmw app pages: {left} {left2} C:{c2} A:{a1} A:{a2} C:{c}"),
    );
    t.advance(61000);
    let v = t.eval("UI.page");
    t.check(v == "1", "bmw app: back to page 1 after a minute", |_| format!("bmw app idle page: {v}"));
    swipe(t, 160, 200, 160, 40);
    let v = t.eval("UI.page");
    t.check(v == "1", "bmw app: a swipe up doesn't change the page", |_| format!("bmw app swipe up: {v}"));

    t.advance(90000); // the refresh is due
    let ok = t.until_net(2.0, |t| t.eval("UI.refreshes") == "1" && blog.count(&format!("CONNECT {GCID}")) == 2);
    t.check(ok && log.has("TOKEN refresh ok"), "bmw app: refresh reconnects with the new password", |t| {
        format!("bmw app refresh: refreshes {} / {}", t.eval("UI.refreshes"), blog.text())
    });

    let rest = log.count("REST ");
    refuse.store(2, Ordering::SeqCst); // the broker still sees the old connection for a moment
    t.e(&["run", "bmw_cardata"]);
    let ok = t.until_sim(1.0, |t| t.eval("UI.soc") == "81");
    t.check(ok && log.count("DEVICECODE") == 1, "bmw app: restart shows saved values, no login", |t| {
        format!("bmw app restart: soc {} / {}", t.eval("UI.soc"), log.text())
    });
    let v = t.eval("UI.picture");
    t.check(v == "stored" && log.count(&image) == 1, "bmw app: a restart uses the kept picture", |_| {
        format!("bmw app restart picture: {v} / {}", log.count(&image))
    });
    // the tallest picture page 1 takes, and a problem showing: nothing overlaps it
    t.eval(r#"store.set("picture", string.rep(string.char(0x52, 0xAA), 250 * 100)) store.set("picture_w", 250) store.set("picture_h", 100)"#);
    t.e(&["run", "bmw_cardata"]);
    t.until_sim(1.0, |t| t.eval("UI.mode") == "car");
    let v = t.eval(
        r#"UI.status = "offline" redraw() local bg, bad = disp.get(0, 239), 0
        for y = 214, 239 do for x = 0, 319 do local c = disp.get(x, y)
          if c ~= bg and not ((c >> 16 & 255) > (c >> 8 & 255) + 60) then bad = bad + 1 end end end
        UI.status = "" redraw() return bad"#,
    );
    t.check(v == "0", "bmw app: a tall picture and a problem line don't overlap", |_| {
        format!("bmw app overlap: {v} pixels")
    });
    t.advance(10000); // refused at once and after 3 s, then in after 6 more
    let ok = t.until_net(10.0, |t| t.eval("UI.live") == "true"); // (a slow handshake under load moves the tries)
    let quiet = !t.eval("UI.status").contains("another app");
    t.check(ok && quiet && blog.count("AUTHFAIL") == 2, "bmw app: quick reopen retried quietly", |t| {
        format!("bmw app reopen: live {} status {} / {}", t.eval("UI.live"), t.eval("UI.status"), blog.tail(5))
    });
    t.check(log.count("REST ") == rest, "bmw app: no REST call when the saved values are fresh", |_| {
        format!("bmw app restart REST: {}", log.tail(3))
    });

    // The connection drops later (a broker restart) and another client has
    // the account's one connection meanwhile.
    refuse.store(1000, Ordering::SeqCst);
    let fails = blog.count("AUTHFAIL");
    broker.kick();
    let ok = t.until_net(2.0, |t| t.eval("UI.live") == "false") && t.eval("UI.soc") == "81";
    t.check(ok, "bmw app: offline keeps values", |t| {
        format!("bmw app offline: live {} soc {}", t.eval("UI.live"), t.eval("UI.soc"))
    });
    let ok = t.until_net(2.0, |t| t.eval("UI.wait >= 3000") == "true");
    t.check(ok, "bmw app: a dropped stream waits before connecting again", |t| {
        format!("bmw app drop wait: {}", t.eval("UI.wait"))
    });
    t.advance(6 * 60 * 1000);
    let v = t.eval("UI.status");
    t.check(
        v.contains("another app") && blog.count("AUTHFAIL") > fails + 3,
        "bmw app: refused for minutes after a drop: says another app may use the stream",
        |_| format!("bmw app refused later: status {v} / {}", blog.tail(3)),
    );
    refuse.store(0, Ordering::SeqCst);
    t.advance(61000);
    let ok = t.until_net(2.0, |t| t.eval("UI.live") == "true");
    let v = t.eval("UI.status");
    t.check(ok && !v.contains("another app"), "bmw app: back in when the stream is free again", |t| {
        format!("bmw app back: live {} status {v}", t.eval("UI.live"))
    });

    // BMW refuses the id token itself: no refresh storm.
    let refreshes = log.count("TOKEN refresh");
    bad_pw.store(3, Ordering::SeqCst);
    t.e(&["run", "bmw_cardata"]); // a start: a refresh, then the stream refuses its fresh password
    t.advance(25000);
    let ok = t.until_net(2.0, |t| t.eval("UI.live") == "true");
    let n = log.count("TOKEN refresh") - refreshes;
    t.check(ok && n <= 2, "bmw app: a refused password backs off, no refresh storm", |t| {
        format!("bmw app bad password: live {} refreshes {n} / {}", t.eval("UI.live"), blog.tail(4))
    });

    bmw.set_image(None);
    t.eval(r#"store.set("picture", nil) store.set("picture_day", nil)"#);
    t.e(&["run", "bmw_cardata"]);
    let ok = t.until_net(2.0, |t| t.eval("UI.picture") == "none" && t.eval("UI.live") == "true");
    let v = t.eval("tostring(UI.picture) .. ' ' .. tostring(UI.soc)");
    t.check(ok && log.count(&image) == 2 && v == "none 81", "bmw app: no picture when /image fails", |_| {
        format!("bmw app no picture: {v} / {}", log.count(&image))
    });

    bmw.revoke();
    bmw.set_code_expires_in(30);
    t.advance(91000); // the refresh is refused: a new login
    let ok = t.until_net(5.0, |t| t.eval("UI.mode") == "login");
    t.check(ok && log.count("DEVICECODE") == 2, "bmw app: refused refresh asks for a login", |t| {
        format!(
            "bmw app relogin: ok {ok} mode {} codes {} / {}",
            t.eval("UI.mode"),
            log.count("DEVICECODE"),
            log.text()
        )
    });

    t.advance(31000); // nobody logs in
    let ok = t.until_net(5.0, |t| t.eval("UI.mode") == "code_expired");
    t.advance(60000);
    let waits = log.count("DEVICECODE") == 2;
    t.press("B");
    let again = t.until_net(5.0, |_| log.count("DEVICECODE") == 3);
    t.check(ok && waits && again, "bmw app: expired code waits for o", |t| {
        format!("bmw app expired: {} waits {waits} again {again}", t.eval("UI.mode"))
    });

    // Log out: hold the screen, then o. Anything else cancels.
    bmw.approve();
    t.advance(5100);
    t.until_net(3.0, |t| t.eval("UI.mode == 'car' and UI.live") == "true");
    let hold = |t: &mut T| t.ctl(&["touch=160,120", "advance=1000", "touch=-", "advance=50"]);
    hold(t);
    let asked = t.eval("UI.confirm_at ~= nil");
    t.press("A");
    let v = t.eval("tostring(UI.confirm_at) .. ' ' .. UI.mode .. ' ' .. UI.page .. ' ' .. tostring(secrets.get('bmw_refresh') ~= nil)");
    t.check(
        asked == "true" && v == "nil car 1 true",
        "bmw app: holding the screen asks to log out, << cancels",
        |_| format!("bmw app logout cancel: asked {asked} / {v}"),
    );
    let codes = log.count("DEVICECODE");
    hold(t);
    t.press("B");
    // (the fake logs the request before the app has the answer)
    let ok = t.until_net(2.0, |t| log.count("DEVICECODE") == codes + 1 && t.eval("UI.mode") == "login");
    let v = t.eval(
        "UI.mode .. ' ' .. tostring(secrets.get('bmw_refresh')) .. ' ' .. tostring(store.get('vin')) .. ' ' .. tostring(store.get('state')) .. ' ' .. tostring(UI.soc)",
    );
    t.check(ok && v == "login nil nil nil nil", "bmw app: o logs out and asks for a new code", |_| {
        format!("bmw app logout: {v} / {}", log.tail(3))
    });

    let n = log.lines().len();
    let wapp = "bmw_cardata";
    t.e(&["secrets", "rm", wapp, "bmw_client"]);
    t.e(&["run", "bmw_cardata"]);
    let ok = t.until_sim(1.0, |t| t.eval("UI.mode") == "setup");
    t.advance(1000);
    t.check(ok && log.lines().len() == n, "bmw app: no client ID shows the setup", |t| {
        format!("bmw app setup: {} / {}", t.eval("UI.mode"), log.tail(2))
    });
}
