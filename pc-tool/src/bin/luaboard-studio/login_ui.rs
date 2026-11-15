//! A script's browser login (luaboard::login).

use super::*;

/// A script's request to log in somewhere through the browser (generic: the
/// studio doesn't know the service). See luaboard::login.
pub(crate) struct LoginPrompt {
    pub(crate) req: login::Request,
    /// Running login: set to stop waiting; `result` is filled by the thread.
    pub(crate) job: Option<Arc<LoginJob>>,
    pub(crate) error: Option<String>,
}

#[derive(Default)]
pub(crate) struct LoginJob {
    pub(crate) cancel: AtomicBool,
    pub(crate) failed: Mutex<Option<String>>,
}

impl Studio {
    /// Show (Some) or close (None) the login prompt; stops a running login.
    pub(crate) fn set_login(&mut self, req: Option<login::Request>) {
        if let Some(old) = &self.login {
            if req.as_ref().is_some_and(|r| r.id == old.req.id) {
                return; // already showing this one
            }
            if let Some(job) = &old.job {
                job.cancel.store(true, Ordering::Relaxed);
            }
        }
        self.login = req.map(|req| LoginPrompt { req, job: None, error: None });
        if self.auto_login && self.login.is_some() {
            self.start_login();
        }
    }

    /// Open the browser and wait (in a thread) for the redirect.
    pub(crate) fn start_login(&mut self) {
        let Some(l) = self.login.as_mut() else { return };
        let job = Arc::new(LoginJob::default());
        l.job = Some(job.clone());
        l.error = None;
        let req = l.req.clone();
        let tx = self.tx.clone();
        let ctx = self.ctx.clone();
        thread::spawn(move || {
            match login::catch_redirect(&req, true, &job.cancel, Duration::from_secs(600)) {
                Ok(q) => {
                    let _ = tx.send(Cmd::BrowserAnswer(req.id, q));
                }
                Err(e) => *job.failed.lock().unwrap() = Some(format!("{e:#}")),
            }
            ctx.request_repaint();
        });
    }

    pub(crate) fn login_window(&mut self, ctx: &egui::Context) {
        let Some(l) = self.login.as_mut() else { return };
        // A failed or cancelled attempt: back to the buttons, with the reason.
        let failed = l.job.as_ref().and_then(|job| {
            let e = job.failed.lock().unwrap().take()?;
            Some((!job.cancel.load(Ordering::Relaxed)).then_some(e))
        });
        if let Some(e) = failed {
            l.error = e;
            l.job = None;
        }
        let (mut start, mut decline, mut stop) = (false, false, false);
        egui::Window::new(format!("Log in: {}", l.req.title))
            .collapsible(false)
            .resizable(false)
            .anchor(egui::Align2::CENTER_CENTER, [0.0, 0.0])
            .show(ctx, |ui| {
                ui.set_max_width(420.0);
                ui.label(format!(
                    "The app \"{}\" on the device wants you to log in to {} at {}.",
                    l.req.app,
                    l.req.title,
                    l.req.host()
                ));
                ui.weak(
                    "Only continue if you expect this app to use that account: whatever you allow \
                     on the login page, this app gets.",
                );
                ui.weak(
                    "Your browser opens that page. After you log in, the studio passes a one-time code \
                     to the device, which finishes the login itself. Your password and tokens never \
                     go through the studio.",
                );
                if let Some(e) = &l.error {
                    ui.colored_label(egui::Color32::from_rgb(255, 110, 110), e);
                }
                ui.add_space(4.0);
                if l.job.is_some() {
                    ui.horizontal(|ui| {
                        ui.spinner();
                        ui.label("Waiting for the login in your browser…");
                    });
                    ui.horizontal(|ui| {
                        if ui.button("Copy link").on_hover_text("If the browser didn't open").clicked() {
                            ui.ctx().copy_text(l.req.url.clone());
                        }
                        if ui.button("Cancel").clicked() {
                            stop = true;
                        }
                    });
                } else {
                    ui.horizontal(|ui| {
                        if ui.button("Open browser and log in").clicked() {
                            start = true;
                        }
                        if ui.button("Not now").clicked() {
                            decline = true;
                        }
                    });
                }
            });
        if start {
            self.start_login();
        }
        if stop {
            if let Some(job) = self.login.as_mut().and_then(|l| l.job.take()) {
                job.cancel.store(true, Ordering::Relaxed);
            }
        }
        if decline {
            let id = self.login.as_ref().map(|l| l.req.id).unwrap_or(0);
            self.send(Cmd::BrowserCancel(id));
            self.set_login(None);
        }
        if self.login.as_ref().is_some_and(|l| l.job.is_some()) {
            ctx.request_repaint_after(Duration::from_millis(250));
        }
    }
}
