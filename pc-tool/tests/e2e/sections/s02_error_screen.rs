//! A runtime error shows the red error screen, also for a broken autostart
//! app at boot.

use crate::harness::T;

pub fn run(t: &mut T) {
    t.say("== error screen on the device");
    t.wipe();
    t.boot();
    t.write("main.lua", "local n = 0\nfunction loop()\n  n = n + 1\n  if n == 5 then error(\"boom\") end\nend\n");
    t.e(&["put", &t.path("main.lua"), "--app", "t", "--run", "main.lua", "--save"]);
    t.e(&["autorun", "t"]);
    let mut red = 0;
    let ok = t.until(3.0, |t| {
        red = t.shot("screen.ppm").red_pixels();
        red > 5000
    });
    t.check(ok, "runtime error shown on the screen", |_| format!("no error screen ({red} red px)"));
    t.stop();
    t.boot(); // broken autostart app at boot: error screen right away
    let ok = t.until(3.0, |t| t.shot("screen.ppm").red_pixels() > 5000);
    t.check(ok, "error screen at boot (autorun)", |_| "no error screen at boot".into());

    // An app that can't start still stops the one running: its sockets and
    // pins are let go either way.
    t.write("live/main.lua", "function loop() disp.clear(0x00ff00) end\n");
    t.e(&["put", &t.path("live/main.lua"), "--app", "live", "--run", "main.lua"]);
    let running = t.until(3.0, |t| t.e(&["info"]).contains("running: live/main.lua"));
    let out = t.e(&["run", "nosuch"]);
    let ok = running
        && out.contains("no such app")
        && t.e(&["info"]).contains("running: -")
        && t.until(3.0, |t| t.shot("screen.ppm").red_pixels() > 5000);
    t.check(ok, "running an app that isn't there stops the old one, on the error screen", |t| {
        format!("running before: {running}; run: {out}; info: {}", t.e(&["info"]))
    });
    t.stop();

    t.say("   an app this firmware won't run (its lua_api): kept, greyed, never started");
    t.wipe();
    t.boot();
    for (name, json) in [("future", r#"{ "title": "Future", "lua_api": 99 }"#), ("plain", r#"{ "title": "Plain" }"#)] {
        t.write(&format!("{name}/main.lua"), "function loop() disp.clear(disp.RED) end\n");
        t.write(&format!("{name}/app.json"), json);
        t.e(&["install", &t.path(name)]);
    }
    t.e(&["autorun", "future"]);
    t.stop();
    t.boot(); // the autostart app won't run: the menu, not its error
    let shot = t.shot("screen.ppm");
    let (running, red) = (t.info("running"), shot.red_pixels());
    let bright = |y0: usize| shot.count(0..200, y0..y0 + 28, |r, g, b| r > 200 && g > 200 && b > 200);
    let (future, plain) = (bright(32), bright(62)); // the rows, sorted by title
    t.check(running == "-" && red < 5000, "an autostart app it won't run: the menu at boot", |_| {
        format!("running {running}, red pixels {red}")
    });
    t.check(future == 0 && plain > 50, "its title is greyed", |_| {
        format!("bright pixels: Future {future}, Plain {plain}")
    });
    t.tap_at(60, 47);
    let (running, red) = (t.info("running"), t.shot("screen.ppm").red_pixels());
    t.tap_at(60, 44); // the options' first row: Autostart, as there is no Open
    let autorun = t.info("autorun");
    t.check(running == "-" && red < 5000 && autorun == "-", "a tap opens its options, without Open", |_| {
        format!("running {running}, red pixels {red}, autorun after the first row: {autorun}")
    });
    t.stop();
}
