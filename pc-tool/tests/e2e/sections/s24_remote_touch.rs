//! Remote touch (TOUCH, KEY) and the frame buffer only when it changed (FB <crc>).

use crate::harness::{Ppm, T};

const EV_LUA: &str = r#"ev = {}
for _, k in ipairs({ "press", "long", "tap", "hold", "swipe" }) do
  touch.on(k, function(a, b, c)
    ev[#ev + 1] = k .. ":" .. tostring(a) .. (b and "," .. b or "") .. (c and "," .. c or "")
  end)
end
function evs() local s = table.concat(ev, " ") ev = {} return s end
function loop() disp.clear(0) end
"#;

fn first_line(s: &str) -> &str {
    s.lines().next().unwrap_or("")
}

pub fn run(t: &mut T) {
    t.say("== remote touch (TOUCH, KEY, FB <crc>)");
    t.wipe();
    t.boot();

    t.write("ev.lua", EV_LUA);
    t.e(&["put", &t.path("ev.lua"), "--app", "ev", "--run", "ev.lua"]);

    t.e(&["touch", "50", "60"]);
    t.advance(100);
    let v = t.eval("evs()");
    t.check(v == "tap:50,60", "touch X Y taps the screen", |_| format!("remote tap: {v}"));
    t.e(&["touch", "B", "--hold", "0"]);
    t.advance(100);
    let v = t.eval("evs()");
    t.check(v == "press:B", "touch B presses the B button", |_| format!("remote B: {v}"));
    // C held for 1.6 s of the device's time, said again every 0.4 s as
    // `luaboard touch --hold` does (every 0.3 s, in real time).
    for _ in 0..4 {
        t.raw(&["TOUCH 260 260"]);
        t.advance(400);
    }
    t.raw(&["TOUCH UP"]);
    t.advance(100);
    let v = t.eval("evs()");
    t.check(v == "long:C", "a remote finger held past the 1 s timeout stays down while repeated", |_| {
        format!("remote long C: {v}")
    });
    let r = t.raw(&["TOUCH 160 180", "TOUCH 160 140", "TOUCH 160 100", "TOUCH UP"]);
    t.advance(200);
    let v = t.eval("evs()");
    t.check(v == "swipe:up,160,180", "a swipe that arrives in one go is still a swipe", |_| {
        format!("batched swipe: {r} / {v}")
    });
    t.raw(&["TOUCH 30 40", "TOUCH UP"]);
    t.advance(200);
    let v = t.eval("evs()");
    t.check(v == "tap:30,40", "a tap whose down and up arrive together is still a tap", |_| {
        format!("batched tap: {v}")
    });
    t.raw(&["TOUCH 100 100"]);
    t.advance(1500);
    let r = t.eval("evs(), touch.read()");
    t.check(r == "hold:100,100\tnil", "a remote finger nobody repeats is let go after 1 s", |_| {
        format!("stuck finger: {r}")
    });
    let r = t.raw(&[
        "TOUCH 320 0",
        "TOUCH -1 5",
        "TOUCH 5",
        "TOUCH 1 2 3",
        "TOUCH 10x 5",
        "TOUCH UP 1",
        "KEY X",
        "KEY POWER 1",
    ]);
    let n = r.lines().filter(|l| l.starts_with("ERR usage")).count();
    t.check(n == 8, "bad TOUCH / KEY arguments are refused", |_| format!("bad args: {r}"));
    t.raw(&["TOUCH 0 279", "TOUCH UP"]);
    t.advance(200);
    let v = t.eval("evs()");
    t.check(v == "press:A", "the strip's corner is in the touch area", |_| format!("corner: {v}"));

    let fb = t.raw(&["FB 00000000"]);
    t.check(first_line(&fb).starts_with("OK 320 240 rgb565 153600 "), "FB <other crc>: the frame", |_| {
        format!("FB other crc: {}", first_line(&fb))
    });
    let crc = first_line(&fb).split_whitespace().nth(5).unwrap_or("").to_string();
    let r = t.raw(&[&format!("FB {crc}")]);
    t.check(first_line(&r) == "OK SAME", "FB <crc>: unchanged frame isn't sent again", |t| {
        format!("FB same: {}", t.raw(&[&format!("FB {crc}")]))
    });
    // The CLI's screenshot (a plain FB) shows what is on the LCD.
    let cli = t.path("cli.ppm");
    let out = t.e(&["screenshot", &cli]);
    let lcd = t.shot("lcd.ppm");
    let same = Ppm::load(std::path::Path::new(&cli)).same_as(&lcd);
    t.check(out.contains("(320x240)") && same, "luaboard screenshot: the frame as the LCD shows it", |_| {
        format!("screenshot: {out}, same as the LCD: {same}")
    });

    t.e(&["touch", "power"]);
    t.advance(300);
    let info = t.e(&["info"]);
    t.check(info.contains("running: -"), "touch power: the power key goes home", |_| {
        let l: Vec<&str> = info.lines().filter(|l| l.contains("running")).collect();
        format!("power key: {}", l.join("\n"))
    });

    // Over Wi-Fi: the same, but only for a paired, logged-in client.
    t.e(&["wifi", "set", "Home Net", "pw"]);
    let wport = t.wifi_port(); // the port the simulator's Wi-Fi server took
    let w = format!("tcp://127.0.0.1:{wport}");
    let wi = |t: &T, args: &[&str]| {
        let mut all = vec!["-p", &w, "--linger", "0"];
        all.extend_from_slice(args);
        t.cli(&all)
    };
    t.wifi_up();
    if !t.until_sim(3.0, |t| wi(t, &["ping"]).contains("not paired")) {
        t.bad("no Wi-Fi link");
    }
    let ok = wi(t, &["touch", "10", "10"]).contains("not paired");
    t.check(ok, "not paired: no remote touch over Wi-Fi", |t| {
        format!("unpaired touch: {}", wi(t, &["touch", "10", "10"]))
    });
    t.e(&["pair"]);
    t.e(&["put", &t.path("ev.lua"), "--app", "ev", "--run", "ev.lua"]);
    wi(t, &["touch", "70", "80", "--hold", "0"]);
    t.advance(100);
    let v = t.eval("evs()");
    t.check(v == "tap:70,80", "a paired computer touches over Wi-Fi", |_| format!("Wi-Fi tap: {v}"));
    t.stop();
}
