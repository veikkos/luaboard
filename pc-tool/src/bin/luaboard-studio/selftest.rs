//! The self-test hook (LUABOARD_STUDIO_SCREENSHOT): run, screenshot, exit.

use super::*;

pub(crate) struct SelfTest {
    pub(crate) out: PathBuf,
    pub(crate) started: Option<std::time::Instant>,
    pub(crate) requested: bool,
}

impl Studio {
    pub(crate) fn run_selftest(&mut self, ctx: &egui::Context) {
        let Some(st) = self.selftest.as_mut() else { return };
        if !self.connected {
            ctx.request_repaint_after(Duration::from_millis(100));
            return;
        }
        let Some(t0) = st.started else {
            st.started = Some(std::time::Instant::now());
            self.upload(true);
            self.log(LineKind::Input, "lua> sys.heap()");
            self.send(Cmd::Eval("sys.heap()".into()));
            return;
        };
        if !st.requested && t0.elapsed() > Duration::from_millis(2500) {
            st.requested = true;
            let out = st.out.clone();
            ctx.request_screenshot(move |img| {
                let [w, h] = img.size;
                let mut buf = format!("P6\n{w} {h}\n255\n").into_bytes();
                for p in &img.pixels {
                    buf.extend_from_slice(&[p.r(), p.g(), p.b()]);
                }
                let _ = std::fs::write(&out, buf);
                std::process::exit(0);
            });
        }
        ctx.request_repaint_after(Duration::from_millis(100));
    }
}
