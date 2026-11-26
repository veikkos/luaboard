//! App secrets and the store: each app has its own.

use std::fs;
use std::path::Path;

use crate::harness::{grep, Ppm, T};

const MENU_PX: &str = "local c = disp.get(100, 28) return (c >> 16) < 120 and (c & 255) > 200";

/// One record of the simulator's NVS file: namespace, key, data, each with its length.
fn nvs_rec(ns: &[u8], key: &[u8], data: &[u8]) -> Vec<u8> {
    let mut r = vec![ns.len() as u8];
    r.extend_from_slice(ns);
    r.push(key.len() as u8);
    r.extend_from_slice(key);
    r.extend_from_slice(&(data.len() as u32).to_le_bytes());
    r.extend_from_slice(data);
    r
}

/// `ls -a` of a folder, on one line.
fn ls_a(p: &Path) -> String {
    let mut names = vec![".".to_string(), "..".to_string()];
    let mut rest: Vec<String> = fs::read_dir(p)
        .map(|d| d.flatten().map(|e| e.file_name().to_string_lossy().into_owned()).collect())
        .unwrap_or_default();
    rest.sort();
    names.extend(rest);
    names.join("\n")
}

pub fn run(t: &mut T) {
    t.say("== app secrets: each app has its own");
    t.wipe();
    t.boot();
    t.wait_lua(2.0, MENU_PX);
    t.write("alpha/main.lua", "function loop() end\n");
    t.write("beta/main.lua", "function loop() end\n");
    t.write("alpha/secrets.env", "token = from-env\n");
    let (alpha, beta) = (t.path("alpha"), t.path("beta"));
    t.e(&["install", &alpha]);
    t.e(&["install", &beta]);
    t.e(&["run", "alpha"]);
    t.eval(r#"secrets.set("shared", "alpha value")"#);
    let v = t.eval(r#"secrets.get("token")"#);
    t.check(v == "from-env", "secrets.env goes to the installed app", |_| format!("app secret from secrets.env: {v}"));
    t.e(&["run", "beta"]);
    let ok = t.eval(r#"secrets.get("token")"#) == "nil" && t.eval("#secrets.keys()") == "0";
    t.check(ok, "an app can't read or list another app's secrets", |t| {
        format!("beta sees: {} / {}", t.eval(r#"secrets.get("token")"#), t.eval("#secrets.keys()"))
    });
    t.eval(r#"secrets.set("shared", "beta value")"#);
    t.e(&["run", "alpha"]);
    let v = t.eval(r#"secrets.get("shared")"#);
    t.check(v == "alpha value", "the same name in two apps: two secrets", |_| format!("shared: {v}"));
    let list = t.e(&["secrets"]);
    let ok = grep("^alpha  *shared  *11 B", &list) && grep("^beta  *shared  *10 B", &list) && !list.contains("value");
    t.check(ok, "listing: app, name and size, never values", |_| format!("listing: {list}"));
    t.e(&["secrets", "rm", "beta", "shared"]);
    let ok = !grep("^beta ", &t.e(&["secrets"])) && grep("^alpha  *shared ", &t.e(&["secrets"]));
    t.check(ok, "secrets rm APP NAME deletes only that app's secret", |t| {
        format!("rm beta shared: {}", t.e(&["secrets"]))
    });
    let out = t.e(&["install", &alpha]);
    t.e(&["run", "alpha"]);
    let ok = t.eval(r#"secrets.get("shared")"#) == "alpha value";
    t.check(ok, "reinstalling keeps the app's secrets", |_| "reinstall lost secrets".into());
    let note = "note: app 'alpha' has secrets stored on the device (named: shared)";
    t.check(
        out.contains(note),
        "install says which kept secrets the new code can read (not those from secrets.env)",
        |_| format!("no kept-secrets note: {out}"),
    );
    let out = t.e(&["put", &t.path("alpha/main.lua")]);
    t.check(out.contains(note), "put says it too", |_| format!("no note on put: {out}"));
    t.write("gamma/main.lua", "function loop() end\n");
    let long = "k".repeat(2000);
    t.write("gamma/secrets.env", format!("key = {long}\n"));
    let gamma = t.path("gamma");
    t.e(&["install", &gamma]);
    t.e(&["run", "gamma"]);
    let ok = t.eval(r#"#secrets.get("key")"#) == "2000"
        && t.eval(r#"secrets.get("key") == string.rep("k", 2000)"#) == "true";
    t.check(ok, "a 2000-byte secret goes over in pieces", |t| {
        format!("long secret: {}", t.eval(r#"#(secrets.get("key") or "")"#))
    });
    t.write("gamma/secrets.env", "_tok = x\n");
    let ok = t.e(&["install", &gamma]).contains("not starting with _");
    t.check(ok, "a secret name starting with _ is refused on the PC", |_| "_tok accepted on the PC".into());
    let _ = fs::remove_file(t.work.join("gamma/secrets.env"));
    let full = t
        .eval(r#"store.set("big", string.rep("a", 40000)) store.set("big", string.rep("b", 40000))"#)
        .contains("store full");
    t.check(!full, "replacing a value doesn't count the old one against the limit", |_| {
        "replacing a big value counted the old one".into()
    });
    t.eval(r#"store.set("big", nil)"#);
    t.e(&["uninstall", "gamma"]);
    let out = t.e(&["put", &t.path("alpha/main.lua"), "--app", "Alpha"]);
    let ok = out.contains("there is an app 'alpha' already") && !grep("^Alpha ", &t.e(&["apps"]));
    t.check(ok, "an app name differing only in letter case is refused", |_| format!("Alpha: {out}"));
    t.e(&["run", "alpha"]);
    t.eval(
        r#"store.set("n", 42) store.set("f", 1.5) store.set("s", "hi") store.set("b", false) store.set("gone", 1) store.set("gone", nil)"#,
    );
    let q =
        r#"math.type(store.get("n")), store.get("f"), store.get("s"), store.get("b"), table.concat(store.keys(), ",")"#;
    let ok = t.eval(q) == "integer\t1.5\thi\tfalse\tb,f,n,s";
    t.check(ok, "store keeps strings, integers, floats and booleans", |t| format!("store: {}", t.eval(q)));
    let ok = t.eval(r#"store.set("t", {})"#).contains("json.encode")
        && t.eval(r#"store.set("bad key", 1)"#).contains("keys are 1..32")
        && t.eval(r#"store.set("big", ("x"):rep(70000))"#).contains("store full");
    t.check(ok, "store refuses tables, bad keys and too much", |t| {
        format!("store limits: {}", t.eval(r#"store.set("big", ("x"):rep(70000))"#))
    });
    t.e(&["run", "beta"]);
    t.eval(r#"store.set("s", "beta")"#);
    let ok = t.eval(r#"store.get("n"), store.get("s")"#) == "nil\tbeta";
    t.check(ok, "each app has its own store", |t| format!("beta sees: {}", t.eval(r#"store.get("n")"#)));
    t.stop();
    t.boot();
    t.e(&["run", "alpha"]);
    let v = t.eval(r#"store.get("s")"#);
    t.check(v == "hi", "store survives a restart", |_| format!("after restart: {v}"));
    t.eval(r#"for i = 1, 100 do store.set("c", i) end"#);
    let v = t.eval(r#"store.get("c")"#);
    t.check(v == "100", "rapid store.set calls read back the newest value", |_| format!("rapid sets: {v}"));
    t.advance(1100);
    t.stop();
    t.boot();
    t.e(&["run", "alpha"]);
    let v = t.eval(r#"store.get("c")"#);
    t.check(v == "100", "... and the newest value reaches flash a second later", |_| format!("after restart: {v}"));
    t.eval(r#"store.set("c", nil)"#);
    t.e(&["secrets", "set", "beta", "x", "1"]);
    t.e(&["uninstall", "beta"]);
    let data = t.work.join("nvs.bin.flash/.data");
    let ok = !grep("^beta ", &t.e(&["secrets"])) && !data.join("beta").exists() && data.join("alpha").exists();
    t.check(ok, "uninstall deletes the app's secrets and stored values", |t| {
        format!("uninstall left: {} {}", t.e(&["secrets"]), ls_a(&data))
    });
    let out = t.eval(r#"for i = 1, 4 do secrets.set("q" .. i, ("x"):rep(2048)) end"#);
    let ok = out.contains("no room") && t.eval("#secrets.keys()") == "5";
    t.check(ok, "an app's secrets can't take more than 8 KB of the device's settings flash", |t| {
        format!("secrets quota: {out} / {}", t.eval("#secrets.keys()"))
    });
    t.eval(r#"for i = 1, 4 do secrets.set("q" .. i, nil) end"#);
    t.e(&["secrets", "set", "gamma", "t", "1"]); // gamma: no app at all
    let ok = t.e(&["uninstall", "gamma"]).contains("UNINSTALLED gamma") && !grep("^gamma ", &t.e(&["secrets"]));
    t.check(ok, "uninstall deletes the secrets of a name without an app", |t| {
        format!("uninstall gamma: {}", t.e(&["secrets"]))
    });
    t.e(&["run", "alpha"]);
    t.eval("sys.exit()"); // the menu, alpha selected
    t.wait_lua(2.0, MENU_PX);
    t.press("A"); // the focus
    t.press_for("B", 1000); // alpha's options: Open, Autostart, Clear data, Uninstall, Back
    t.press("C");
    t.press("C");
    t.press("B");
    let ok = grep("^alpha ", &t.e(&["secrets"]));
    t.check(ok, "Clear data waits for the confirm", |_| "cleared without a confirm".into());
    t.press("B");
    let ok = !grep("^alpha ", &t.e(&["secrets"]))
        && !data.join("alpha").exists()
        && grep("^alpha .*installed", &t.e(&["apps"]));
    t.check(ok, "Clear data in the app options deletes its secrets and stored values, keeps the app", |t| {
        format!("clear data: {} / {}", t.e(&["secrets"]).replace('\n', " "), t.e(&["apps"]).replace('\n', " "))
    });
    t.write(
        "alpha/app.json",
        format!(
            "{{\"title\":\"Alpha\",\"description\":\"{}\",\"version\":\"1.0\",\"author\":\"Tester\"}}\n",
            "A long description that takes up two whole lines of the options page, and then some more words"
        ),
    );
    t.e(&["install", &alpha]);
    t.advance(200); // options of an app with all its info: nothing runs into the footer
    let shot: Ppm = t.shot("opt.ppm");
    let lit = shot.count(0..shot.w, 209..218, |r, g, b| r.max(g).max(b) > 120);
    t.check(lit == 0, "app options: everything fits above the footer", |_| {
        format!("app options run into the footer ({lit} px)")
    });
    // the app options page with this title and description
    fn options(t: &mut T, alpha: &str, title: &str, desc: &str) -> Ppm {
        t.write("alpha/app.json", format!("{{\"title\":\"{title}\",\"description\":\"{desc}\"}}\n"));
        t.e(&["install", alpha]);
        t.advance(200);
        t.shot("opt.ppm")
    }
    let plain = options(t, &alpha, "Oljy", "x").px;
    let nordic = options(t, &alpha, "Öljy", "x").px;
    let escaped = options(t, &alpha, "\\u00d6ljy", "x").px;
    t.check(
        plain != nordic && escaped == nordic,
        "app.json title keeps its Nordic letters, also as \\u escapes",
        |_| "Öljy shown as Oljy".into(),
    );
    // the lowest lit row above the footer: as many description lines for ä as for a
    let lowest = |p: &Ppm| (0..209).rev().find(|&y| p.count(0..p.w, y..y + 1, |r, g, b| r.max(g).max(b) > 120) > 0);
    let a = lowest(&options(t, &alpha, "Alpha", &"a".repeat(45)));
    let ae = lowest(&options(t, &alpha, "Alpha", &"ä".repeat(45)));
    t.check(a == ae, "app options: a UTF-8 description wraps by characters", |_| format!("lowest row {a:?} / {ae:?}"));
    t.e(&["secrets", "set", "alpha", "a", "1"]);
    t.e(&["secrets", "set", "alpha", "b", "2"]);
    t.e(&["secrets", "rm", "alpha"]);
    let ok = !grep("^alpha ", &t.e(&["secrets"]));
    t.check(ok, "secrets rm APP deletes all of an app's secrets", |t| format!("rm alpha: {}", t.e(&["secrets"])));
    t.stop();
    t.wipe(); // secrets stored by older firmware: no app, so dropped
    let mut nvs = nvs_rec(b"secrets", b"_index", b"oldtoken\n");
    nvs.extend(nvs_rec(b"secrets", b"oldtoken", b"x"));
    t.write("nvs.bin", nvs);
    t.boot();
    let ok = grep("^0 secret", &t.e(&["secrets"]));
    t.check(ok, "secrets from older firmware are dropped", |t| format!("old secrets: {}", t.e(&["secrets"])));
    t.stop();
}
