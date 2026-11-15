//! The clock example: its button hints go after a while, a touch brings them back.

use crate::harness::T;

/// Whether something brighter than the background is drawn in rows Y0..Y1.
fn drawn(y0: u32, y1: u32) -> String {
    format!(
        "local n = 0 for y = {y0}, {y1} do for x = 0, 319, 2 do local c = disp.get(x, y) \
         if (c >> 16) + ((c >> 8) & 255) + (c & 255) > 120 then n = n + 1 end end end return n > 0"
    )
}

pub fn run(t: &mut T) {
    let hints = drawn(224, 236); // the bottom rows
    let face = drawn(65, 120); // the digital face's time
    let hints = hints.as_str();
    t.say("== clock example: the button hints");
    t.wipe();
    t.boot();
    t.e(&["wifi", "set", "Home Net", "pw"]); // the network gives the clock (NTP)
    t.wait_lua(3.0, "sys.time() ~= nil");
    let main = t.root.join("marketplace/clock/main.lua").to_string_lossy().into_owned();
    t.e(&["put", &main, "--run", "main.lua"]);
    let ok = t.until(2.0, |t| t.eval(hints) == "true");
    t.check(ok, "the hints show at start", |_| "no hints at start".into());
    t.advance(4000);
    let ok = t.eval(hints) == "true";
    t.check(ok, "... still after 4 s", |_| "hints gone too soon".into());
    t.advance(1500);
    let ok = t.until(2.0, |t| t.eval(hints) == "false");
    t.check(ok, "after 5 s without a touch they go", |_| "hints still there".into());
    let ok = t.eval(&face) == "true";
    t.check(ok, "the face stays", |_| "face gone with the hints".into());

    t.tap_at(160, 100);
    let ok = t.until(2.0, |t| t.eval(hints) == "true");
    t.check(ok, "a tap on the screen brings them back", |_| "no hints after a tap".into());
    t.advance(5500);
    t.until(2.0, |t| t.eval(hints) == "false");
    t.press("C");
    let ok = t.until(2.0, |t| t.eval(hints) == "true");
    t.check(ok, "so does a button, which still does its job", |_| "no hints after C".into());
    let ok = t.eval("disp.get(160, 104) ~= disp.get(1, 1)") == "true"; // the analog face's centre
    t.check(ok, "(C switched to the analog face)", |_| "C didn't switch".into());
    t.stop();
}
