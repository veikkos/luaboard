//! disp.png and disp.png_size against PNGs made by the test (every colour
//! type, bit depths, filters, alpha, scaling, broken files), and disp.grab.
//! The device fetches them from a small server here.

use std::collections::HashMap;
use std::sync::Arc;

use crate::fakes::http::{self, Request, Response};
use crate::harness::T;
use crate::pngs::{encode, rgba, Png};

const SCRIPT: &str = r#"local http = require("http")
P = {}
NAMES = { $NAMES }
function near(x, y, r, g, b)
  local c = disp.get(x, y)
  return math.abs((c >> 16 & 255) - r) <= 8 and math.abs((c >> 8 & 255) - g) <= 8 and math.abs((c & 255) - b) <= 8
end
task(function()
  for _, n in ipairs(NAMES) do
    local st, body = http.get("http://127.0.0.1:$PORT/" .. n)
    P[n] = st == 200 and body or ("HTTP " .. tostring(st) .. " " .. tostring(body))
  end
  DONE = true
end)
"#;

fn files() -> HashMap<String, Vec<u8>> {
    let mut f = HashMap::new();
    let rows = vec![vec![255, 0, 0, 0, 255, 0, 0, 0, 255], vec![255, 255, 0, 0, 255, 255, 128, 64, 32]];
    f.insert(
        "rgb".into(),
        encode(&Png { w: 3, h: 2, color_type: 2, depth: 8, rows: &rows, extra: &[], filters: &[0], interlace: false }),
    );
    f.insert("blend".into(), rgba(2, 1, |x, _| if x == 0 { [255, 0, 0, 128] } else { [9, 9, 9, 0] }));
    // palette, 4 bits a pixel: index 0 opaque, 1 transparent, 2 opaque
    let rows = vec![vec![0x01, 0x20]];
    let extra: Vec<(&[u8; 4], Vec<u8>)> =
        vec![(b"PLTE", vec![10, 20, 30, 200, 100, 50, 0, 160, 80]), (b"tRNS", vec![255, 0])];
    f.insert(
        "palette".into(),
        encode(&Png {
            w: 3,
            h: 1,
            color_type: 3,
            depth: 4,
            rows: &rows,
            extra: &extra,
            filters: &[0],
            interlace: false,
        }),
    );
    let rows = vec![vec![77, 200]];
    f.insert(
        "grey".into(),
        encode(&Png { w: 2, h: 1, color_type: 0, depth: 8, rows: &rows, extra: &[], filters: &[0], interlace: false }),
    );
    let rows = vec![vec![0b1010_1010]];
    f.insert(
        "grey1".into(),
        encode(&Png { w: 8, h: 1, color_type: 0, depth: 1, rows: &rows, extra: &[], filters: &[0], interlace: false }),
    );
    let rows = vec![vec![200, 255, 50, 0]];
    f.insert(
        "greya".into(),
        encode(&Png { w: 2, h: 1, color_type: 4, depth: 8, rows: &rows, extra: &[], filters: &[0], interlace: false }),
    );
    let rows = vec![vec![0xFF, 0x00, 0x80, 0x00, 0x00, 0x00]];
    f.insert(
        "rgb16".into(),
        encode(&Png { w: 1, h: 1, color_type: 2, depth: 16, rows: &rows, extra: &[], filters: &[0], interlace: false }),
    );
    let rows: Vec<Vec<u8>> = (0..16u32)
        .map(|y| (0..16u32).flat_map(|x| [(x * 16) as u8, (y * 16) as u8, ((x * y) & 255) as u8]).collect())
        .collect();
    f.insert(
        "filters".into(),
        encode(&Png {
            w: 16,
            h: 16,
            color_type: 2,
            depth: 8,
            rows: &rows,
            extra: &[],
            filters: &[0, 1, 2, 3, 4],
            interlace: false,
        }),
    );
    // each 2x2 block: two black and two white pixels
    f.insert("checker".into(), rgba(4, 4, |x, y| if (x + y) % 2 == 0 { [255, 255, 255, 255] } else { [0, 0, 0, 255] }));
    f.insert("odd".into(), rgba(9, 5, |_, _| [0, 255, 0, 255]));
    let mut header = b"\x89PNG\r\n\x1a\n".to_vec();
    let big = rgba(1000, 600, |x, y| {
        let (dx, dy) = (x as f32 - 500.0, y as f32 - 300.0);
        let inside = dx * dx / (400.0 * 400.0) + dy * dy / (220.0 * 220.0) < 1.0;
        if inside {
            [40, 80, 160, 255]
        } else {
            [0, 0, 0, 0]
        }
    });
    header.extend_from_slice(&big[8..8 + 25]); // the IHDR chunk only
    f.insert("header".into(), header);
    f.insert("big".into(), big);
    let rows = vec![vec![1, 2, 3]];
    f.insert(
        "interlaced".into(),
        encode(&Png { w: 1, h: 1, color_type: 2, depth: 8, rows: &rows, extra: &[], filters: &[0], interlace: true }),
    );
    f.insert("jpeg".into(), b"\xFF\xD8\xFF\xE0\x00\x10JFIF\x00".to_vec());
    let noise = rgba(64, 64, |x, y| [(x * 37 % 251) as u8, (y * 91 % 241) as u8, ((x ^ y) * 13) as u8, 255]);
    f.insert("cut".into(), noise[..noise.len() / 2].to_vec());
    f.insert("square".into(), rgba(64, 64, |_, _| [255, 0, 255, 255]));
    // 1000 rows, cut after about 600: only the first 240 are on the screen
    let tall = rgba(16, 1000, |x, y| [(x * 16) as u8, (y & 255) as u8, 7, 255]);
    f.insert("tall_cut".into(), tall[..tall.len() * 6 / 10].to_vec());
    let mut huge = b"\x89PNG\r\n\x1a\n".to_vec();
    huge.extend_from_slice(&rgba(1, 20000, |_, _| [0, 0, 0, 255])[8..8 + 25]); // the IHDR only
    f.insert("huge".into(), huge);
    f
}

