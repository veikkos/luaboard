//! The `sd` module: files on the microSD card. In the simulator the card is
//! a folder (`sd=DIR`), `sd=none` an empty slot, `sd=unformatted` a card
//! that isn't FAT.

use crate::harness::{Ppm, T};

fn failed(v: &str) -> bool {
    v.starts_with("nil\t")
}

pub fn run(t: &mut T) {
    t.say("== sd: files on the microSD card");
    t.wipe();
    t.boot();
    let v = t.eval("sd.info()");
    t.check(failed(&v) && v.contains("no card"), "no card: sd.info() is nil and why", |_| format!("info: {v}"));
    let v = t.eval("sd.read('hello.txt')");
    t.check(failed(&v) && v.contains("no card"), "... and so is every other call", |_| format!("read: {v}"));

    t.write("card/hello.txt", "hi");
    t.ctl(&[&format!("sd={}", t.path("card")), "advance=2100"]); // (an empty slot is looked at again after 2 s)
    let v =
        t.eval("local total, free = sd.info() return math.type(total) .. ' ' .. tostring(free > 0 and free <= total)");
    t.check(v == "integer true", "a card is in: sd.info() gives its size and free space", |_| format!("info: {v}"));
    let v = t.eval("sd.read('hello.txt')");
    t.check(v == "hi", "sd.read: a whole file", |_| format!("read: {v}"));

    let v = t.eval("sd.write('log/a.txt', 'one\\n')");
    t.check(failed(&v), "writing into a folder that isn't there fails", |_| format!("write: {v}"));
    let v = t.eval("tostring(sd.mkdir('log')) .. tostring(sd.write('log/a.txt', 'one\\n')) .. tostring(sd.append('log/a.txt', 'two\\n'))");
    let on_card = t.read("card/log/a.txt");
    t.check(v == "truetruetrue" && on_card == "one\ntwo\n", "mkdir, write, append: on the card", |_| {
        format!("calls: {v}, file: {on_card:?}")
    });
    let v = t.eval(
        "tostring(sd.read('log/a.txt', 4) == 'two\\n') .. '|' .. sd.read('log/a.txt', 4, 2) .. '|' .. #sd.read('log/a.txt', 100)",
    );
    t.check(v == "true|tw|0", "sd.read(path, offset, n): a part, and nothing past the end", |_| {
        format!("reads: {v:?}")
    });
    let v = t.eval("tostring(sd.exists('log')) .. ' ' .. tostring(sd.exists('log/a.txt')) .. ' ' .. tostring(sd.exists('nope')) .. ' ' .. sd.size('log/a.txt')");
    t.check(v == "dir file false 8", "sd.exists and sd.size", |_| format!("exists, size: {v}"));
    let v = t.eval(
        "local out = {} for _, e in ipairs(sd.list()) do out[#out + 1] = e.name .. ':' .. e.size .. ':' .. tostring(e.dir) end return table.concat(out, ' ')",
    );
    t.check(v == "hello.txt:2:false log:0:true", "sd.list(): names, sizes, folders, sorted", |_| format!("list: {v}"));
    let v = t.eval("local l = sd.list('log') return #l .. ' ' .. l[1].name");
    t.check(v == "1 a.txt", "sd.list(dir)", |_| format!("list: {v}"));

    for bad in
        ["'../x'", "''", "'a\\\\b'", "string.rep('x', 300)", "'log/../../x'", "12", "'log.'", "'a /b'", "'x\\127'"]
    {
        let v = t.eval(&format!("tostring((pcall(sd.read, {bad})))"));
        t.check(v == "false", &format!("sd.read({bad}) is refused"), |_| format!("{bad}: {v}"));
    }

    let v = t.eval("tostring(sd.remove('log/a.txt')) .. tostring(sd.remove('log')) .. tostring(sd.exists('log'))");
    let gone = t.eval("sd.remove('nope')");
    t.check(v == "truetruefalse" && failed(&gone), "sd.remove: a file, an empty folder; not what isn't there", |_| {
        format!("remove: {v}, missing: {gone}")
    });

    // The card pulled out, and another put in.
    t.ctl(&["sd=none"]);
    let v = t.eval("sd.read('hello.txt')");
    t.check(failed(&v), "the card pulled out: calls fail", |_| format!("read: {v}"));
    t.ctl(&["sd=unformatted", "advance=2100"]);
    let v = t.eval("sd.info()");
    t.check(failed(&v) && v.contains("FAT"), "a card that isn't FAT: it says so", |_| format!("info: {v}"));
    t.ctl(&[&format!("sd={}", t.path("card")), "advance=2100"]);

    // Pulled out between two calls: the next one says so, also one that would
    // have found nothing on the card anyway.
    let had = t.eval("sd.exists('hello.txt')");
    t.ctl(&["sd=none"]);
    let v = t.eval("sd.exists('nope')");
    t.check(
        had == "file" && failed(&v) && v.contains("no card"),
        "pulled out between calls: the next says no card",
        |_| format!("before: {had}, after: {v}"),
    );
    // Swapped for another between two calls: the next one is on the new card.
    t.ctl(&[&format!("sd={}", t.path("card")), "advance=2100"]);
    let had = t.eval("sd.exists('hello.txt')");
    t.ctl(&["sd=none", &format!("sd={}", t.path("card2"))]);
    let v = t.eval("tostring(sd.exists('hello.txt')) .. tostring(sd.write('new.txt', '1'))");
    let there = std::path::Path::new(&t.path("card2/new.txt")).exists();
    t.check(had == "file" && v == "falsetrue" && there, "swapped between calls: the next is on the new card", |_| {
        format!("before: {had}, after: {v}, file there: {there}")
    });
    t.ctl(&[&format!("sd={}", t.path("card")), "advance=2100"]);

    // A script that used the card: it's let go of when the script ends.
    t.write("logger/main.lua", "OK = sd.append('boot.txt', 'x')\nfunction loop() end\n");
    t.e(&["put", &t.path("logger/main.lua"), "--app", "logger", "--run", "main.lua"]);
    t.until(2.0, |t| t.e(&["info"]).contains("running: logger/main.lua"));
    let ok = t.eval("tostring(OK)") == "true" && t.ask("sdcard=mounted");
    t.e(&["stop"]);
    let free = t.ask("sdcard=free");
    t.check(ok && free, "the card is let go of when the script ends", |_| format!("used {ok}, free after {free}"));

    // The card shares a pin with the display (its data out is the display's
    // D/C): after card use the display must still take what's drawn. The
    // simulated panel's own picture is written when the simulator stops.
    let v = t.eval("sd.read('hello.txt')");
    t.eval("disp.clear(disp.RED) disp.show()");
    t.stop();
    let red = Ppm::load(&t.work.join("screen.ppm")).red_pixels();
    t.check(v == "hi" && red > 70_000, "the display still draws after card use", |_| {
        format!("read {v}, red pixels: {red}")
    });
}
