//! oauth.lua (built into the firmware): the browser login (authorization code
//! and PKCE) through `luaboard browser` against a fake provider, the refresh
//! token kept as a secret, a reboot that refreshes without a new login, and a
//! revoked login that asks again.

use crate::fakes::oauth::FakeOauth;
use crate::harness::{free_port, grep, T};

pub fn run(t: &mut T) {
    t.say("== oauth.lua: browser login (OAuth + PKCE)");
    let rp = free_port();
    t.wipe();
    t.boot();
    let web = FakeOauth::start(Vec::new(), None);
    let (wp, log) = (web.port, web.log.clone());
    let main = format!(
        r#"local oauth = require('oauth')
login = oauth.new{{ name = 'acct', title = 'Account', client_id = 'test-client', scope = 'me',
  authorize_url = 'http://127.0.0.1:{wp}/authorize', token_url = 'http://127.0.0.1:{wp}/api/token',
  redirect = 'http://127.0.0.1:{rp}/callback', on_login = function(w) WAITING = w end }}
task(function()
  while true do
    LAST = login:request{{ url = 'http://127.0.0.1:{wp}/api/me' }}
    sleep(2000)
  end
end)
"#
    );
    t.write("main.lua", main);
    t.e(&["put", &t.path("main.lua"), "--run", "main.lua"]);
    let ok = t.wait_lua(3.0, "return WAITING == true");
    t.check(ok, "the script is told a login is needed", |_| "on_login(true) not called".into());
    let out = t.browse();
    t.check(out.contains("Login handed to the device"), "luaboard browser completes the login", |_| {
        format!("browser login failed: {out}")
    });
    let ok = t.until(3.0, |_| log.has("API ok"));
    let ok = ok && log.has("AUTHORIZE test-client me S256") && log.has("TOKEN code");
    t.check(ok, "code exchanged with the PKCE verifier, the API called with the token", |_| log.tail(4));
    let wapp = regex::escape(&t.wapp);
    let ok = grep(&format!("^{wapp}  *acct "), &t.e(&["secrets"]));
    t.check(ok, "the refresh token is kept as the secret 'acct'", |_| "no acct secret".into());

    t.crash();
    let logins = log.count("AUTHORIZE");
    t.boot();
    t.e(&["put", &t.path("main.lua"), "--run", "main.lua"]);
    let ok = t.until(5.0, |_| log.grep("TOKEN refresh$")) && log.count("AUTHORIZE") == logins;
    t.check(ok, "after a reboot: a refresh, no new login", |_| log.tail(4));

    web.revoke(); // the user removed the app's access
    t.advance(2000); // the script's next request finds out
    let out = t.browse(); // waits until the device asks again
    let ok = out.contains("Login handed to the device") && log.has("TOKEN refresh rejected");
    t.check(ok, "a revoked login asks again and logs in again", |_| format!("re-login: {out}"));
    t.crash();
}
