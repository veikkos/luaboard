//! Hardening: drawing with absurd coordinates, bytecode, sockets, names,
//! apps kept apart, errors contained (deep recursion in C included).

use crate::harness::{free_port, grep, T};

pub fn run(t: &mut T) {
    t.say("== hardening: drawing, bytecode, sockets, names");
    t.wipe();
    t.boot();
    t.eval(
        "disp.rect(2147483000, 10, 1000, 5, 0xFFFFFF, true) disp.rect(0x7fffff00, 0x7fffff00, 0x200, 0x200, 1, true)",
    );
    let ok = t.eval("1 + 1") == "2";
    t.check(ok, "huge rectangles are clipped, no wild write", |_| "device died on a huge rectangle".into());
    let ok = t
        .eval(
            r#"local t = {} for i = 1, 20000 do t[i] = "x" end
        setmetatable(t, { __len = function() return 0x100000001 end }) return #net._join(t)"#,
        )
        .lines()
        .any(|l| l == "20000");
    t.check(ok, "net._join ignores a fake length", |_| "net._join with a fake __len".into());
    let ok = t.eval(r#"disp.circle(0, 0, 2000000000, 1, true) disp.circle(5, 5, 2000000000, 1) disp.line(-2^31, 0, 2^31, 1) return "done""#)
        == "done";
    t.check(ok, "huge circles and lines return at once", |_| "huge circle/line hung the device".into());
    t.eval("disp.pixel(2^32 + 5, 5, 0xFFFFFF)");
    let ok = t.eval("disp.get(5, 5)") == "0";
    t.check(ok, "coordinates saturate instead of wrapping", |_| "2^32+5 wrapped to 5".into());
    t.eval(r#"disp.text(-2^40, 0, ("x"):rep(100000), 0xFFFFFF, 8)"#);
    let ok = t.eval(r#"disp.text_width("ab", 2)"#) == "22";
    t.check(ok, "long off-screen text and text_width", |t| format!("text: {}", t.eval(r#"disp.text_width("ab", 2)"#)));
    let ok = t.eval(r#"disp.text_width("x", 1000)"#).contains("scale must be 1..8");
    t.check(ok, "text_width checks its scale", |_| "text_width scale".into());
    let ok = t.eval("sys.date(2^62)").contains("out of range");
    t.check(ok, "sys.date rejects absurd times", |t| format!("sys.date: {}", t.eval("sys.date(2^62)")));
    let ok = t.eval(r#"sys.date(0, "UTC").year"#) == "1970";
    t.check(ok, "sys.date still works", |_| "sys.date(0)".into());

    let ok = t.eval("string.dump") == "nil";
    t.check(ok, "string.dump is gone", |_| "string.dump exposed".into());
    const BC: &str = r#"load("\27Lua\84\0", "x", "b")"#;
    let ok = t.eval(BC).contains("attempt to load a binary chunk");
    t.check(ok, "load refuses bytecode, even with mode b", |t| format!("bytecode: {}", t.eval(BC)));
    const ENV: &str = r#"load("return x", "c", "bt", { x = 5 })()"#;
    let ok = t.eval(r#"load("return 6 * 7")()"#) == "42" && t.eval(ENV) == "5";
    t.check(ok, "load still runs source, with an env", |t| format!("load source: {}", t.eval(ENV)));
    t.write("main.lua", b"\x1bLuaT\0");
    let ok = t.e(&["put", &t.path("main.lua"), "--app", "bc", "--run", "main.lua"]).contains("binary chunk");
    t.check(ok, "a bytecode main.lua is refused", |_| "bytecode main.lua ran".into());

    let port = free_port();
    let v = t.eval(&format!("local s = net.listen({port}) local m = getmetatable(s) s:close() return m"));
    t.check(v == "false", "socket metatable is locked (no __gc removal)", |_| format!("socket metatable: {v}"));

    t.write("f.tmp", "x");
    let ok = t.e(&["put", &t.path("f.tmp"), "--app", "t"]).contains("bad name");
    t.check(ok, "file names ending in .tmp are refused", |t| {
        format!(".tmp upload: {}", t.e(&["put", &t.path("f.tmp"), "--app", "t"]))
    });
    let help = t.raw(&["help", "NOSUCH"]);
    let ok = grep("^OK PING INFO .* SECRETS SECRET CRASH HELP - see docs/PROTOCOL.md$", &help)
        && help.contains("ERR unknown command 'NOSUCH'");
    t.check(ok, "HELP lists the commands (in any case), an unknown one is refused", |_| format!("help: {help}"));
    let ok = first_line_matches(t, "WIFI SET 0x41", "^ERR usage");
    t.check(ok, "hex arguments are strict (no 0x)", |t| format!("WIFI SET 0x41: {}", t.raw(&["WIFI SET 0x41"])));
    let ok = first_line_matches(t, "PAIR SET 00", "^ERR usage");
    t.check(ok, "PAIR takes only its own subcommands", |t| format!("PAIR SET: {}", t.raw(&["PAIR SET 00"])));
    let ok = first_line_matches(t, "PUT t/a.lua 1 zz", "^ERR usage");
    t.check(ok, "PUT checks its CRC argument", |t| format!("PUT crc: {}", t.raw(&["PUT t/a.lua 1 zz"])));
    let ok = first_line_matches(t, "SECRET SET t k 0x41", "^ERR value must be hex");
    t.check(ok, "secret values are strict hex", |_| "SECRET SET 0x41".into());

    t.write("main.lua", "function loop() end\n");
    t.e(&["put", &t.path("main.lua"), "--app", "br", "--run", "main.lua"]);
    const AT: &str = r#"browser._start("https://x.example/", "http://127.0.0.1:80@evil.example/", "t")"#;
    let ok = t.eval(AT).contains("redirect must be");
    t.check(ok, "a redirect with a user-info host is refused", |t| format!("redirect @: {}", t.eval(AT)));
    const LOOP: &str = r#"browser._start("https://x.example/", "http://127.0.0.1:8888/cb", "t")"#;
    let ok = grep("^[0-9]*$", &t.eval(LOOP));
    t.check(ok, "a loopback redirect is still accepted", |t| format!("redirect: {}", t.eval(LOOP)));
    let ok = first_line_matches(t, "BROWSER", "^OK BROWSER [0-9]* br http://127.0.0.1:8888/cb https://x.example/ t$");
    t.check(ok, "a browser request names the app asking (from the firmware)", |t| {
        format!("BROWSER: {}", t.raw(&["BROWSER"]))
    });
    t.stop();

    t.say("== hardening: apps apart, errors contained");
    t.wipe();
    t.boot();
    t.write(
        "evil/main.lua",
        "KEEP = setmetatable({}, { __gc = function() secrets.set(\"planted\", \"x\") end })\nfunction loop() end\n",
    );
    t.write("victim/main.lua", "function loop() end\n");
    t.e(&["put", &t.path("evil/main.lua"), "--app", "evil", "--run", "main.lua"]);
    t.e(&["put", &t.path("victim/main.lua"), "--app", "victim", "--run", "main.lua"]);
    let ok = grep("^evil  *planted ", &t.e(&["secrets"])) && !grep("^victim ", &t.e(&["secrets"]));
    t.check(ok, "an app's finalizers run as that app, not the next one", |t| {
        format!("finalizer identity: {}", t.e(&["secrets"]))
    });

    let id = t
        .eval(r#"ID = browser._start("https://x.example/", "http://127.0.0.1:8888/cb", "t") return ID"#)
        .lines()
        .last()
        .unwrap_or("")
        .to_string();
    t.raw(&[&format!("BROWSER {id} code=SECRETCODE")]);
    if !t.eval("browser._poll(ID)").contains("SECRETCODE") {
        t.bad("the login answer never reached the app");
    }
    t.e(&["put", &t.path("evil/main.lua"), "--app", "evil", "--run", "main.lua"]);
    let poll = format!("browser._poll({id})");
    let ok = !t.eval(&poll).contains("SECRETCODE");
    t.check(ok, "a finished login's answer is gone when its app stops", |t| {
        format!("next app read the login: {}", t.eval(&poll))
    });

    t.write(
        "req/mod.lua",
        "tries = (tries or 0) + 1\nif tries == 1 then error(\"first try fails\") end\nreturn { ok = true }\n",
    );
    t.write("req/main.lua", "function loop() end\n");
    t.e(&["put", &t.path("req/mod.lua"), "--app", "req"]);
    t.e(&["put", &t.path("req/main.lua"), "--app", "req", "--run", "main.lua"]);
    t.eval(r#"pcall(require, "mod")"#);
    let ok = t.eval(r#"require("mod").ok"#) == "true";
    t.check(ok, "a module that failed once can be required again", |t| {
        format!("require after a failure: {}", t.eval(r#"require("mod").ok"#))
    });

    let ok = t.eval("net.listen(3333)").contains("control port");
    t.check(ok, "net.listen refuses the control port", |t| format!("listen 3333: {}", t.eval("net.listen(3333)")));

    t.write("main.lua", "setmetatable(_G, { __index = function() error(\"no globals here\") end })\n");
    let ok = t.e(&["put", &t.path("main.lua"), "--app", "g", "--run", "main.lua"]).contains("no globals here")
        && t.eval("1 + 1") == "2";
    t.check(ok, "an error looking up setup() stops the script, not the device", |_| {
        "setup lookup: device died?".into()
    });
    let ok =
        t.eval(r#"setmetatable({}, { __tostring = function() error("bad tostring") end })"#).contains("bad tostring")
            && t.eval("1 + 1") == "2";
    t.check(ok, "an EVAL result whose __tostring fails is an error", |_| "EVAL __tostring: device died?".into());
    let ok = t
        .eval("local t = setmetatable({}, { __index = function(t, k) return t[k] end }) return t.x")
        .contains("stack overflow")
        && t.eval("1 + 1") == "2";
    t.check(ok, "deep metamethod recursion is a Lua error", |_| "deep recursion: device died?".into());

    // Library functions that call back into Lua take far more C stack per level
    // than a pcall (gsub keeps a 1 KB buffer on it). The limit is the stack left,
    // so these end in a Lua error too, not off the end of the device's 48 KB stack.
    const GSUB: &str = "local function g(n) if n == 0 then return 'x' end return (string.gsub('a', 'a', function() return g(n-1) end)) end";
    let cases = [
        ("gsub callbacks", GSUB),
        ("string.format of __tostring", "local mt = {} local function g(n) if n == 0 then return 'x' end return string.format('%s', setmetatable({n = n-1}, mt)) end mt.__tostring = function(t) return g(t.n) end"),
        ("table.concat of __index", "local function g(n) if n == 0 then return 'x' end return table.concat(setmetatable({}, {__index = function() return g(n-1) end}), '', 1, 1) end"),
        ("coroutines", "local function g(n) if n == 0 then return 'x' end local ok, r = coroutine.resume(coroutine.create(g), n-1) if not ok then error(r, 0) end return r end"),
    ];
    for (name, code) in cases {
        let r = deep(t, code);
        let ok = r == "false C stack overflow" && t.eval("1 + 1") == "2";
        t.check(ok, &format!("deeply nested {name}: a Lua error"), |_| format!("nested {name}: {r}"));
    }
    let r = t.eval(&format!(
        "{GSUB}
            local ok, e = xpcall(g, function(m) return 'handled: ' .. m end, 200) return e"
    ));
    t.check(r == "handled: C stack overflow", "the overflow's message handler still runs", |_| {
        format!("xpcall handler: {r}")
    });
    let g8 = format!("{GSUB} return g(8)");
    let first_words: Vec<String> =
        deep(t, GSUB).lines().map(|l| l.split(' ').next().unwrap_or("").to_string()).collect();
    let ok = first_words.join("\n") == "false" && t.eval(&g8) == "x";
    t.check(ok, "moderate nesting still works after an overflow", |t| {
        format!("nesting after overflow: {}", t.eval(&g8))
    });
    t.write("main.lua", "local function g(n) return (string.gsub(\"a\", \"a\", function() return g(n + 1) end)) end\nfunction setup() g(0) end\n");
    let ok = t.e(&["put", &t.path("main.lua"), "--app", "deep", "--run", "main.lua"]).contains("C stack overflow")
        && t.eval("1 + 1") == "2";
    t.check(ok, "an app that nests too deep stops with an error, the device stays up", |_| {
        "deep app: device died?".into()
    });
    t.stop();
}

/// Whether the first reply line to a protocol line matches `pattern`.
fn first_line_matches(t: &T, line: &str, pattern: &str) -> bool {
    grep(pattern, t.raw(&[line]).lines().next().unwrap_or(""))
}

/// `code` defines a recursive g(n) that nests one C call per level: g(200)
/// under pcall, as "ok error".
fn deep(t: &T, code: &str) -> String {
    t.eval(&format!("{code} local ok, e = pcall(g, 200) return tostring(ok) .. ' ' .. tostring(e)"))
}
