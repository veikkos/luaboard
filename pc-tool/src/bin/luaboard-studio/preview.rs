//! The live preview of the device's screen, and remote touch.

use super::*;

/// How often the live display preview polls the framebuffer.
const PREVIEW_INTERVAL: Duration = Duration::from_millis(250);
/// ... and for a while after touching the device from here, to show what it did.
const PREVIEW_INTERVAL_ACTIVE: Duration = Duration::from_millis(60);
const PREVIEW_ACTIVE_FOR: Duration = Duration::from_secs(2);
/// A finger held from here is said again this often (the device lets go after 1 s).
const TOUCH_KEEPALIVE: Duration = Duration::from_millis(300);
/// The device's touch area: the screen and the A/B/C strip below it.
const TOUCH_W: u32 = 320;
const TOUCH_H: u32 = 280;
const STRIP_H: u32 = 40;

/// Live view of the device display, fed by polling the `FB` command, and
/// remote control: the mouse and keys touch the device through `TOUCH`/`KEY`.
/// Nothing is polled or sent while the panel is hidden.
pub(crate) struct Preview {
    /// The panel is open (toolbar toggle).
    pub(crate) shown: bool,
    pub(crate) live: bool,
    pub(crate) pending: bool,
    pub(crate) last_request: Instant,
    pub(crate) texture: Option<egui::TextureHandle>,
    pub(crate) have_frame: bool,
    /// Last frame size (width, height); the device decides.
    pub(crate) size: [usize; 2],
    /// CRC of the frame shown: the device sends a new one only if it changed.
    pub(crate) crc: Option<u32>,
    pub(crate) remote: Remote,
}

/// The finger this computer puts on the device. Changes are sent one at a
/// time (each waits for its reply), so a slow link doesn't pile them up:
/// moves in between collapse into the newest, presses and releases all go.
#[derive(Default)]
pub(crate) struct Remote {
    /// To send, oldest first: Some(point) down/move, None up; Power: the power key.
    pub(crate) queue: std::collections::VecDeque<RemoteOp>,
    pub(crate) in_flight: bool,
    /// Where the device has the finger (as far as sent).
    pub(crate) down: Option<(u32, u32)>,
    pub(crate) last_sent: Option<Instant>,
    /// When this computer last touched (polls faster for a while after).
    pub(crate) last_input: Option<Instant>,
    /// The keyboard key holding the finger down, if a key does.
    pub(crate) key: Option<egui::Key>,
    /// The mouse holds the finger down.
    pub(crate) mouse: bool,
}

#[derive(Clone, Copy, PartialEq)]
pub(crate) enum RemoteOp {
    Touch(Option<(u32, u32)>),
    Power,
}

impl Studio {
    pub(crate) fn update_preview_texture(&mut self, fb: &Frame) {
        let pixels = (0..fb.width * fb.height)
            .map(|i| {
                let (r, g, b) = fb.rgb(i % fb.width, i / fb.width);
                egui::Color32::from_rgb(r, g, b)
            })
            .collect();
        let img = egui::ColorImage::new([fb.width, fb.height], pixels);
        match &mut self.preview.texture {
            Some(t) => t.set(img, egui::TextureOptions::NEAREST),
            None => self.preview.texture = Some(self.ctx.load_texture("lcd", img, egui::TextureOptions::NEAREST)),
        }
        self.preview.size = [fb.width, fb.height];
        self.preview.have_frame = true;
    }

    /// While the Display panel is open: request a new framebuffer when live
    /// preview is on and none is in flight, and send the remote finger.
    /// A hidden panel costs nothing: no polls, and a finger left down is let go.
    pub(crate) fn poll_preview(&mut self) {
        if !self.connected {
            return;
        }
        if !self.preview.shown {
            let rm = &mut self.preview.remote;
            if rm.down.is_some() && !rm.queue.contains(&RemoteOp::Touch(None)) {
                rm.key = None;
                rm.mouse = false;
                rm.queue.push_back(RemoteOp::Touch(None));
            }
        }
        self.pump_remote();
        if !self.preview.shown {
            return;
        }
        let active = self.preview.remote.last_input.is_some_and(|t| t.elapsed() < PREVIEW_ACTIVE_FOR);
        let interval = if active { PREVIEW_INTERVAL_ACTIVE } else { PREVIEW_INTERVAL };
        let p = &mut self.preview;
        if p.live && !p.pending && p.last_request.elapsed() >= interval {
            p.pending = true;
            p.last_request = Instant::now();
            let _ = self.tx.send(Cmd::Framebuffer(p.crc));
        }
        if p.live || p.remote.down.is_some() || !p.remote.queue.is_empty() {
            self.ctx.request_repaint_after(interval.min(TOUCH_KEEPALIVE));
        }
    }

