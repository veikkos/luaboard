//! The serial protocol and the sandbox: eval, the watchdog, guards, limits,
//! files, and persistence across a reboot.

use crate::harness::{grep, T};

pub fn run(t: &mut T) {
    t.say("== protocol & sandbox");
    t.wipe();
    t.boot();
    let ok = t.eval("1 + 2") == "3";
    t.check(ok, "eval expression", |_| "eval expression".into());
    // Busy loops: the watchdog ends a call after a second of the device's
    // clock, which skips ahead once the loop is running.
    let over = 1100;
    let v = t.eval_skipping("while true do end", over);
    t.check(v.contains("watchdog"), "watchdog stops busy loop", |_| format!("busy loop: {v}"));
    // (the skip may fall inside a disp.jpeg call, which then looks 3.5 s
    // long: it gets its 2 s at most, and the watchdog's second is still over)
    let v = t.eval_skipping(r#"while true do pcall(disp.jpeg, 0, 0, "x") end"#, 3500);
    t.check(v.contains("watchdog"), "cheap calls that may take long don't push the watchdog back", |_| {
        format!("jpeg loop: {v}")
    });
    let v = t.eval_skipping("local n = 0 while true do pcall(function() while true do end end) n = n + 1 end", over);
    t.check(v.contains("watchdog"), "pcall can't catch the watchdog and go on", |_| format!("pcall retry loop: {v}"));
    let v = t.eval_skipping(
        "local co = coroutine.wrap(function() while true do pcall(function() while true do end end) end end) co()",
        over,
    );
    t.check(v.contains("watchdog"), "nor inside a coroutine", |_| format!("coroutine pcall loop: {v}"));
    let v = t.eval_skipping("xpcall(function() while true do end end, function() while true do end end)", over);
    t.check(v.contains("watchdog"), "an error handler that loops ends with the watchdog too", |_| {
        format!("xpcall handler loop: {v}")
    });
    let ok = t.eval("1 + 2") == "3";
    t.check(ok, "next call runs normally after a trip", |_| "watchdog stayed tripped".into());
    t.write("delays.lua", "function loop() while true do sys.delay(1000) end end\n");
    t.e(&["put", &t.path("delays.lua"), "--run", "delays.lua"]);
    t.advance(12000); // simulated time: the delays go by at once
    let ok = t.e(&["info"]).contains("running: -");
    t.check(ok, "sys.delay can't hold a call past the hard limit", |t| {
        format!("delay loop still running: {}", t.e(&["info"]))
    });
    let ok = t.eval("gpio.write(12, 1)").contains("reserved");
    t.check(ok, "reserved pin guard", |_| "reserved pin guard".into());
    let ok = t.eval("io").lines().any(|l| l == "nil");
    t.check(ok, "io library not exposed", |_| "io library exposed".into());
    let ok = t.eval("disp.W, disp.H") == "320\t240";
    t.check(ok, "screen size from board", |_| "screen size".into());
    let ok = t.eval("touch.read()") == "nil";
    t.check(ok, "touch idle", |_| "touch idle".into());
    let ok = t.eval("sys.battery()") == "nil\tfalse\ttrue";
    t.check(ok, "no battery by default (SE)", |t| format!("no battery: {}", t.eval("sys.battery()")));
    let ok = t.eval("local t = {} for i = 1, 1e7 do t[i] = i end").contains("memory");
    t.check(ok, "heap cap enforced", |_| "heap cap".into());
    let v =
        t.eval(r#"local ok = pcall(string.rep, "x", 1536 * 1024) return tostring(ok) .. " " .. select(3, sys.heap())"#);
    t.check(v == "true 4194304", "a 1.5 MB string fits in the 4 MiB heap (3 MB while it is made)", |_| {
        format!("1.5 MB string: {v}")
    });
    let v =
        t.eval(r#"local ok, err = pcall(string.rep, "x", 4608 * 1024) return tostring(ok) .. " " .. tostring(err)"#);
    t.check(v.contains("false") && v.contains("memory"), "4.5 MB doesn't fit", |_| format!("4.5 MB string: {v}"));
    t.write("main.lua", "print(\"persisted\")\n");
    let (ok, _) =
        t.cli_status(&[], &["-p", &t.tty, "--linger", "0", "put", &t.path("main.lua"), "--app", "t", "--save"]);
    t.check(ok, "upload + install", |_| "upload + install".into());
    let ok = t.e(&["get", "t/main.lua"]) == r#"print("persisted")"#;
    t.check(ok, "download matches", |_| "download".into());
    let ok = t.e(&["get", "main.lua"]) == r#"print("persisted")"#;
    t.check(ok, "bare file name means the app last uploaded to", |_| "default app".into());
    t.write("app.json", r#"{"title": "T"}"#);
    t.e(&["put", &t.path("app.json"), "--app", "t", "--save"]);
    let ok = grep("^t/app.json", &t.e(&["ls"]))
        && t.e(&["rm", "t/app.json"]).contains("removed")
        && !grep("^t/app.json", &t.e(&["ls"]));
    t.check(ok, "rm removes a non-Lua file", |_| "rm non-Lua file".into());
    let ok = t.e(&["rm", "t"]).contains("no such file") && grep("^t/main.lua", &t.e(&["ls"]));
    t.check(ok, "rm of a bare app name removes nothing", |_| "rm bare app name".into());
    t.write("main.lua", "print(\"replaced\")\n");
    t.e(&["put", &t.path("main.lua"), "--app", "t", "--save"]);
    let ok = t.e(&["get", "t/main.lua"]) == r#"print("replaced")"#;
    t.check(ok, "reinstall replaces the file", |_| "reinstall replace".into());

    let ok = t.eval("sys.info().lua_api") == "1" && grep("^ +api: 1$", &t.e(&["info"]));
    t.check(ok, "Lua API version 1 in sys.info and INFO", |t| format!("lua_api: {}", t.eval("sys.info().lua_api")));
    t.write("api/main.lua", "print(\"api ran\")\n");
    t.write("api/app.json", r#"{"title": "Api", "lua_api": 99}"#);
    t.e(&["install", &t.path("api")]);
    let out = t.e(&["run", "api"]);
    let ok = out.contains("needs newer firmware") && !out.contains("api ran");
    t.check(ok, "an app for a newer Lua API doesn't start", |_| format!("run: {out}"));
    t.write("api/app.json", r#"{"title": "Api", "lua_api": "1"}"#);
    t.e(&["install", &t.path("api")]);
    let out = t.e(&["run", "api"]);
    t.check(out.contains("whole number"), "lua_api must be a whole number", |_| format!("run: {out}"));
    t.write("api/app.json", r#"{"title": "Api", "lua_api": 1}"#);
    t.e(&["install", &t.path("api")]);
    let out = t.e(&["run", "api"]);
    t.check(!out.contains("firmware") && !out.contains("lua_api"), "lua_api 1 runs", |_| format!("run: {out}"));
    t.e(&["uninstall", "api"]);
    t.stop();

    t.say("== persistence across reboot");
    t.boot();
    let ok = grep("^t/main.lua", &t.e(&["ls"]));
    t.check(ok, "installed app survived reboot", |_| "app lost on reboot".into());
    t.stop();
}
