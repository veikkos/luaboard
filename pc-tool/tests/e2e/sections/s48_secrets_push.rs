//! `luaboard secrets push`: every app folder's secrets.env onto the device,
//! for the apps it has; the others are skipped.

use crate::harness::T;

fn secrets(t: &T) -> String {
    t.e(&["secrets"])
}

fn has(list: &str, app: &str, name: &str) -> bool {
    list.lines().any(|l| {
        let mut w = l.split_whitespace();
        w.next() == Some(app) && w.next() == Some(name)
    })
}

pub fn run(t: &mut T) {
    t.say("== luaboard secrets push");
    // apps/one: installed, with secrets.env; apps/two: not installed;
    // apps/three: installed, no secrets.env; apps/tools: not an app.
    t.write("apps/one/main.lua", "-- one");
    t.write("apps/one/secrets.env", "key=k1\nsecret = s1\n");
    t.write("apps/two/main.lua", "-- two");
    t.write("apps/two/secrets.env", "key=k2\n");
    t.write("apps/three/main.lua", "-- three");
    t.write("apps/tools/build.py", "");
    t.wipe();
    t.boot();
    t.e(&["install", &t.path("apps/three")]);
    t.write("plain/one/main.lua", "-- one"); // installed without its secrets.env
    t.e(&["install", &t.path("plain/one")]);
    t.e(&["secrets", "set", "one", "token", "login"]); // a login it has: stays

    let (ok, out) = t.cli_status(&[], &["-p", &t.tty, "secrets", "push", &t.path("apps")]);
    t.check(ok, "secrets push succeeds", |_| out.clone());
    t.check(out.contains("one: 2 secrets set (key, secret)"), "an installed app gets its secrets.env", |_| out.clone());
    t.check(out.contains("two: not installed, skipped"), "an app that isn't installed is skipped", |_| out.clone());
    t.check(!out.contains("three") && !out.contains("tools"), "no secrets.env, or not an app: nothing said", |_| {
        out.clone()
    });
    t.check(!out.contains("k1") && !out.contains("s1"), "values are never printed", |_| out.clone());
    let list = secrets(t);
    let ok = has(&list, "one", "key")
        && has(&list, "one", "secret")
        && has(&list, "one", "token")
        && !has(&list, "two", "key");
    t.check(ok, "the device has them, and keeps the login", |_| list.clone());

    // One app's folder works too; a broken secrets.env is reported, the rest go on.
    t.write("apps/one/secrets.env", "key=k3\n");
    let (ok, out) = t.cli_status(&[], &["-p", &t.tty, "secrets", "push", &t.path("apps/one")]);
    t.check(ok && out.contains("one: 1 secret set (key)"), "one app's folder", |_| out.clone());
    t.e(&["install", &t.path("apps/two")]);
    t.write("apps/two/secrets.env", "no equals sign\n");
    let (ok, out) = t.cli_status(&[], &["-p", &t.tty, "secrets", "push", &t.path("apps")]);
    t.check(
        !ok && out.contains("two:") && out.contains("one: 1 secret set"),
        "a broken file fails, the others still go",
        |_| out.clone(),
    );
    t.stop();
}
