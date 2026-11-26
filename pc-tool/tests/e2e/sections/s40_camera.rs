//! The CoreS3's camera: `camera.draw`, `camera.jpeg` and `camera.stop`. The
//! simulator's camera sees a test picture: color bars above a gradient, and
//! a white square that `camera=N` moves. The SE has no camera.

use crate::harness::T;

type Rgb = (u8, u8, u8);

fn near(a: Rgb, b: Rgb, tol: i32) -> bool {
    (a.0 as i32 - b.0 as i32).abs() <= tol
        && (a.1 as i32 - b.1 as i32).abs() <= tol
        && (a.2 as i32 - b.2 as i32).abs() <= tol
}

const WHITE: Rgb = (255, 255, 255);
const RED: Rgb = (255, 0, 0);
const BLUE: Rgb = (0, 0, 255);
const BLACK: Rgb = (0, 0, 0);

pub fn run(t: &mut T) {
    t.say("== the board's camera");
    t.wipe();
    t.boot_with(&["--model", "cores3"]);
    let off = t.ask("sensor=camera:off") && t.ask("sensor=camerapower:off");
    t.check(off, "the camera and its supplies are off until a script uses it", |_| "on at boot".into());

    let size = t.eval("table.concat({ camera.draw() }, ' ')");
    let on = t.ask("sensor=camera:on") && t.ask("sensor=camerapower:on");
    let live = t.shot("live.ppm");
    // The bars at the top, the gradient's far corner (red 239, green 213), the square.
    let seen = [live.rgb(20, 10), live.rgb(220, 10), live.rgb(260, 10), live.rgb(300, 200), live.rgb(40, 140)];
    let ok = near(seen[0], WHITE, 10)
        && near(seen[1], RED, 10)
        && near(seen[2], BLUE, 10)
        && near(seen[3], (239, 213, 0), 10)
        && near(seen[4], WHITE, 10);
    t.check(size == "320 240" && on && ok, "camera.draw() puts the picture on the screen", |_| {
        format!("size {size:?}, on {on}, pixels {seen:?}")
    });

    let size =
        t.eval("(function() disp.clear(disp.BLACK) return table.concat({ camera.draw(160, 120, 2) }, ' ') end)()");
    let s = t.shot("half.ppm");
    let seen = [s.rgb(150, 110), s.rgb(170, 125), s.rgb(270, 125), s.rgb(180, 190)];
    let ok = near(seen[0], BLACK, 10) && near(seen[1], WHITE, 10) && near(seen[2], RED, 10) && near(seen[3], WHITE, 10);
    t.check(size == "160 120" && ok, "at half size, where it was asked for", |_| {
        format!("size {size:?}, pixels {seen:?}")
    });

    let v = t.eval(
        "(function() local j = camera.jpeg() disp.clear(disp.BLACK) \
         return table.concat({ j:byte(1), j:byte(2), j:byte(-2), j:byte(-1), disp.jpeg(0, 0, j) }, ' ') end)()",
    );
    let s = t.shot("jpeg.ppm");
    let points = [(20, 10), (220, 10), (260, 10), (300, 200), (40, 140), (100, 100)];
    let same = points.iter().all(|&(x, y)| near(s.rgb(x, y), live.rgb(x, y), 30));
    t.check(v == "255 216 255 217 320 240" && same, "camera.jpeg() is a JPEG of the same picture", |_| {
        format!("{v:?}, decoded {:?}", points.iter().map(|&(x, y)| s.rgb(x, y)).collect::<Vec<_>>())
    });
    let v = t.eval("#camera.jpeg(10) < #camera.jpeg(95)");
    t.check(v == "true", "a lower quality is a smaller JPEG", |_| v.clone());

    t.ctl(&["camera=3"]);
    t.eval("camera.draw()");
    let s = t.shot("moved.ppm");
    let seen = [s.rgb(40, 140), s.rgb(160, 140)];
    t.check(!near(seen[0], WHITE, 10) && near(seen[1], WHITE, 10), "the picture follows what the camera sees", |_| {
        format!("pixels {seen:?}")
    });

    // Arguments as the disp calls take them: a flag is true or false, and a
    // place far off the screen is off the screen.
    let v = t.eval("tostring(pcall(camera.mirror, 0)) .. ' ' .. tostring(pcall(camera.draw, 0, 0, 3))");
    t.eval(
        "(function() disp.clear(disp.BLACK) camera.draw(4294967296 + 5, 0) camera.draw(0, 9007199254740992, 2) end)()",
    );
    let far = t.shot("far.ppm").rgb(25, 10);
    t.check(
        v == "false false" && near(far, BLACK, 10),
        "a flag that isn't one and a scale there isn't are errors",
        |_| format!("{v:?}, far off: {far:?}"),
    );

    // Mirrored: the white bar from the left edge is at the right one. It
    // holds over the camera being off, and jpeg() gives the same picture.
    t.eval("(function() camera.mirror(true) camera.stop() camera.draw() end)()");
    let s = t.shot("mirror.ppm");
    let seen = [s.rgb(20, 10), s.rgb(299, 10)];
    let v = t.eval("(function() local j = camera.jpeg() disp.clear(disp.BLACK) return disp.jpeg(0, 0, j) end)()");
    let j = t.shot("mirror-jpeg.ppm").rgb(299, 10);
    t.eval("(function() camera.mirror(false) camera.draw() end)()");
    let back = t.shot("unmirror.ppm").rgb(20, 10);
    let ok = !near(seen[0], WHITE, 10) && near(seen[1], WHITE, 10) && near(j, WHITE, 30) && near(back, WHITE, 10);
    t.check(v.starts_with("320") && ok, "camera.mirror() mirrors the picture, drawn and as a JPEG", |_| {
        format!("mirrored {seen:?}, jpeg {j:?} ({v}), back {back:?}")
    });

    // The light sensor shares one of the camera's supplies: it stays on for it.
    let lux = t.eval("(function() light.lux() camera.stop() return light.lux() end)()");
    let state = t.ask("sensor=camera:off") && t.ask("sensor=camerapower:off") && t.ask("sensor=lightpower:on");
    let lit = lux.parse::<f64>().is_ok_and(|v| (v - 300.0).abs() < 2.0);
    t.check(lit && state, "camera.stop() switches it off, the light sensor goes on measuring", |_| {
        format!("lux {lux:?}, state {state}")
    });

    t.write("cam/main.lua", "camera.mirror(true)\nfunction loop() W = camera.draw() end\n");
    t.e(&["put", &t.path("cam/main.lua"), "--app", "cam", "--run", "main.lua"]);
    t.until(2.0, |t| t.eval("tostring(W)") == "320");
    let on = t.ask("sensor=camera:on");
    t.e(&["stop"]);
    let off = t.ask("sensor=camera:off") && t.ask("sensor=camerapower:off");
    t.check(on && off, "the camera is switched off when the script ends", |_| format!("on {on}, off after {off}"));
    t.eval("camera.draw()");
    let plain = t.shot("after.ppm").rgb(20, 10);
    t.check(near(plain, WHITE, 10), "... and the next script's picture isn't mirrored", |_| format!("{plain:?}"));
    t.eval("camera.stop()");
    // A script whose only drawing is the camera's picture has drawn.
    t.write("view/main.lua", "function loop() W = camera.draw(80, 60, 2) end\n");
    t.e(&["put", &t.path("view/main.lua"), "--app", "view", "--run", "main.lua"]);
    t.until(2.0, |t| t.eval("tostring(W)") == "160");
    t.advance(2000);
    let s = t.shot("view.ppm");
    let (edge, picture) = (s.rgb(5, 1), s.rgb(90, 65));
    t.check(
        near(edge, BLACK, 10) && near(picture, WHITE, 10),
        "a script that draws only the camera's picture gets no notice",
        |_| format!("screen's edge {edge:?}, picture {picture:?}"),
    );
    t.e(&["stop"]);
    // The camera driver's warnings are the ROM's printf, which writes to the
    // USB port too unless the firmware has taken that away.
    let log = t.read("sim.err");
    let warned = log.contains("cam_hal:");
    let stray = log.contains("printf wrote onto the USB link");
    t.check(warned && !stray, "the camera driver's warnings stay off the USB link", |_| {
        format!("warned {warned}, on the link {stray}")
    });
    t.stop();

    t.say("   the SE has no camera");
    t.wipe();
    t.boot();
    let v = t.eval("tostring(camera.draw()) .. ' / ' .. select(2, camera.jpeg())");
    t.check(v == "nil / no camera on this board", "camera on the SE: nil and why", |_| v.clone());
    t.stop();
}
