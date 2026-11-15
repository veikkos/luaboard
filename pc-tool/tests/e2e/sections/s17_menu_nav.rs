//! Apps, the home menu, the buttons and touch events, Settings.

use crate::harness::{grep, T};

/// The menu's header has a light-blue line along its bottom edge (y = 28).
const MENU_PX: &str = "local c = disp.get(100, 28) return (c >> 16) < 120 and (c & 255) > 200";
const GRAY: &str =
    "local c = disp.get(6, 40) local r, g, b = c >> 16, (c >> 8) & 255, c & 255 return r > 70 and g > 80 and b > 95 and b < 130";
const SETGRAY: &str =
    "local c = disp.get(6, 195) local r, g, b = c >> 16, (c >> 8) & 255, c & 255 return r > 70 and g > 80 and b > 95 and b < 130";

// touch.on(): the system tells a press from a long press, one event per touch
const EV_LUA: &str = r#"ev = {}
for _, k in ipairs({ "press", "long", "tap", "hold", "swipe" }) do
  touch.on(k, function(a, b, c)
    ev[#ev + 1] = k .. ":" .. tostring(a) .. (b and "," .. b or "") .. (c and "," .. c or "")
    down = touch.button()           -- the button still held when the event came
  end)
end
function evs() return table.concat(ev, " ") end
"#;

/// The "running:" line of `info` (what `E info | grep running` shows).
fn running(t: &T) -> String {
    t.e(&["info"]).lines().filter(|l| l.contains("running")).collect::<Vec<_>>().join("\n")
}

fn hold_at(t: &mut T, x: i32, y: i32) {
    t.ctl(&[&format!("touch={x},{y}"), "advance=1000", "touch=-", "advance=250"]);
}

/// Settings: drag the list to its end and tap Back (the last row).
fn settings_back(t: &mut T) {
    t.ctl(&[
        "touch=60,195",
        "advance=50",
        "touch=60,105",
        "advance=50",
        "touch=60,15",
        "advance=50",
        "touch=-",
        "advance=250",
    ]);
    t.tap_at(60, 195);
}

/// The screen's state from `info`: "on" or "off".
fn screen(t: &T) -> String {
    let info = t.e(&["info"]);
    info.lines().find_map(|l| l.trim().strip_prefix("screen:")).unwrap_or("").trim().to_string()
}

fn percent(s: &str) -> Option<i64> {
    s.trim().parse().ok()
}

pub fn run(t: &mut T) {
    t.say("== apps, home menu, buttons, settings");
    t.wipe();
    t.boot();
    let ok = t.wait_lua(2.0, MENU_PX);
    t.check(ok, "boots into the home menu", |_| "no menu at boot".into());
    let paint = t.root.join("examples/paint").to_string_lossy().into_owned();
    let hello = t.root.join("examples/hello/main.lua").to_string_lossy().into_owned();
    let ok = t.e(&["install", &paint]).contains("INSTALLED paint");
    t.check(ok, "install a project folder", |_| "install folder".into());
    let ok = t.work.join("nvs.bin.flash/paint/main.lua").is_file();
    t.check(ok, "installed app is a file in the simulator's flash folder", |_| "not in the flash folder".into());
    let ok = t.work.join("nvs.bin.flash/paint/app.json").is_file();
    t.check(ok, "install takes the project's app.json along", |_| "no app.json installed".into());
    t.e(&["put", &hello]);
    let apps = t.e(&["apps"]);
    let ok = grep("^paint .*installed", &apps) && grep("^hello .*RAM only", &apps);
    t.check(ok, "apps: installed vs. RAM only", |_| format!("apps: {apps}"));
    t.press("B"); // first row: hello
    let ok = t.until(3.0, |t| t.e(&["info"]).contains("running: hello/main.lua"));
    t.check(ok, "B opens the selected app", |t| format!("B didn't open: {}", running(t)));
    t.press_for("B", 1000); // hold B
    let ok = t.e(&["info"]).contains("running: -") && t.eval(MENU_PX) == "true";
    t.check(ok, "holding B goes back to the menu", |t| format!("long B: {}", running(t)));
    t.press("C"); // down to paint, open
    t.press("B");
    let ok = t.e(&["info"]).contains("running: paint/main.lua");
    t.check(ok, "C moves down the list", |t| format!("C: {}", running(t)));
    t.press("B");
    let ok = t.e(&["info"]).contains("running: paint/main.lua");
    t.check(ok, "a short B goes to the app, not the menu", |_| "short B left the app".into());
    t.eval("sys.exit()");
    let ok = t.until(2.0, |t| t.e(&["info"]).contains("running: -") && t.eval(MENU_PX) == "true");
    t.check(ok, "sys.exit() returns to the menu", |_| "sys.exit".into());
    t.write("ev.lua", EV_LUA);
    t.ctl(&["button=A", "advance=100"]); // held in the menu while the app starts
    t.e(&["put", &t.path("ev.lua"), "--app", "ev", "--run", "ev.lua"]);
    t.ctl(&["advance=1000", "button=-", "advance=200"]);
    let v = t.eval("evs()");
    t.check(v.is_empty(), "a touch from the menu isn't the new app's", |_| format!("focus: {v}"));
    // No EVAL while B is held: the chip's clock also runs in real time, and on a
    // slow machine the CLI call alone could make it 800 ms, the back gesture.
    t.ctl(&["button=B", "advance=200", "button=-", "advance=200"]);
    let v = t.eval("evs(), tostring(down)");
    t.check(v == "press:B\tnil", "a short press arrives when it's let go, not while held", |_| format!("press B: {v}"));
    t.eval("ev = {}");
    t.ctl(&["button=A", "advance=1000", "button=-", "advance=200"]);
    let v = t.eval("evs()");
    t.check(v == "long:A", "a long press is only a long press (no press after it)", |_| format!("long A: {v}"));
    t.eval("ev = {}");
    t.tap_at(50, 60);
    t.ctl(&["touch=200,100", "advance=1000", "touch=-", "advance=200"]);
    t.ctl(&[
        "touch=160,180",
        "advance=50",
        "touch=160,140",
        "advance=50",
        "touch=160,100",
        "advance=50",
        "touch=-",
        "advance=200",
    ]);
    let v = t.eval("evs()");
    t.check(v == "tap:50,60 hold:200,100 swipe:up,160,180", "screen: tap, hold and swipe", |_| {
        format!("screen events: {v}")
    });
    // A, not B: held past 800 ms (real time counts too) A is only a long press.
    let idle = t.eval("touch.button()");
    t.ctl(&["button=A", "advance=100"]);
    let held = t.eval("touch.button()");
    t.ctl(&["button=-", "advance=100"]);
    t.check(idle == "nil" && held == "A", "touch.button(): the button held right now", |_| {
        format!("touch.button: {idle} {held}")
    });
    // Each frame: what touch.button() says, after the frame's events ("P": a
    // press). The button stays held until the frame its press comes, never a
    // frame with neither (a lit button blinked off before its press lit it).
    t.eval(
        r#"sys.rate(50); seen = {}; touch.on("press", function() seen[#seen + 1] = "P" end)
        function loop() seen[#seen + 1] = touch.button() or "-" end"#,
    );
    t.ctl(&["advance=100", "button=A", "advance=200", "button=-", "advance=200"]);
    let seen = t.eval("loop = nil; sys.rate(30); return table.concat(seen)");
    t.check(grep("^-+A+P-+$", &seen), "touch.button() holds until the frame of its press", |_| {
        format!("frames: {seen}")
    });
    t.eval(r#"touch.on("press", function(a) ev[#ev + 1] = "press:" .. a; down = touch.button() end)"#);
    t.eval("ev = {}");
    t.ctl(&["touch=40,236", "advance=50", "touch=40,245", "advance=50", "touch=-", "advance=200"]);
    let v = t.eval("evs()");
    t.check(v == "tap:40,236", "a touch that drifts onto the strip is still one tap", |_| format!("drift: {v}"));
    t.eval("ev = {}");
    t.ctl(&["touch=100,100", "advance=50", "touch=110,108", "advance=50", "touch=-", "advance=200"]);
    let v = t.eval("evs()");
    t.check(v == "tap:100,100", "a tap that rolls a little is still a tap", |_| format!("sloppy tap: {v}"));
    t.eval("ev = {}");
    t.ctl(&["touch=150,242", "advance=50", "touch=150,275", "advance=50", "touch=-", "advance=200"]);
    let v = t.eval("evs()");
    t.check(v == "press:B", "a press that rolls on its button is still a press", |_| format!("roll on B: {v}"));
    t.eval(
        r#"ev = {}; touch.on("drag", function(x, y, x0, y0, ended) ev[#ev + 1] = "drag:" .. x .. "," .. y .. (ended and ",end" or "") end)"#,
    );
    t.ctl(&[
        "touch=100,100",
        "advance=50",
        "touch=100,120",
        "advance=50",
        "touch=100,104",
        "advance=50",
        "touch=-",
        "advance=200",
    ]);
    let v = t.eval("evs()");
    t.check(
        v == "drag:100,120 drag:100,104 drag:100,104,end",
        "a drag is never a tap, even let go where it started",
        |_| format!("drag back: {v}"),
    );
    t.eval("ev = {}");
    t.ctl(&["touch=100,100", "advance=50", "touch=120,100", "advance=50", "touch=-", "advance=200"]);
    let v = t.eval("evs()");
    t.check(
        v == "drag:120,100 drag:120,100,end swipe:right,100,100",
        "a drag let go away from its start is also a swipe",
        |_| format!("drag swipe: {v}"),
    );
    t.eval(r#"ev = {}; touch.on("drag", nil); touch.on("long", nil); touch.on("hold", nil)"#);
    t.ctl(&["button=C", "advance=1000", "button=-", "advance=200"]);
    t.ctl(&["touch=80,90", "advance=1000", "touch=-", "advance=250"]);
    let v = t.eval("evs()");
    t.check(v.is_empty(), "a long press or hold without its handler is nothing (no press, no tap)", |_| {
        format!("no fallback: {v}")
    });
    // A slow app: a frame takes 0.9 s. Two of them, then it's quick again (a
    // command from the PC waits for the frame to end, in real time).
    t.eval("ev = {}; local n = 0; function loop() sys.delay(900) n = n + 1 if n == 2 then loop = nil end end");
    t.ctl(&[
        "advance=100",
        "touch=60,60",
        "advance=60",
        "touch=-",
        "advance=80",
        "touch=60,60",
        "advance=60",
        "touch=-",
        "advance=1700",
    ]);
    let v = t.eval("evs()");
    t.check(v == "tap:60,60 tap:60,60", "two quick taps during one slow frame are two taps", |_| {
        format!("slow frame: {v}")
    });
    t.eval("loop = nil");
    t.ctl(&["advance=1000"]);
    let pid = t.e_bg(
        "wait.out",
        &[
            "--linger",
            "0",
            "eval",
            "print('waiting') local t0 = sys.millis() repeat sys.delay(20) until touch.read() or sys.millis() - t0 > 3000 return touch.read()",
        ],
    );
    t.until(2.0, |t| t.read("wait.out").contains("waiting")); // the call is under way
    t.ctl(&["touch=70,80", "advance=200"]);
    t.wait_bg(pid, 60.0);
    t.ctl(&["touch=-", "advance=300"]);
    let out = t.read("wait.out");
    t.check(grep("70.*80", &out), "a script can wait for a finger inside one call", |_| {
        format!("wait for touch: {out}")
    });
    t.eval("ev = {}");
    t.eval("ev = {}");
    t.ctl(&["touch=165,260", "advance=50", "touch=100,260", "advance=50", "touch=-", "advance=200"]);
    let v = t.eval("evs()");
    t.check(v.is_empty(), "a press slid off its button is cancelled", |_| format!("slide off: {v}"));
    t.ctl(&["button=B", "advance=600"]);
    let v = t.eval("evs()");
    t.check(v.is_empty(), "a held B isn't the app's", |_| format!("held B: {v}"));
    t.ctl(&["advance=400", "button=-", "advance=300"]);
    let ok = t.e(&["info"]).contains("running: -");
    t.check(ok, "holding B leaves the app", |t| format!("hold B: {}", running(t)));
    t.e(&["uninstall", "ev"]);

    // the menu by touch only: hold an app for its options, the error screen
    t.wait_lua(2.0, MENU_PX);
    t.ctl(&["touch=60,45", "advance=100"]);
    let ok = t.eval(GRAY) == "true";
    t.check(ok, "a row shows pressed (gray) under the finger", |t| format!("pressed: {}", t.eval("disp.get(6, 40)")));
    t.ctl(&["touch=60,75", "advance=100"]); // slid: no longer a tap, not pressed
    let ok = t.eval(GRAY) == "false";
    t.check(ok, "... not once the finger slides", |_| "pressed after slide".into());
    t.ctl(&["touch=-", "advance=250"]);
    t.ctl(&["touch=60,200", "advance=1000"]); // hold Settings (nothing to do on a hold) ...
    let ok = t.eval(SETGRAY) == "true";
    t.check(ok, "a held row stays pressed", |t| format!("held row: {}", t.eval("disp.get(6, 195)")));
    t.ctl(&["touch=-", "advance=250"]); // ... and let go: it opens, like a tap
    t.tap_at(256, 45); // in Settings: the bar (at home this would open hello)
    let ok = t.eval("disp.brightness()") == "100" && t.e(&["info"]).contains("running: -");
    t.check(ok, "letting go of a held row acts like a tap", |t| {
        format!("hold-up: {} {}", t.eval("disp.brightness()"), running(t))
    });
    t.tap_at(239, 45); // brightness back to 80, Back
    settings_back(t);
    hold_at(t, 60, 50); // first row: hello -> its options
    t.tap_at(60, 116); // options of a RAM app (24 px rows): Open, Clear data, Uninstall, Back
    t.press("C"); // home again: C moves to paint, B opens it
    t.press("B");
    let ok = t.e(&["info"]).contains("running: paint/main.lua");
    t.check(ok, "holding an app opens its options; Back by touch", |t| format!("hold app: {}", running(t)));
    t.eval("sys.exit()");
    t.until(2.0, |t| t.e(&["info"]).contains("running: -"));
    t.write("boom.lua", "error(\"boom\")\n");
    t.e(&["put", &t.path("boom.lua"), "--app", "boom", "--run", "boom.lua"]);
    hold_at(t, 160, 120);
    let ok = t.until_sim(2.0, |t| t.eval(MENU_PX) == "true" && t.e(&["info"]).contains("running: -"));
    t.check(ok, "holding the error screen goes back to the menu", |_| "error screen hold".into());
    t.e(&["uninstall", "boom"]);
    t.tap_at(60, 200); // tap Settings (bottom row)
    t.press("B"); // brightness: edit, 80 -> 60, save
    t.press("A");
    t.press("A");
    t.press("B");
    let v = t.eval("disp.brightness()");
    t.check(v == "60", "brightness set in Settings", |_| format!("brightness: {v}"));
    t.tap_at(256, 45); // touch: the bar's right end
    let v = t.eval("disp.brightness()");
    t.check(v == "100", "tapping the brightness bar sets it", |_| format!("bar tap: {v}"));
    t.tap_at(210, 45); // half way along
    let b = t.eval("disp.brightness()");
    let ok = percent(&b).is_some_and(|n| (45..=60).contains(&n));
    t.check(ok, &format!("... to where it's tapped ({b}%)"), |_| format!("bar middle: {b}"));
    t.ctl(&[
        "touch=180,45",
        "advance=50",
        "touch=200,45",
        "advance=50",
        "touch=240,45",
        "advance=50",
        "touch=-",
        "advance=250",
    ]);
    let b = t.eval("disp.brightness()");
    let ok = percent(&b).is_some_and(|n| (75..=85).contains(&n));
    t.check(ok, &format!("dragging along the brightness bar sets it ({b}%)"), |_| format!("bar drag: {b}"));
    t.tap_at(290, 45); // the % label (like the name): selects the row only
    let v = t.eval("disp.brightness()");
    t.check(v == b, "the % label isn't part of the bar", |_| format!("label: {v}"));
    t.press("C"); // not in the edit mode: C moves down instead of brightening
    let v = t.eval("disp.brightness()");
    t.check(v == b, "tapping Brightness doesn't start the button edit mode", |_| format!("edit mode: {v}"));
    t.tap_at(60, 195); // Autostart: the picker (Off, paint, Back)
    t.tap_at(60, 75); // second row: paint
    let v = t.e(&["autorun"]);
    t.check(v.contains("AUTORUN paint"), "autostart picked by touch", |_| format!("picker: {v}"));
    t.tap_at(60, 195); // the picker again, first row: off
    t.tap_at(60, 45);
    let v = t.e(&["autorun"]);
    t.check(v.contains("AUTORUN off"), "autostart off by touch", |_| format!("picker off: {v}"));
    // the screen sleeps: after the Screen off time, by Sleep now, by a long press of the power key
    t.tap_at(60, 105); // Screen off: never -> after 1 min
    t.advance(59_000);
    let on = screen(t) == "on";
    t.advance(2_000);
    let ok = on && screen(t) == "off";
    t.check(ok, "the screen goes off after the Screen off time", |t| format!("screen off: {}", screen(t)));
    t.tap_at(60, 105); // wakes it; not a tap on Screen off (that would change it)
    let ok = screen(t) == "on";
    t.check(ok, "a tap wakes the screen and does nothing else", |t| format!("wake tap: {}", screen(t)));
    t.write("keep.lua", "disp.keep_awake(true)\nfunction loop() end\n");
    t.e(&["put", &t.path("keep.lua"), "--app", "keep", "--run", "keep.lua"]);
    t.advance(61_000);
    let kept = screen(t) == "on";
    t.eval("disp.keep_awake(false)");
    t.advance(61_000);
    let ok = kept && screen(t) == "off";
    t.check(ok, "disp.keep_awake holds the screen on until let go", |t| format!("keep_awake: {}", screen(t)));
    t.tap_at(160, 120); // wake
    t.eval("disp.keep_awake(true)");
    t.e(&["stop"]);
    t.advance(61_000);
    let ok = screen(t) == "off";
    t.check(ok, "... and ends with the app", |t| format!("keep_awake after stop: {}", screen(t)));
    t.tap_at(160, 120); // wake (on the home menu: only that)
    t.e(&["uninstall", "keep"]);
    t.tap_at(60, 200); // Settings again
    for _ in 0..5 {
        t.tap_at(60, 105); // 5, 15, 30, 60 min, never
    }
    t.advance(61_000);
    let ok = screen(t) == "on";
    t.check(ok, "Screen off back to never", |t| format!("never: {}", screen(t)));
    // Volume: the same bar as Brightness, right under it
    t.tap_at(256, 75); // the bar's right end
    let full = t.eval("sound.volume()");
    t.tap_at(162, 75); // its left end: off (brightness stops at 10 %)
    let off = t.eval("sound.volume()");
    t.press("B"); // with the buttons: edit, 0 -> 20, save
    t.press("C");
    t.press("C");
    t.press("B");
    let edited = t.eval("sound.volume()");
    let ok = full == "100" && off == "0" && edited == "20";
    t.check(ok, "Volume is a bar too: tap it, or B then A and C", |_| format!("volume: {full} {off} {edited}"));
    t.tap_at(60, 165); // Sleep now
    let off = screen(t) == "off";
    t.ctl(&["power=short", "advance=300"]); // wakes it, doesn't go home
    let ok = off && screen(t) == "on" && t.eval(MENU_PX) == "true";
    t.check(ok, "Sleep now; the power key wakes it", |t| format!("sleep now: {}", screen(t)));
    t.ctl(&["power=long", "advance=300"]);
    let off = screen(t) == "off";
    t.tap_at(160, 120);
    let ok = off && screen(t) == "on";
    t.check(ok, "a long press of the power key turns the screen off", |t| format!("long press: {}", screen(t)));
    settings_back(t);
    let ok = t.eval(MENU_PX) == "true";
    t.check(ok, "Settings' Back row by touch", |_| "settings back".into());
    // With the buttons: down through every row to Back (the information rows are skipped)
    t.tap_at(60, 200); // Settings
    for _ in 0..8 {
        t.press("C");
    }
    let ok = !t.read("sim.err").contains("crash") && t.e(&["info"]).contains("running: -");
    t.check(ok, "the buttons walk Settings down to Back", |t| format!("walk to Back: {}", t.read("sim.err")));
    t.press("B");
    let ok = t.eval(MENU_PX) == "true" && !t.read("sim.err").contains("crash");
    t.check(ok, "... and B there leaves Settings", |_| "B on Back".into());
    t.tap_at(60, 200); // brightness back to 60 (for the reboot check)
    t.tap_at(220, 45);
    settings_back(t);
    t.ctl(&["battery=55"]);
    let ok = t.until(2.0, |t| t.eval("sys.battery()") == "55\ttrue\ttrue");
    t.check(ok, "battery module attached: level and charging", |t| {
        format!("battery attach: {}", t.eval("sys.battery()"))
    });
    let ok = t.e(&["autorun", "hello"]).contains("isn't installed");
    t.check(ok, "autostart refuses an app that isn't installed", |_| "autorun of RAM app".into());
    let ok = t.e(&["autorun", "paint"]).contains("AUTORUN paint");
    t.check(ok, "autostart set", |_| "autorun paint".into());
    t.stop();
    t.boot();
    let ok = t.e(&["info"]).contains("running: paint/main.lua");
    t.check(ok, "autostart app runs at boot", |t| format!("autostart: {}", running(t)));
    let ok = t.eval("disp.brightness()") == "60";
    t.check(ok, "brightness kept across a reboot", |_| "brightness after reboot".into());
    let ok = !grep("^hello", &t.e(&["apps"]));
    t.check(ok, "RAM-only app gone after a reboot", |_| "RAM-only app survived a reboot".into());
    let ok = t.e(&["uninstall", "paint"]).contains("UNINSTALLED")
        && !t.work.join("nvs.bin.flash/paint").exists()
        && t.e(&["autorun"]).contains("AUTORUN off");
    t.check(ok, "uninstall removes the app and its autostart", |_| "uninstall".into());
    // a long list scrolls with the finger, while it's still down
    t.write("list/main.lua", "disp.clear(disp.BLACK)\nfunction loop() end\n");
    let list = t.path("list/main.lua");
    for i in 0..8 {
        t.e(&["put", &list, "--app", &format!("list{i}")]);
    }
    t.e(&["stop"]);
    t.until_sim(2.0, |t| t.eval(MENU_PX) == "true");
    t.advance(300); // the menu sees the new apps
    t.ctl(&[
        "touch=60,170",
        "advance=50",
        "touch=60,140",
        "advance=50",
        "touch=60,110",
        "advance=50",
        "touch=60,80",
        "advance=50",
    ]);
    t.ctl(&["touch=-", "advance=250"]);
    t.tap_at(60, 45); // the first row shown: 3 rows (90 px) down the list
    let ok = t.e(&["info"]).contains("running: list3/");
    t.check(ok, "dragging a list scrolls it a row per row height", |t| format!("list drag: {}", running(t)));
    t.stop();
}
