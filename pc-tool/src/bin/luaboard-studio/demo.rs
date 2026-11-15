//! Test hook for recording a demo (the README's GIF): with
//! LUABOARD_STUDIO_DEMO=steps.txt and LUABOARD_STUDIO_RECORD=dir, once
//! connected the studio turns Live update on, runs the project and types
//! the edits from the steps file into the open file, a character at a time
//! as a person would, while it saves a frame of its window every 100 ms
//! (dir/frame-00000.ppm, ...). Then it exits.
//!
//! Steps, one per line (# comments):
//!   wait MS            do nothing for that long
//!   type OLD => NEW    erase OLD where it first appears, then type NEW
//!   insert OLD => NEW  type NEW in front of OLD, then delete OLD: the code
//!                      stays valid all along (12 -> 2412 -> 24)
//!   end                stop recording and exit

use std::path::PathBuf;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use eframe::egui;

use super::{Studio, LIVE_QUIET};

const FRAME_EVERY: Duration = Duration::from_millis(100);
const KEY_EVERY: Duration = Duration::from_millis(70);

enum Step {
    Wait(u64),
    Type(String, String),
    Insert(String, String),
    End,
}

/// An edit being typed: OLD is erased from its end, then NEW typed in;
/// or (`new_first`) NEW typed in front of OLD, then OLD deleted.
struct Typing {
    at: usize,
    old: String,
    new: Vec<char>,
    typed: usize,
    new_first: bool,
}

pub struct Demo {
    steps: Vec<Step>,
    next_step: usize,
    dir: PathBuf,
    started: bool,
    until: Instant,
    typing: Option<Typing>,
    frames: Arc<AtomicUsize>,
    shot_at: Instant,
}

impl Demo {
    pub fn from_env() -> Option<Demo> {
        let steps = std::fs::read_to_string(std::env::var_os("LUABOARD_STUDIO_DEMO")?).ok()?;
        let dir = PathBuf::from(std::env::var_os("LUABOARD_STUDIO_RECORD")?);
        let _ = std::fs::create_dir_all(&dir);
        let steps = steps
            .lines()
            .map(str::trim)
            .filter(|l| !l.is_empty() && !l.starts_with('#'))
            .filter_map(|l| {
                if let Some(ms) = l.strip_prefix("wait ") {
                    ms.trim().parse().ok().map(Step::Wait)
                } else if let Some(edit) = l.strip_prefix("type ") {
                    let (old, new) = edit.split_once(" => ")?;
                    Some(Step::Type(old.into(), new.into()))
                } else if let Some(edit) = l.strip_prefix("insert ") {
                    let (old, new) = edit.split_once(" => ")?;
                    Some(Step::Insert(old.into(), new.into()))
                } else {
                    (l == "end").then_some(Step::End)
                }
            })
            .collect();
        Some(Demo {
            steps,
            next_step: 0,
            dir,
            started: false,
            until: Instant::now(),
            typing: None,
            frames: Arc::new(AtomicUsize::new(0)),
            shot_at: Instant::now(),
        })
    }
}

impl Studio {
    pub(super) fn run_demo(&mut self, ctx: &egui::Context) {
        let Some(mut d) = self.demo.take() else { return };
        ctx.request_repaint_after(Duration::from_millis(20));
        if self.connected {
            if !d.started {
                d.started = true;
                self.live = true;
                self.upload(true);
                d.until = Instant::now() + Duration::from_millis(1500);
            }
            record(&mut d, ctx);
            if !self.demo_step(&mut d) {
                return; // the end: exits once the last frame is written
            }
        }
        self.demo = Some(d);
    }

    /// One step's worth of work; false at the end.
    fn demo_step(&mut self, d: &mut Demo) -> bool {
        let now = Instant::now();
        if now < d.until {
            return true;
        }
        if let Some(t) = &mut d.typing {
            let script = &mut self.scripts[self.selected];
            let typed_bytes = t.new[..t.typed].iter().map(|c| c.len_utf8()).sum::<usize>();
            if t.new_first && t.typed < t.new.len() {
                script.text.insert(t.at + typed_bytes, t.new[t.typed]);
                t.typed += 1;
            } else if t.new_first {
                let c = t.old.remove(0); // delete forward, after what was typed
                script.text.replace_range(t.at + typed_bytes..t.at + typed_bytes + c.len_utf8(), "");
            } else if t.old.pop().is_some() {
                script.text.remove(t.at + t.old.len()); // the character just past what's left of OLD
            } else if t.typed < t.new.len() {
                script.text.insert(t.at + typed_bytes, t.new[t.typed]);
                t.typed += 1;
            }
            script.dirty = true;
            script.checked = false;
            self.live_due = Some(now + LIVE_QUIET);
            if t.old.is_empty() && t.typed == t.new.len() {
                d.typing = None;
            }
            d.until = now + KEY_EVERY;
            return true;
        }
        let Some(step) = d.steps.get(d.next_step) else {
            return self.demo_end(d);
        };
        d.next_step += 1;
        match step {
            Step::Wait(ms) => d.until = now + Duration::from_millis(*ms),
            Step::Type(old, new) | Step::Insert(old, new) => {
                match self.scripts[self.selected].text.find(old.as_str()) {
                    Some(at) => {
                        let new_first = matches!(step, Step::Insert(..));
                        d.typing =
                            Some(Typing { at, old: old.clone(), new: new.chars().collect(), typed: 0, new_first })
                    }
                    None => eprintln!("demo: no {old:?} in the open file"),
                }
            }
            Step::End => return self.demo_end(d),
        }
        true
    }

    fn demo_end(&mut self, d: &Demo) -> bool {
        let frames = d.frames.clone();
        std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(500)); // the last screenshots land
            eprintln!("demo: {} frames", frames.load(Ordering::SeqCst));
            std::process::exit(0);
        });
        false
    }
}

/// A frame every FRAME_EVERY, halved on a high-density screen so the files stay small.
fn record(d: &mut Demo, ctx: &egui::Context) {
    if d.shot_at.elapsed() < FRAME_EVERY {
        return;
    }
    d.shot_at = Instant::now();
    let n = d.frames.fetch_add(1, Ordering::SeqCst);
    let out = d.dir.join(format!("frame-{n:05}.ppm"));
    let scale = ctx.pixels_per_point().round().max(1.0) as usize;
    ctx.request_screenshot(move |img| {
        let [w, h] = img.size;
        let (ow, oh) = (w / scale, h / scale);
        let mut buf = format!("P6\n{ow} {oh}\n255\n").into_bytes();
        for y in 0..oh {
            for x in 0..ow {
                let mut sum = [0u32; 3];
                for dy in 0..scale {
                    for dx in 0..scale {
                        let p = img.pixels[(y * scale + dy) * w + x * scale + dx];
                        sum[0] += p.r() as u32;
                        sum[1] += p.g() as u32;
                        sum[2] += p.b() as u32;
                    }
                }
                let k = (scale * scale) as u32;
                buf.extend_from_slice(&[(sum[0] / k) as u8, (sum[1] / k) as u8, (sum[2] / k) as u8]);
            }
        }
        let _ = std::fs::write(out, buf);
    });
}