/// A Lua expression that must give true.
fn check(t: &mut T, name: &str, lua: &str) {
    let v = t.eval(lua);
    t.check(v == "true", name, |_| format!("{name}: {v}"));
}

pub fn run(t: &mut T) {
    t.say("== disp.png, disp.png_size, disp.grab");
    let files = Arc::new(files());
    let names: Vec<String> = files.keys().cloned().collect();
    let served = files.clone();
    let server = http::serve(
        http::Opts::default(),
        Arc::new(move |r: &Request| match served.get(r.path().trim_start_matches('/')) {
            Some(b) => Response::new(200, "image/png", b.clone()),
            None => Response::new(404, "text/plain", "no"),
        }),
    );
    t.boot();
    let list = names.iter().map(|n| format!("\"{n}\"")).collect::<Vec<_>>().join(", ");
    t.write("png.lua", SCRIPT.replace("$NAMES", &list).replace("$PORT", &server.port.to_string()));
    t.e(&["put", &t.path("png.lua"), "--run", "png.lua"]);
    if !t.until_net(5.0, |t| t.eval("DONE") == "true") {
        t.bad(&format!("png: test files not fetched: {}", t.eval("P.rgb")));
        return;
    }

    check(
        t,
        "png: RGB 3x2 drawn exactly",
        r#"disp.clear(0) local w, h = disp.png(10, 10, P.rgb)
        return w == 3 and h == 2 and near(10, 10, 255, 0, 0) and near(11, 10, 0, 255, 0) and near(12, 10, 0, 0, 255)
          and near(10, 11, 255, 255, 0) and near(11, 11, 0, 255, 255) and near(12, 11, 128, 64, 32)"#,
    );
    check(
        t,
        "png: RGBA blends over the screen",
        r#"disp.clear(0x0000FF) disp.png(0, 0, P.blend) return near(0, 0, 128, 0, 127)"#,
    );
    check(t, "png: fully transparent leaves the screen", r#"return near(1, 0, 0, 0, 255)"#);
    check(
        t,
        "png: palette with tRNS",
        r#"disp.clear(0xFFFFFF) disp.png(0, 0, P.palette) return near(0, 0, 10, 20, 30) and near(1, 0, 255, 255, 255) and near(2, 0, 0, 160, 80)"#,
    );
    check(
        t,
        "png: grey and grey+alpha",
        r#"disp.clear(0) disp.png(0, 0, P.grey) disp.png(0, 1, P.greya) disp.png(0, 2, P.grey1)
        return near(0, 0, 77, 77, 77) and near(1, 0, 200, 200, 200) and near(0, 1, 200, 200, 200) and near(1, 1, 0, 0, 0)
          and near(0, 2, 255, 255, 255) and near(1, 2, 0, 0, 0) and near(6, 2, 255, 255, 255)"#,
    );
    check(t, "png: 16-bit RGB", r#"disp.clear(0) disp.png(0, 0, P.rgb16) return near(0, 0, 255, 128, 0)"#);
    check(
        t,
        "png: every filter type",
        r#"disp.clear(0) disp.png(0, 0, P.filters) local bad = 0
        for y = 0, 15 do for x = 0, 15 do if not near(x, y, x * 16, y * 16, (x * y) & 255) then bad = bad + 1 end end end
        return bad == 0 or bad"#,
    );
    check(
        t,
        "png: scale 2 averages",
        r#"disp.clear(0xFF0000) local w, h = disp.png(0, 0, P.checker, 2)
        return w == 2 and h == 2 and near(0, 0, 128, 128, 128) and near(1, 1, 128, 128, 128) and near(2, 0, 255, 0, 0)"#,
    );
    check(t, "png: returns the drawn size", r#"local w, h = disp.png(0, 0, P.odd, 4) return w == 3 and h == 2"#);
    check(t, "png: png_size", r#"local w, h = disp.png_size(P.header) return w == 1000 and h == 600"#);
    check(
        t,
        "png: interlaced refused",
        r#"local ok, err = disp.png(0, 0, P.interlaced) return ok == nil and err:find("interlaced") ~= nil"#,
    );
    check(
        t,
        "png: not a PNG",
        r#"local ok, err = disp.png(0, 0, P.jpeg) local s, e2 = disp.png_size(P.jpeg)
        return ok == nil and err == "disp.png: not a PNG" and s == nil and e2 == "disp.png_size: not a PNG""#,
    );
    check(
        t,
        "png: cut short",
        r#"local ok, err = disp.png(0, 0, P.cut) return ok == nil and err == "disp.png: broken data""#,
    );
    check(
        t,
        "png: clipped off the screen",
        r#"disp.clear(0) disp.png(-32, 200, P.square) return near(0, 239, 255, 0, 255) and near(31, 239, 255, 0, 255) and near(32, 239, 0, 0, 0)"#,
    );
    check(
        t,
        "png: a 1000x600 RGBA at scale 4",
        r#"disp.clear(0xFFFFFF) local w, h = disp.png(0, 0, P.big, 4)
        return w == 250 and h == 150 and near(125, 75, 40, 80, 160) and near(0, 0, 255, 255, 255) and near(249, 149, 255, 255, 255)"#,
    );
    check(
        t,
        "png: decoding stops below the screen",
        r#"local w, h = disp.png(0, 0, P.tall_cut) return w == 16 and h == 1000"#,
    );
    check(
        t,
        "png: a PNG too tall to decode is refused",
        r#"local ok, err = disp.png(0, 0, P.huge) return ok == nil and err == "disp.png: PNG too big""#,
    );
    check(t, "png: scale must be 1, 2, 4 or 8", r#"return pcall(disp.png, 0, 0, P.rgb, 3) == false"#);
    check(
        t,
        "grab: round trip",
        r#"disp.clear(0) disp.rect(12, 14, 20, 10, 0xFF8800, true) disp.circle(30, 25, 6, 0x00FF00, true)
        local p = disp.grab(10, 10, 40, 30) disp.clear(0) disp.image(10, 10, 40, 30, p)
        return #p == 2400 and disp.grab(10, 10, 40, 30) == p and near(12, 14, 255, 136, 0)"#,
    );
    check(
        t,
        "grab: off the screen is black",
        r#"disp.clear(0xFFFFFF) local p = disp.grab(-2, 0, 4, 1) return p:sub(1, 4) == string.char(0, 0, 0, 0) and p:sub(5, 8) == string.char(255, 255, 255, 255)"#,
    );
    check(
        t,
        "grab: size checked",
        r#"return pcall(disp.grab, 0, 0, 0, 1) == false and pcall(disp.grab, 0, 0, 1, 5000) == false"#,
    );
    drop(server);
}
