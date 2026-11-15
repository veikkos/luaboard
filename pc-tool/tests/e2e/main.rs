//! End-to-end tests: headless simulators driven by the luaboard CLI, against
//! fake servers running in this process. Each section (sections/sNN_*.rs) runs
//! in a thread of its own, with its own simulator, work folder and ports; the
//! output is shown per section once all have finished.
//!
//!   cargo test --release --test e2e              all sections
//!   cargo test --release --test e2e -- oauth menu only sections whose name contains a word
//!
//!   E2E_ONLY="gcal menu"  only these sections, when no words are given
//!   E2E_KEEP=1   keep the work folder (logs, screenshots) of a failed section
//!   E2E_LIMIT=N  stop a section after N seconds (default 120)
//!   SIM=PATH     the simulator (default build/m5stack-cores3/esp32-sim)
//!
//! The same executable is the "browser" for logins: `e2e --browse URL`
//! fetches URL and follows its redirects (see T::browse).

mod fakes;
mod harness;
mod pngs;
mod sections;

use std::panic::{self, AssertUnwindSafe};
use std::sync::Arc;
use std::time::{Duration, Instant};

use harness::{Shared, T};

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.first().map(String::as_str) == Some("--browse") {
        let url = args.get(1).expect("--browse URL");
        if let Err(e) = fakes::http::browse(url) {
            eprintln!("browse {url}: {e}");
            std::process::exit(1);
        }
        return;
    }
    // (cargo passes its own flags through, e.g. --nocapture: not words)
    let mut words: Vec<String> = args.iter().filter(|a| !a.starts_with('-')).cloned().collect();
    if words.is_empty() {
        words = std::env::var("E2E_ONLY").unwrap_or_default().split_whitespace().map(String::from).collect();
    }
    let chosen: Vec<_> = sections::ALL
        .iter()
        .filter(|(name, _)| words.is_empty() || words.iter().any(|w| name.contains(w.as_str())))
        .collect();
    if chosen.is_empty() {
        eprintln!("no section matches: {}", words.join(" "));
        std::process::exit(2);
    }
    let limit = std::env::var("E2E_LIMIT").ok().and_then(|s| s.parse().ok()).unwrap_or(120u64);

    // A failed check panics only for what stops a section; keep the message
    // for its output instead of printing it in the middle of the run.
    panic::set_hook(Box::new(|info| {
        let msg = match (info.payload().downcast_ref::<&str>(), info.payload().downcast_ref::<String>()) {
            (Some(s), _) => s.to_string(),
            (_, Some(s)) => s.clone(),
            _ => "panic".into(),
        };
        let at = info.location().map(|l| format!(" ({}:{})", l.file(), l.line())).unwrap_or_default();
        harness::PANIC_MSG.with(|m| *m.borrow_mut() = format!("{msg}{at}"));
    }));

    // A program built just now is checked by the system when it's first
    // started (macOS: a tenth of a second and more). Once here, not by every
    // section at the same moment, where some simulators then took seconds.
    let sim = std::env::var_os("SIM")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| harness::root().join(format!("build/m5stack-cores3/esp32-sim{}", harness::EXE)));
    for (program, arg) in
        [(sim.as_os_str(), "--help"), (std::ffi::OsStr::new(env!("CARGO_BIN_EXE_luaboard")), "--version")]
    {
        let _ = std::process::Command::new(program).arg(arg).output();
    }

    let start = Instant::now();
    let mut running = Vec::new();
    for (name, run) in &chosen {
        let sh = Arc::new(Shared::default());
        let sh2 = sh.clone();
        let (name, run) = (*name, *run);
        std::thread::Builder::new()
            .name(name.into())
            .spawn(move || {
                let t0 = Instant::now();
                for attempt in 1..=2 {
                    let mut t = T::new(name, sh2.clone());
                    let res = panic::catch_unwind(AssertUnwindSafe(|| run(&mut t)));
                    if res.is_err() {
                        let msg = harness::PANIC_MSG.with(|m| m.borrow().clone());
                        t.fail_line(&format!("section stopped early: {msg}"));
                    }
                    // A port picked ahead and taken by something else isn't
                    // this section's fault: once more, with new ports.
                    let again = attempt == 1 && t.failed() && t.port_clash();
                    drop(t); // stops the simulator, removes (or keeps) the work folder
                    if !again {
                        break;
                    }
                    sh2.reset();
                    sh2.say(&format!("  (a port it picked was taken meanwhile: {name} ran again)"));
                }
                if harness::times() {
                    sh2.say(&format!("  [{name}: {} ms: {}]", t0.elapsed().as_millis(), sh2.spent_on()));
                }
                sh2.finish(t0.elapsed().as_secs());
            })
            .expect("thread");
        running.push((name, sh));
    }

    // A hung section must not hang the whole run.
    loop {
        let open: Vec<_> = running.iter().filter(|(_, sh)| !sh.is_done()).collect();
        if open.is_empty() {
            break;
        }
        if start.elapsed() > Duration::from_secs(limit) {
            for (_, sh) in open {
                sh.fail_line(&format!("section ran over {limit} s"));
                sh.kill_children();
                sh.finish(limit);
            }
            break;
        }
        std::thread::sleep(Duration::from_millis(50));
    }

    let (mut pass, mut fail) = (0, 0);
    let mut slow = Vec::new();
    for (name, sh) in &running {
        print!("{}", sh.output());
        let (p, f, secs) = sh.result();
        pass += p;
        fail += f;
        slow.push((secs, *name));
    }
    slow.sort_by_key(|s| std::cmp::Reverse(s.0));
    println!();
    let top: Vec<String> = slow.iter().take(3).map(|(s, n)| format!("{n} {s}s")).collect();
    println!("slowest sections: {}", top.join("  "));
    println!("{pass} passed, {fail} failed");
    // Don't wait for threads of sections that ran over.
    std::process::exit(if fail == 0 { 0 } else { 1 });
}