    /// Send the next queued finger change, or say again where a held finger is.
    pub(crate) fn pump_remote(&mut self) {
        let rm = &mut self.preview.remote;
        if rm.in_flight {
            return;
        }
        let op = match rm.queue.pop_front() {
            Some(op) => op,
            None if rm.down.is_some() && rm.last_sent.is_none_or(|t| t.elapsed() >= TOUCH_KEEPALIVE) => {
                RemoteOp::Touch(rm.down)
            }
            None => return,
        };
        rm.in_flight = true;
        rm.last_sent = Some(Instant::now());
        let _ = self.tx.send(match op {
            RemoteOp::Touch(at) => {
                rm.down = at;
                Cmd::Touch(at)
            }
            RemoteOp::Power => Cmd::PowerKey,
        });
    }

    /// Queue a finger change from the mouse or a key. Moves in a row collapse
    /// into the newest; where it went down and where it went up always go.
    pub(crate) fn remote_input(&mut self, op: RemoteOp) {
        if !self.connected {
            return;
        }
        if let RemoteOp::Touch(at @ Some(_)) = op {
            if self.remote_finger() == at {
                return; // still the same place
            }
        }
        let rm = &mut self.preview.remote;
        rm.last_input = Some(Instant::now());
        if let RemoteOp::Touch(Some(_)) = op {
            let n = rm.queue.len();
            let last_is_move = n >= 1 && matches!(rm.queue[n - 1], RemoteOp::Touch(Some(_)));
            let prev_is_down =
                if n >= 2 { matches!(rm.queue[n - 2], RemoteOp::Touch(Some(_))) } else { n == 1 && rm.down.is_some() };
            if last_is_move && prev_is_down {
                rm.queue[n - 1] = op;
                return;
            }
        }
        rm.queue.push_back(op);
        self.pump_remote();
    }

    /// Where the finger is going to be once the queue is sent.
    pub(crate) fn remote_finger(&self) -> Option<(u32, u32)> {
        let rm = &self.preview.remote;
        rm.queue
            .iter()
            .rev()
            .find_map(|op| match op {
                RemoteOp::Touch(at) => Some(*at),
                RemoteOp::Power => None,
            })
            .unwrap_or(rm.down)
    }

