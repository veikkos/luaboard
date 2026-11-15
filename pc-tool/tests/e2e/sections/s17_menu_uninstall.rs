//! Apps: reinstall while running, uninstall from the menu.

use crate::harness::{grep, T};

const MENU_PX: &str = "local c = disp.get(100, 28) return (c >> 16) < 120 and (c & 255) > 200";

pub fn run(t: &mut T) {
    t.say("== apps: reinstall while running, uninstall from the menu");
    t.wipe();
    t.boot();
    t.wait_lua(2.0, MENU_PX);
    let paint = t.root.join("marketplace/paint").to_string_lossy().into_owned();
    let hello = t.root.join("marketplace/hello/main.lua").to_string_lossy().into_owned();
    t.e(&["install", &paint]);
    t.e(&["put", &hello]);
    let menu_brightness = t.eval("disp.brightness()");
    let live = t.path("live");
    t.write("live/main.lua", "v = 1\nfunction loop() end\n");
    t.e(&["install", &live, "--run"]);
    t.write("live/main.lua", "v = 2\nfunction loop() end\n");
    t.e(&["install", &live]);
    let ok = t.e(&["info"]).contains("running: live/main.lua") && t.eval("v") == "2";
    t.check(ok, "installing the running app restarts it with the new code", |t| {
        let running: Vec<String> = t.e(&["info"]).lines().filter(|l| l.contains("running")).map(String::from).collect();
        format!("install while running: v={}, {}", t.eval("v"), running.join("\n"))
    });
    t.press_for("B", 1000); // back to the menu: hello, live, paint, Get apps, Settings
    t.tap_at(60, 200); // Settings...
    t.press_for("B", 1000); // ...back home, up three: live
    t.press("A");
    t.press("A");
    t.press("A");
    t.press_for("B", 1000); // hold: live's options (Open, Autostart, Clear data, Uninstall, Back)
    t.press("C"); // Uninstall: asks for a confirm
    t.press("C");
    t.press("C");
    t.press("B");
    let ok = grep("^live ", &t.e(&["apps"]));
    t.check(ok, "uninstall waits for the confirm", |_| "uninstalled without a confirm".into());
    t.press("B");
    let apps = t.e(&["apps"]);
    let ok = !grep("^live ", &apps) && grep("^paint ", &apps) && !t.work.join("nvs.bin.flash/live").exists();
    t.check(ok, "uninstall from the menu", |t| format!("menu uninstall: {}", t.e(&["apps"]).replace('\n', " ")));
    if menu_brightness == "100" {
        t.bad("test needs a menu brightness other than 100");
    }
    t.e(&["run", "hello"]);
    t.eval("disp.brightness(100)");
    t.press_for("B", 1000);
    let ok = t.eval("disp.brightness()") == menu_brightness;
    t.check(ok, "an app's brightness change ends with the app", |_| "brightness not restored".into());
    t.stop();
}
