//! The computer gives up on the USB port over a restart of the chip, as seen
//! on a real board: the port is still there, but nothing comes or goes. The
//! firmware notices that USB power is there with no computer heard, and has
//! the computer start on the port anew.

use crate::harness::T;

pub fn run(t: &mut T) {
    t.say("== USB: the computer stopped talking to the port over a restart");
    t.wipe();
    t.boot_with(&["--set", "usb_host=gone"]);
    let gone = t.ask("sensor=usbhost:off");
    t.advance(2000);
    let still = t.ask("sensor=usbhost:off");
    t.check(gone && still, "nothing is done about it in the first seconds", |_| format!("gone {gone}, at 2 s {still}"));
    t.advance(1500);
    let back = t.ask("sensor=usbhost:on");
    let pong = t.e(&["ping"]);
    t.check(
        back && pong.contains("PONG"),
        "then the computer is made to start on the port again, and the link works",
        |_| format!("talking {back}, ping: {pong}"),
    );
    t.stop();

    // On a charger nobody ever answers: two tries, then it's left alone.
    t.boot_with(&["--set", "usb_host=gone"]);
    t.advance(3400);
    t.ctl(&["usb_host=gone"]);
    t.advance(3400);
    let second = t.ask("sensor=usbhost:on");
    t.ctl(&["usb_host=gone"]);
    t.advance(8000);
    let third = t.ask("sensor=usbhost:off");
    t.check(second && third, "a port that stays quiet is tried twice, no more", |_| {
        format!("second {second}, left alone {third}")
    });
    t.stop();
}
