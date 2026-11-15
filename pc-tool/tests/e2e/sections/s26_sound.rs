//! sound: tones and WAV clips on the speaker, recorded by the simulator
//! (--wav) as its amplifier plays them, then measured: how long each
//! sound lasted and its pitch. And assets: a picture and a sound installed
//! with an app, read with asset().

use std::fs;

use crate::harness::T;

/// A WAV file made in Lua: `ms` of a square wave of `hz`, 8 kHz mono 16-bit.
const WAV_LUA: &str = r#"local function wav(hz, ms)
  local rate, n = 8000, 8 * ms
  local s = {}
  for i = 0, n - 1 do s[#s + 1] = string.pack("<i2", (i * hz * 2 // rate) % 2 == 0 and 12000 or -12000) end
  local data = table.concat(s)
  return "RIFF" .. string.pack("<I4", 36 + #data) .. "WAVEfmt " .. string.pack("<I4I2I2I4I4I2I2", 16, 1, 1, rate, rate * 2, 2, 16)
    .. "data" .. string.pack("<I4", #data) .. data
end
return wav
"#;

/// A WAV file: `ms` of a square wave of `hz`, 8 kHz mono 16-bit.
fn wav_file(hz: u32, ms: u32) -> Vec<u8> {
    let (rate, n) = (8000u32, 8 * ms);
    let data: Vec<u8> = (0..n)
        .flat_map(|i| (if (i * hz * 2 / rate).is_multiple_of(2) { 12000i16 } else { -12000 }).to_le_bytes())
        .collect();
    let mut w = b"RIFF".to_vec();
    w.extend((36 + data.len() as u32).to_le_bytes());
    w.extend(b"WAVEfmt ");
    for v in [16u32, 1 | 1 << 16, rate, rate * 2, 2 | 16 << 16] {
        w.extend(v.to_le_bytes());
    }
    w.extend(b"data");
    w.extend((data.len() as u32).to_le_bytes());
    w.extend(data);
    w
}

/// The left channel of a 16-bit stereo WAV the simulator wrote, and its rate.
fn samples(path: &str) -> (Vec<i16>, u32) {
    let d = fs::read(path).unwrap_or_default();
    if d.len() < 44 {
        return (vec![], 0);
    }
    let rate = u32::from_le_bytes([d[24], d[25], d[26], d[27]]);
    let left = d[44..].as_chunks::<4>().0.iter().map(|f| i16::from_le_bytes([f[0], f[1]])).collect();
    (left, rate)
}

/// The sounds in it: (seconds, Hz from zero crossings), apart where it's
/// silent for more than 20 ms.
fn sounds(s: &[i16], rate: u32) -> Vec<(f64, f64)> {
    let gap = rate as usize / 50;
    let mut out = vec![];
    let mut i = 0;
    while i < s.len() {
        if s[i].abs() < 100 {
            i += 1;
            continue;
        }
        let (start, mut end, mut quiet) = (i, i, 0);
        while i < s.len() && quiet < gap {
            if s[i].abs() >= 100 {
                end = i;
                quiet = 0;
            } else {
                quiet += 1;
            }
            i += 1;
        }
        let seg = &s[start..=end];
        let crossings = seg.windows(2).filter(|w| (w[0] < 0) != (w[1] < 0)).count();
        let secs = seg.len() as f64 / rate as f64;
        out.push((secs, crossings as f64 / 2.0 / secs));
    }
    out
}

fn near(v: f64, want: f64, tol: f64) -> bool {
    (v - want).abs() <= tol
}

pub fn run(t: &mut T) {
    t.say("== sound: tones and WAV clips on the speaker");
    t.wipe();
    let wav = t.path("speaker.wav");
    t.boot_with(&["--wav", &wav]);
    t.write("snd/main.lua", "function loop() end\n");
    t.write("snd/wav.lua", WAV_LUA);
    t.e(&["install", &t.path("snd"), "--run"]);

    let v = t.eval("sound.tone(1000, 200)");
    t.check(v == "true", "a tone is queued", |_| format!("tone: {v}"));
    let v = t.eval("sound.playing()");
    t.check(v == "true", "... and playing", |_| format!("playing: {v}"));
    let ok = t.until_sim(3.0, |t| t.eval("sound.playing()") == "false");
    t.check(ok, "... and done after it", |_| "a 200 ms tone still playing".into());

    let v = t.eval(r#"sound.play(require("wav")(500, 250))"#);
    t.check(v == "true", "a WAV clip plays", |_| format!("play: {v}"));
    t.until_sim(3.0, |t| t.eval("sound.playing()") == "false");
    let v = t.eval(r#"sound.play("not a sound")"#);
    t.check(v.contains("nil") && v.contains("not a WAV file"), "a bad WAV is an error value", |_| format!("bad: {v}"));
    let v = t.eval("sound.tone(5, 100)");
    t.check(v.contains("20..20000"), "a frequency out of range is an error", |_| format!("5 Hz: {v}"));

    t.eval("sound.tone(700, 5000)");
    t.advance(300);
    t.eval("sound.stop()");
    let v = t.eval("sound.playing()");
    t.check(v == "false", "sound.stop ends it at once", |_| format!("after stop: {v}"));

    t.write("art/main.lua", "function loop() end\n");
    t.write("art/ding.wav", wav_file(600, 150));
    t.write("art/cover.jpg", fs::read(t.root.join("pc-tool/tests/e2e/data/cover.jpg")).unwrap());
    t.write("art/notes.txt", "not an asset");
    t.e(&["install", &t.path("art"), "--run"]);
    let ls = t.e(&["ls"]);
    let ok = ls.contains("art/ding.wav") && ls.contains("art/cover.jpg") && !ls.contains("art/notes.txt");
    t.check(ok, "install takes the app's pictures and sounds along, not other files", |_| format!("ls: {ls}"));
    let v = t.eval(r#"disp.jpeg_size(asset("cover.jpg"))"#);
    t.check(v.contains('\t'), "asset() reads a picture for disp.jpeg", |_| format!("jpeg_size: {v}"));
    let v = t.eval(r#"sound.play(asset("ding.wav"))"#);
    t.check(v == "true", "... and a sound for sound.play", |_| format!("play asset: {v}"));
    t.until_sim(3.0, |t| t.eval("sound.playing()") == "false");
    let v = t.eval(r#"asset("missing.wav")"#);
    t.check(v.contains("nil") && v.contains("no such file"), "a missing asset is nil and why", |_| {
        format!("missing: {v}")
    });
    let v = t.eval(r#"asset("../snd/main.lua")"#);
    t.check(v.contains("without '/'"), "only the app's own files", |_| format!("other app: {v}"));

    t.write("long.lua", "sound.tone(440, 5000)\nfunction loop() end\n");
    t.e(&["put", &t.path("long.lua"), "--app", "snd", "--run", "long.lua"]);
    t.advance(300);
    t.e(&["stop"]);
    let v = t.eval("sound.playing()");
    t.check(v == "false", "a sound ends with its script", |_| format!("after STOP: {v}"));
    t.advance(3000); // the amplifier goes off
    t.stop();

    let (s, rate) = samples(&wav);
    let got = sounds(&s, rate);
    let show = || got.iter().map(|(d, f)| format!("{d:.2} s {f:.0} Hz")).collect::<Vec<_>>().join(", ");
    t.check(got.len() == 5, "the speaker played five sounds", |_| format!("sounds: {}", show()));
    let ok = got.first().is_some_and(|&(d, f)| near(d, 0.2, 0.02) && near(f, 1000.0, 30.0));
    t.check(ok, "the tone: 200 ms at 1000 Hz", |_| format!("sounds: {}", show()));
    let ok = got.get(1).is_some_and(|&(d, f)| near(d, 0.25, 0.02) && near(f, 500.0, 30.0));
    t.check(ok, "the clip: 250 ms at 500 Hz, resampled from 8 kHz", |_| format!("sounds: {}", show()));
    let ok = got.get(3).is_some_and(|&(d, f)| near(d, 0.15, 0.02) && near(f, 600.0, 30.0));
    t.check(ok, "the asset: 150 ms at 600 Hz", |_| format!("sounds: {}", show()));
    // (5 s sounds stopped well before their end; how soon depends on the host)
    let ok = got.get(2).is_some_and(|&(d, _)| d < 4.0) && got.get(4).is_some_and(|&(d, _)| d < 4.0);
    t.check(ok, "stopped sounds end early", |_| format!("sounds: {}", show()));
    let peak = s.iter().map(|v| v.unsigned_abs()).max().unwrap_or(0);
    let ok = (2000..=3100).contains(&peak);
    t.check(ok, "at the default volume (50 %: a quarter of the level)", |_| format!("peak: {peak}"));
}
