//! Every app in tools/own_apps.txt on its own fresh simulated board: upload,
//! run for a second of the device's time, look for runtime errors, check the
//! screen shows something. The rest of marketplace/ is the marketplace's.

use std::fs;
use std::path::Path;

use crate::harness::{Ppm, T};

pub fn run(t: &mut T) {
    t.say("== examples");
    let list = fs::read_to_string(t.root.join("tools/own_apps.txt")).unwrap();
    let mut dirs: Vec<_> = list
        .lines()
        .map(str::trim)
        .filter(|l| !l.is_empty() && !l.starts_with('#'))
        .map(|name| t.root.join("marketplace").join(name))
        .collect();
    dirs.sort();
    let results: Vec<Result<String, String>> = std::thread::scope(|s| {
        let handles: Vec<_> = dirs
            .iter()
            .map(|dir| {
                let mut ex = t.sub(&dir.file_name().unwrap().to_string_lossy());
                s.spawn(move || example(&mut ex, dir))
            })
            .collect();
        handles.into_iter().map(|h| h.join().unwrap_or_else(|_| Err("crashed".into()))).collect()
    });
    for r in results {
        match r {
            Ok(m) => t.ok(&m),
            Err(m) => t.bad(&m),
        }
    }
}

fn example(t: &mut T, dir: &Path) -> Result<String, String> {
    let name = dir.file_name().unwrap().to_string_lossy().into_owned();
    t.boot();
    let mut files: Vec<String> = fs::read_dir(dir)
        .unwrap()
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| p.extension().is_some_and(|x| x == "lua"))
        .map(|p| p.to_string_lossy().into_owned())
        .collect();
    files.sort();
    let mut args = vec!["-p", &t.tty, "--linger", "0", "put"];
    args.extend(files.iter().map(String::as_str));
    args.extend(["--run", "main.lua"]);
    let (put_ok, out) = t.cli_status(&[], &args);
    if put_ok {
        t.advance(1000); // a second of the device's time; what goes wrong in it is in its log
    }
    let log = t.read("sim.err");
    let wrong = |l: &&str| l.contains("error") || l.contains("script stopped") || l.contains(" failed: ");
    let res = if !put_ok {
        Err(format!("{name}: {out}"))
    } else if let Some(l) = out.lines().chain(log.lines()).find(wrong) {
        Err(format!("{name}: runtime error: {l}"))
    } else {
        t.stop();
        let px = Ppm::load(&t.work.join("screen.ppm")).lit_pixels();
        if px > 200 {
            Ok(format!("{name} ({px} lit pixels)"))
        } else {
            Err(format!("{name}: display empty"))
        }
    };
    t.stop();
    res
}
