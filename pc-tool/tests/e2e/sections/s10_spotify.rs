//! Browser login (OAuth + PKCE) and the spotify example against a fake Spotify.

use crate::fakes::oauth::FakeOauth;
use crate::harness::{free_port, grep, T};

/// `sed 's|^FROM|TO|'`: FROM at the start of a line becomes TO.
fn replace_line_start(text: &str, from: &str, to: &str) -> String {
    text.split_inclusive('\n')
        .map(|l| match l.strip_prefix(from) {
            Some(rest) => format!("{to}{rest}"),
            None => l.to_string(),
        })
        .collect()
}

pub fn run(t: &mut T) {
    t.say("== browser login (OAuth + PKCE) and spotify against a fake Spotify");
    let rp = free_port();
    t.wipe();
    t.boot();
    let cover = std::fs::read(t.root.join("pc-tool/tests/e2e/data/cover.jpg")).unwrap(); // cover: 300x300, 0x3366CC
    let web = FakeOauth::start(cover, None);
    let (wp, log) = (web.port, web.log.clone());
    t.write("secrets.env", "# test\nspotify_client = test-client\n"); // put sends it as a secret
    let main = std::fs::read_to_string(t.root.join("examples/spotify/main.lua")).unwrap();
    let main = replace_line_start(
        &main,
        "sp = spotify.new{",
        &format!(
            r#"sp = spotify.new{{ api_url = "http://127.0.0.1:{wp}/v1", accounts_url = "http://127.0.0.1:{wp}", redirect = "http://127.0.0.1:{rp}/callback","#
        ),
    );
    t.write("main.lua", main);
    let lib = t.root.join("examples/spotify/spotify.lua").to_string_lossy().into_owned();
    t.e(&["put", &t.path("main.lua"), &lib, "--run", "main.lua"]);
    let login_px = "local n = 0 for x = 16, 200 do if disp.get(x, 36) == 0xFFFFFF then n = n + 1 end end return n > 20";
    let ok = t.wait_lua(3.0, login_px);
    t.check(ok, "device asks for a login on its screen", |_| "no login screen".into());
    let out = t.browse();
    let ok = out.contains("Login handed to the device");
    t.check(ok, "luaboard browser completes the login", |_| format!("browser login failed: {out}"));
    t.until(4.0, |_| log.has("COVER /cover/0-300.jpg"));
    t.check(log.has("TOKEN code"), "code exchanged with PKCE verifier", |_| {
        format!("no token exchange: {}", log.tail(3))
    });
    t.check(log.has("COVER /cover/0-300.jpg"), "player state and 300 px cover fetched", |_| "no cover fetched".into());
    let wapp = regex::escape(&t.wapp);
    let list = t.e(&["secrets"]);
    let n = list.lines().filter(|l| grep(&format!("^{wapp}  *spotify "), l)).count();
    t.check(n == 1, "refresh token kept as secret 'spotify'", |_| "no spotify secret".into());
    let ok = grep(&format!("^{wapp}  *spotify_client "), &t.e(&["secrets"]));
    t.check(ok, "secrets.env stored as secret 'spotify_client'", |_| "secrets.env not stored".into());
    t.eval("real_read = real_read or touch.read; touch.read = function() return 270, 210 end");
    t.advance(300);
    t.eval("touch.read = real_read");
    let ok = t.until(3.0, |_| log.has("SPOTIFY next"));
    t.check(ok, "next button calls the Web API", |_| "next not sent".into());
    t.crash();
    let logins = log.count("AUTHORIZE");
    t.boot(); // reboot: the login must survive
    t.e(&["put", &t.path("main.lua"), &lib, "--run", "main.lua"]);
    t.until(5.0, |_| log.grep("TOKEN refresh$"));
    let ok = log.count("AUTHORIZE") == logins && log.grep("TOKEN refresh$");
    t.check(ok, "after a reboot: refresh, no new login", |_| "login not kept across reboot".into());
    web.revoke(); // user removed the app's access
    t.advance(2000); // the app's next refresh (every 2 s) finds out
    let out = t.browse(); // waits until the device asks again
    let ok = out.contains("Login handed to the device") && log.has("TOKEN refresh rejected");
    t.check(ok, "revoked login: asks again and logs in again", |_| format!("re-login: {out}"));
    t.crash();
}
