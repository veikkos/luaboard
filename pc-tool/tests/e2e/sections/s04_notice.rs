//! The notice screen when a script draws nothing, `luaboard check` and put
//! refusing syntax errors, and safe mode (a finger on the screen at boot).

use crate::harness::T;

// the notice screen has a blue accent bar along the top
const NOTICE: &str = "local c = disp.get(5, 1) return (c >> 16) < 120 and (c >> 8) & 255 > 150 and c & 255 > 200";

pub fn run(t: &mut T) {
    t.say("== notice when a script draws nothing");
    t.wipe();
    t.boot();
    let widgets = t.root.join("examples/dashboard/widgets.lua").to_string_lossy().into_owned();
    t.e(&["put", &widgets, "--run", "widgets.lua"]);
    let ok = t.wait_lua(2.0, NOTICE);
    t.check(ok, "module run on its own: notice instead of a black screen", |_| "no notice for widgets.lua".into());
    t.write("late.lua", "timer.after(2500, function() disp.clear(disp.RED) end)\n");
    t.e(&["put", &t.path("late.lua"), "--run", "late.lua"]);
    let ok = t.eval(NOTICE) == "false";
    t.check(ok, "no notice right after start", |_| "notice too early".into());
    let ok = t.until_sim(2.0, |t| t.eval(NOTICE) == "true");
    t.check(ok, "running without drawing: notice", |_| "no 'is running' notice".into());
    let ok =
        t.until_sim(2.0, |t| t.eval("disp.get(5, 1)") == t.eval("disp.get(100, 100)") && t.eval(NOTICE) == "false");
    t.check(ok, "first drawing replaces the notice", |_| "notice left on screen".into());
    t.stop();
    t.write("broken.lua", "function loop()\n  disp.text(1, 1, \"x\"\nend\n");
    let (passed, out) = t.cli_status(&[], &["check", &t.path("broken.lua")]);
    if passed {
        t.bad("check passed a syntax error");
    } else {
        t.check(out.contains("broken.lua:3:"), "luaboard check reports file:line", |_| format!("check output: {out}"));
    }
    let (passed, out) = t.cli_status(&[], &["-p", "/nonexistent", "put", &t.path("broken.lua")]);
    if passed {
        t.bad("put uploaded a broken file");
    } else {
        t.check(out.contains("nothing uploaded"), "put refuses files with syntax errors", |_| format!("put: {out}"));
    }

    t.say("== safe mode (finger on the screen at boot)");
    t.wipe();
    t.boot();
    t.write("main.lua", "function loop() disp.clear(disp.BLUE) end\n");
    t.e(&["put", &t.path("main.lua"), "--app", "t", "--save"]);
    t.e(&["autorun", "t"]);
    t.stop();
    t.boot_with(&["--set", "boot_touch=1"]);
    let info = t.e(&["info"]);
    t.check(info.contains("running: -"), "autostart skipped in safe mode", |t| {
        format!("no safe mode: {info} / device: {}", t.read("sim.err"))
    });
    t.stop();
    t.boot(); // and without the finger it does start (so the check above means something)
    let ok = t.until(2.0, |t| t.e(&["info"]).contains("running: t/main.lua"));
    t.check(ok, "autostart without the finger", |t| {
        let info = t.e(&["info"]);
        format!("autostart: {}", info.lines().filter(|l| l.contains("running")).collect::<Vec<_>>().join("\n"))
    });
    t.stop();
}
