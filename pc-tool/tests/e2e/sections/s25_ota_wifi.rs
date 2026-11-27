//! A firmware update over Wi-Fi, and one over USB that stops halfway.

use std::fs;

use super::ota::{cp, info, sha256, Raw, REBOOTS};
use super::s06_wifi::monitor;
use crate::harness::{grep, EXE, T};

pub fn run(t: &mut T) {
    t.say("== firmware update (OTA) over Wi-Fi: the reboot drops the link, the PC reconnects");
    t.wipe();
    t.boot();
    let new_sim = t.work.join(format!("new-sim{EXE}"));
    let new_s = new_sim.to_string_lossy().into_owned();
    cp(&t.sim_bin, &new_sim);
    let img = fs::read(&new_sim).unwrap();
    let size = img.len();
    t.write("main.lua", "function loop() end\n");
    t.e(&["wifi", "set", "Home Net", "pass"]);
    let wport = t.wifi_port(); // the port the simulator's Wi-Fi server took (kept through reboots)
    let tcp = format!("tcp://127.0.0.1:{wport}");
    let wi = |t: &T, args: &[&str]| {
        let mut all = vec!["-p", tcp.as_str(), "--linger", "0"];
        all.extend_from_slice(args);
        t.cli(&all)
    };
    t.e(&["pair"]);
    t.wifi_up();
    if !t.until_sim(5.0, |t| wi(t, &["eval", "1+1"]) == "2") {
        let v = wi(t, &["eval", "1+1"]);
        t.bad(&format!("no Wi-Fi link: {v}"));
    }

    // An update over USB that stops halfway, with a Wi-Fi client meanwhile.
    t.e(&["put", &t.path("main.lua"), "--app", "keep", "--save"]);
    t.e(&["run", "keep"]);
    let sha = sha256(&img);
    // The serial link held open through it all (luaboard raw); the steps
    // watch its replies. What they find goes to `r`, not to the device.
    let mut r = String::new();
    let mut raw = Raw::start(t);
    raw.send(format!("\nOTA {size} {sha}\n").as_bytes());
    if t.until(2.0, |_| raw.has("OK READY")) {
        r.push_str("ready: True\n");
    }
    raw.send(&img[..65536.min(size)]);
    let v = wi(t, &["run", "keep"]).replace('\n', " ");
    r.push_str(&format!("wifi run: {v} \n"));
    if wi(t, &["info"]).contains("slot:") {
        r.push_str("wifi info: True\n");
    }
    // A Wi-Fi client that is logged in stays so while the update runs: a newer login
    // doesn't replace it (it may be the one sending the update), it is turned away.
    let _first = monitor(t, &tcp, "first.txt");
    t.until(2.0, |t| grep("^monitoring", &t.read("first.txt")));
    let late = wi(t, &["info"]).replace('\n', " ");
    // The image stops for 3 s of the device's time (just: the quiet after
    // counts too). The rest of it comes anyway, a PING inside each piece, a
    // tenth of a second apart. All of it goes in one batch of control lines,
    // so the device's clock stands between the lines: on a slow computer,
    // bytes from another process could come later than the quiet the device
    // drops input until, and be taken as commands as they should then.
    let before = raw.pongs();
    let mut lines = vec!["advance=3050".to_string()];
    for i in 0..10 {
        let from = 65536 + i * 40;
        let mut piece = img[from..from + 40].to_vec();
        piece.extend_from_slice(b"\nPING\n");
        lines.push(format!("link={}", piece.iter().map(|b| format!("{b:02x}")).collect::<String>()));
        lines.push("advance=100".to_string());
    }
    t.ctl(&lines.iter().map(String::as_str).collect::<Vec<_>>());
    if t.until(3.0, |_| raw.has("ERR upload timeout")) {
        r.push_str("timeout: True\n");
    }
    t.advance(800);
    // A PING after the quiet, and HELP to mark the end: the device reads its
    // link in order, so once HELP is answered it has answered all it was
    // going to of the image's PINGs. Exactly one PONG in all: none of those
    // was answered, also not late.
    raw.send(b"\nPING\nHELP\n");
    if t.until(2.0, |_| raw.has("see docs/PROTOCOL.md")) && raw.pongs() > before {
        r.push_str("pong after: True\n");
    }
    let streamed = raw.pongs().saturating_sub(before + 1);
    r.push_str(&format!("pongs while it streams: {streamed}\n"));
    let _ = raw.finish();
    if !(r.contains("ready: True") && r.contains("timeout: True")) {
        t.bad(&format!("stalled update: {r}"));
    }
    let ok = grep("wifi run: .*a firmware update is running", &r) && r.contains("wifi info: True");
    t.check(ok, "during an update another link may look but not run apps", |_| {
        format!("other link during update: {r}")
    });
    let kept = t.until(2.0, |t| t.read("first.txt").contains("ota failed")) && !t.read("first.txt").contains("bye");
    let ok = late.contains("a firmware update is running") && !late.contains("slot:") && kept;
    t.check(ok, "during an update a newer Wi-Fi login is turned away, and the client that was there stays", |t| {
        format!("the newer login: {late} / the first client: {}", t.read("first.txt"))
    });
    let ok = r.contains("pongs while it streams: 0") && r.contains("pong after: True");
    t.check(ok, "after an upload timeout the rest of the image isn't run as commands", |_| {
        format!("after timeout: {r}")
    });
    let ok = t.until(3.0, |t| info(t, "running") == "keep/main.lua") && info(t, "slot") == "ota_0";
    t.check(ok, "a failed update starts the app it stopped again", |t| {
        format!("after failed update: {}", t.e(&["info"]))
    });
    let out = t.cli_ahead(&["-p", tcp.as_str(), "--linger", "0", "update", &new_s], REBOOTS);
    t.check(grep("done: running .* on ota_1", &out), "an update over Wi-Fi comes back on the new firmware", |_| {
        format!("Wi-Fi update: {out}")
    });
    let ok = info(t, "slot") == "ota_1" && wi(t, &["eval", "2+2"]) == "4";
    t.check(ok, "and it takes Wi-Fi commands again", |t| format!("after Wi-Fi update: {}", t.e(&["info"])));
    t.stop();
}