    pub(crate) fn preview_panel(&mut self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            ui.strong("Display");
            ui.checkbox(&mut self.preview.live, "live");
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                if ui
                    .add_enabled(self.connected, egui::Button::new("⏻").small())
                    .on_hover_text("Power key, short press: back to the home menu (Home key)")
                    .clicked()
                {
                    self.remote_input(RemoteOp::Power);
                }
            });
        });
        ui.add_space(4.0);
        let [fw, fh] = self.preview.size;
        // Leave room for the frame's 6 px inner margin on both sides, or the panel
        // grows a little every frame.
        let w = (ui.available_width() - 12.0).clamp(80.0, fw as f32 * 2.0);
        let scale = w / fw as f32;
        let screen = egui::vec2(w, fh as f32 * scale);
        let strip_h = STRIP_H as f32 * scale;
        let frame_color = egui::Color32::from_rgb(0x10, 0x32, 0x4a);
        let can_touch = self.connected;
        let mut touch_resp = None;
        egui::Frame::new().fill(frame_color).inner_margin(6.0).corner_radius(4.0).show(ui, |ui| {
            ui.spacing_mut().item_spacing.y = 0.0;
            // The screen and the A/B/C strip below it: one touch area, as on the board.
            let sense = if can_touch { egui::Sense::click_and_drag() } else { egui::Sense::hover() };
            let (rect, resp) = ui.allocate_exact_size(egui::vec2(w, screen.y + strip_h), sense);
            let screen_rect = egui::Rect::from_min_size(rect.min, screen);
            let painter = ui.painter_at(rect);
            match (&self.preview.texture, self.preview.have_frame) {
                (Some(t), true) => {
                    let uv = egui::Rect::from_min_max(egui::pos2(0.0, 0.0), egui::pos2(1.0, 1.0));
                    painter.image(t.id(), screen_rect, uv, egui::Color32::WHITE);
                }
                _ => {
                    painter.rect_filled(screen_rect, 0.0, egui::Color32::from_rgb(0x0c, 0x11, 0x16));
                    painter.text(
                        screen_rect.center(),
                        egui::Align2::CENTER_CENTER,
                        if self.connected { "waiting for frame…" } else { "offline" },
                        egui::FontId::proportional(13.0),
                        egui::Color32::GRAY,
                    );
                }
            }
            // Strip zones as the firmware has them: A x < 130, B 130..199, C x >= 200.
            let finger = self.remote_finger();
            for (button, x0, x1) in [(0, 0u32, 130u32), (1, 130, 200), (2, 200, TOUCH_W)] {
                let zone = egui::Rect::from_min_max(
                    egui::pos2(rect.min.x + x0 as f32 * scale, screen_rect.max.y),
                    egui::pos2(rect.min.x + x1 as f32 * scale, rect.max.y),
                );
                let held = finger.is_some_and(|(x, y)| y >= TOUCH_H - STRIP_H && x >= x0 && x < x1);
                let fill = if held {
                    egui::Color32::from_rgb(0x2a, 0x5a, 0x7a)
                } else {
                    egui::Color32::from_rgb(0x16, 0x26, 0x33)
                };
                painter.rect_filled(zone.shrink(1.5), 3.0, fill);
                // The icons the SE has printed there (‹‹ ○ ››), drawn: no font needed.
                let (c, h) = (zone.center(), (zone.height() * 0.22).clamp(4.0, 9.0));
                let stroke = egui::Stroke::new(1.5, egui::Color32::GRAY);
                if button == 1 {
                    painter.circle_stroke(c, h, stroke);
                } else {
                    let dir = if button == 0 { -1.0 } else { 1.0 };
                    for off in [-0.55 * h, 0.55 * h] {
                        let x = c.x + off;
                        painter.add(egui::Shape::line(
                            vec![
                                egui::pos2(x - dir * 0.5 * h, c.y - h),
                                egui::pos2(x + dir * 0.5 * h, c.y),
                                egui::pos2(x - dir * 0.5 * h, c.y + h),
                            ],
                            stroke,
                        ));
                    }
                }
            }
            if let Some((x, y)) = finger.filter(|&(_, y)| y < TOUCH_H - STRIP_H) {
                let at = egui::pos2(rect.min.x + (x as f32 + 0.5) * scale, rect.min.y + (y as f32 + 0.5) * scale);
                painter.circle_stroke(
                    at,
                    7.0,
                    egui::Stroke::new(1.5, egui::Color32::from_rgba_unmultiplied(255, 255, 255, 160)),
                );
            }
            if resp.has_focus() {
                painter.rect_stroke(
                    rect,
                    2.0,
                    egui::Stroke::new(1.0, egui::Color32::from_rgb(0x5a, 0xa0, 0xd0)),
                    egui::StrokeKind::Outside,
                );
            }
            touch_resp = Some((resp, rect, scale));
        });
        if let Some((resp, rect, scale)) = touch_resp.filter(|_| can_touch) {
            let resp = resp.on_hover_text(
                "Click or drag to touch. With the display focused, the keys are the simulator's: Left or Up, \
                 Enter, and Right or Down (or A, B, C) hold the three buttons; P presses the power key.",
            );
            self.touch_input(ui, &resp, rect, scale);
        }
    }

    /// The mouse on the display is a finger; with the display focused, keys
    /// hold the touch buttons (A, B, C) and P presses the power key. Keys
    /// only count while it has focus, so typing in the editor never reaches
    /// the device.
    pub(crate) fn touch_input(&mut self, ui: &mut egui::Ui, resp: &egui::Response, rect: egui::Rect, scale: f32) {
        let to_device = |p: egui::Pos2| -> (u32, u32) {
            let x = ((p.x - rect.min.x) / scale).floor().clamp(0.0, (TOUCH_W - 1) as f32);
            let y = ((p.y - rect.min.y) / scale).floor().clamp(0.0, (TOUCH_H - 1) as f32);
            (x as u32, y as u32)
        };
        // The mouse, unless a key holds the finger already.
        if self.preview.remote.key.is_none() {
            match resp.interact_pointer_pos().filter(|_| resp.is_pointer_button_down_on()) {
                Some(p) => {
                    resp.request_focus();
                    self.preview.remote.mouse = true;
                    self.remote_input(RemoteOp::Touch(Some(to_device(p))));
                }
                None if self.preview.remote.mouse => {
                    self.preview.remote.mouse = false;
                    self.remote_input(RemoteOp::Touch(None));
                }
                None => {}
            }
        }
        if !resp.has_focus() {
            if self.preview.remote.key.take().is_some() {
                self.remote_input(RemoteOp::Touch(None));
            }
            return;
        }
        // Keep arrows and Escape here instead of moving the focus.
        ui.memory_mut(|m| {
            m.set_focus_lock_filter(
                resp.id,
                egui::EventFilter { tab: false, horizontal_arrows: true, vertical_arrows: true, escape: false },
            )
        });
        // The simulator window's keys (sim_board.c), so both work the same.
        let keys = [
            (egui::Key::A, 65u32),
            (egui::Key::ArrowLeft, 65),
            (egui::Key::ArrowUp, 65),
            (egui::Key::B, 165),
            (egui::Key::Enter, 165),
            (egui::Key::C, 260),
            (egui::Key::ArrowRight, 260),
            (egui::Key::ArrowDown, 260),
        ];
        let (pressed, released, power) = ui.input(|i| {
            let pressed = keys.iter().find(|(k, _)| i.key_pressed(*k) && !i.modifiers.any()).copied();
            let released: Vec<egui::Key> = keys.iter().filter(|(k, _)| i.key_released(*k)).map(|(k, _)| *k).collect();
            (pressed, released, i.key_pressed(egui::Key::P) && !i.modifiers.any())
        });
        if power {
            self.remote_input(RemoteOp::Power);
        }
        if let Some(k) = self.preview.remote.key {
            if released.contains(&k) {
                self.preview.remote.key = None;
                self.remote_input(RemoteOp::Touch(None));
            }
        } else if let Some((k, x)) = pressed {
            if self.remote_finger().is_none() {
                self.preview.remote.key = Some(k);
                self.preview.remote.mouse = false;
                self.remote_input(RemoteOp::Touch(Some((x, TOUCH_H - STRIP_H / 2))));
            }
        }
    }
}
