//! The Lua API's own rules: wrong calls point at the app's line, flags are
//! booleans, sys.exit() leaves at once, an app's coroutine can wait inside
//! a task, and the board has an id of its own.

use crate::harness::{grep, T};

const GEN_LUA: &str = r#"R = {}
task(function()
  local gen = coroutine.wrap(function()
    for i = 1, 3 do sleep(10) coroutine.yield(i) end
    R.inside = in_task()
  end)
  local got = {}
  for v in gen do got[#got + 1] = v end
  R.got = table.concat(got, ",")
  local co = coroutine.create(function() yield() return "back" end)
  local ok, v = coroutine.resume(co)
  R.resumed = tostring(ok) .. " " .. tostring(v)
end)
OUTSIDE = coroutine.wrap(function() sleep(10) end)
function loop() end
"#;

pub fn run(t: &mut T) {
    t.say("== lua api: errors at the app's line, flags, sys.exit, coroutines in tasks, sys.info().id");
    t.wipe();
    t.boot();

    for (code, want) in [
        ("do sleep(1) end", "eval:1: sleep() only works inside task()"),
        ("local x = 1 timer.every(5, print)", "eval:1: interval must be >= 10 ms"),
        (r#"do require("http").get("http://127.0.0.1/") end"#, "eval:1: http.request() must be called inside task()"),
        (r#"do require("http").parse_url("ftp://x") end"#, "eval:1: url must start with http:// or https://: ftp://x"),
        (r#"do require("json").encode({ f = print }) end"#, "eval:1: json: cannot encode a function"),
        (r#"do touch.on("poke", print) end"#, r#"eval:1: touch.on: kind is "press""#),
        ("return sleep(1)", "sleep() only works inside task()"), // a tail call: no line of the app left
    ] {
        let v = t.eval(code);
        let builtin = ["prelude", "http.lua", "json.lua"].iter().any(|f| v.contains(f));
        t.check(v.contains(want) && !builtin, &format!("a wrong call reports the app's line: {want}"), |_| {
            format!("{code}: {v}")
        });
    }

    let v = t.eval("timer.cancel(nil) timer.cancel(0/0) timer.cancel(12345) return 'ok'");
    t.check(v == "ok", "timer.cancel of nil or an unknown id does nothing", |_| format!("cancel: {v}"));

    for code in [
        "disp.rect(0, 0, 5, 5, 0xFFFFFF, 0)",
        "disp.circle(5, 5, 3, 0xFFFFFF, 1)",
        "disp.keep_awake(0)",
        "disp.sleep(0)",
        "sys.keep_on(1)",
        r#"crypto.base64("a", 1)"#,
    ] {
        let v = t.eval(code);
        t.check(v.contains("boolean expected"), &format!("a flag must be a boolean: {code}"), |_| {
            format!("{code}: {v}")
        });
    }
    let v = t.eval(r#"net.connect("127.0.0.1", 1, { tls = 1 })"#);
    t.check(v.contains("opts.tls must be true or false"), "net.connect: opts.tls is a boolean", |_| {
        format!("tls: {v}")
    });
    let v = t.eval("do ble.scan(print, { active = 1 }) end");
    t.check(
        v.contains("eval:1: ble.scan: opts.active must be true or false"),
        "ble.scan: opts.active is a boolean",
        |_| format!("active: {v}"),
    );
    let v = t.eval("disp.keep_awake(true), disp.keep_awake(false), disp.rect(0, 0, 2, 2, 1, true)");
    t.check(v == "true\tfalse", "true and false still work", |_| format!("flags: {v}"));

    let id = t.eval("sys.info().id");
    t.check(grep("^[0-9a-f]{12}$", &id), "sys.info().id: 12 hex digits", |_| format!("id: {id}"));

    t.write("gen.lua", GEN_LUA);
    t.e(&["put", &t.path("gen.lua"), "--app", "co", "--run", "gen.lua"]);
    t.advance(200);
    let v = t.eval("R.got, R.inside, R.resumed");
    t.check(v == "1,2,3\ttrue\ttrue back", "an app's coroutine inside a task can sleep and yield", |_| {
        format!("generator: {v}")
    });
    let v = t.eval("OUTSIDE()");
    t.check(v.contains("sleep() only works inside task()"), "... but not outside one", |_| format!("outside: {v}"));

    // sys.exit() in the top-level code: nothing after it runs, not even setup().
    t.write(
        "bye.lua",
        "print(\"top\")\nsys.exit()\nerror(\"still running\")\nfunction setup() error(\"setup ran\") end\n",
    );
    let out = t.e(&["put", &t.path("bye.lua"), "--app", "co", "--run", "bye.lua"]);
    t.advance(100);
    let ok = !out.contains("still running") && t.info("running") == "-" && t.shot("bye.ppm").red_pixels() < 5000;
    t.check(ok, "sys.exit() in the top-level code leaves at once, without an error", |t| {
        format!("put: {out}; running: {}", t.info("running"))
    });
    t.write("caught.lua", "function loop()\n  pcall(sys.exit)\n  error(\"after the pcall\")\nend\n");
    let out = t.e(&["put", &t.path("caught.lua"), "--app", "co", "--run", "caught.lua"]);
    t.advance(200);
    let ok = t.info("running") == "-" && t.shot("caught.ppm").red_pixels() < 5000;
    t.check(ok, "a pcall around sys.exit() doesn't keep the app going", |t| {
        format!("put: {out}; running: {}", t.info("running"))
    });

    let v = t.eval("error('still an error')");
    t.check(v.contains("still an error"), "after sys.exit(), an EVAL's error is still an error", |_| {
        format!("eval: {v}")
    });

    t.stop();
    t.boot();
    let again = t.eval("sys.info().id");
    t.check(again == id, "the id is the same after a restart", |_| format!("id {id}, then {again}"));
}
