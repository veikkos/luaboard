/*
 * Touch input: one state machine with one set of timings (app_config.h),
 * and one stream of events for whoever has the screen: the home menu (and
 * the error screen) while no app runs, the app (touch.on()) while one does.
 *
 * A touch is one gesture from finger down to finger up, decided by where it
 * went down: on the button strip it is a press of A, B or C, on the screen a
 * tap, hold or drag, wherever the finger drifts meanwhile. A screen touch
 * that stays within TOUCH_SLOP_PX of where it went down is a tap or a hold;
 * one that goes further is a drag, never a tap.
 *
 *   PRESS  A/B/C     let go before BUTTON_LONG_MS. Sliding off the button
 *                    cancels it.
 *   LONG   A/C       held BUTTON_LONG_MS; no PRESS follows
 *   REPEAT A/C       still held: every BUTTON_REPEAT_EVERY after the LONG
 *   TAP              a short touch on the screen, at where it went down
 *   HOLD             a screen touch held in place BUTTON_LONG_MS
 *   HOLD_UP          ... and then let go without sliding (no TAP follows a
 *                    HOLD; this is for buttons with nothing to do on a hold)
 *   DRAG   m/e       a screen touch that went further: at each new
 *                    position ('m'), and once more where it was let go ('e')
 *   SWIPE  u/d/l/r   after the DRAG 'e' of a drag let go further than
 *                    TOUCH_SLOP_PX from where it went down; x0, y0 there
 *
 * The controller is read at most every TOUCH_READ_MS, from everywhere that
 * can run for long (the main loop, the Lua hook, sys.delay), so the timings
 * hold during a slow app frame too. During an app's frame the readings are
 * only kept: they're acted on, in order and each at its own time, once the
 * frame is over. So a frame sees one moment: what's held, where the finger
 * is and the events it gets don't change under it, and agree. Whether a touch was long is decided by
 * its length, also when no reading fell between BUTTON_LONG_MS and the
 * release. It ends after TOUCH_UP_MS without a finger, so a bad reading
 * doesn't split it in two.
 *
 * Holding B and the power key aren't in the stream: they belong to the
 * system (leave the app, back, home) and come back from buttons_poll().
 * A long press of the power key turns the screen off (sleep.h); while it's
 * off, the touch or key that wakes it does only that.
 *
 * A PC can touch too (the TOUCH and KEY commands, for remote use): its
 * readings go through the same state machine, one per TOUCH_READ_MS, so a
 * tap whose down and up arrive together is still a tap. While its finger is
 * down it stands in for the real one. A remote finger not repeated for
 * REMOTE_TOUCH_MS is let go, so a PC that goes away doesn't leave one down.
 */
#pragma once

#include <stdbool.h>

typedef enum { BTN_PRESS, BTN_LONG, BTN_REPEAT, BTN_TAP, BTN_HOLD, BTN_HOLD_UP, BTN_SWIPE, BTN_DRAG } btn_kind_t;

typedef struct {
    btn_kind_t kind;
    char what;          /* PRESS, LONG, REPEAT: 'A' 'B' 'C'; SWIPE: 'u' 'd' 'l' 'r'; DRAG: 'm' 'e' */
    int x, y;           /* screen events: where the finger is (TAP, HOLD, HOLD_UP: where it went down) */
    int x0, y0;         /* screen events: where it went down */
    unsigned touch;     /* which touch (counts up): events of one gesture share it */
} btn_event_t;

typedef enum { BTN_SYS_NONE, BTN_SYS_BACK, BTN_SYS_HOME } btn_sys_t;

/** At boot: ignore whatever is touched now (e.g. the finger that asked for
 *  safe mode) until it's let go. */
void buttons_init(void);
/** The screen changes hands (an app starts or stops): forget queued events
 *  and ignore the touch that is down now until it's let go. */
void buttons_reset(void);
/** The touch in progress has done its job (e.g. a HOLD opened something):
 *  nothing more from it until it's let go. */
void buttons_ignore_touch(void);
/** buttons_sample(), then a system event (B held: back; power key: home)
 *  seen by this or an earlier sample since the last call. */
btn_sys_t buttons_poll(void);
/** Read the touch controller and queue what happened, if TOUCH_READ_MS
 *  has passed since the last reading (else nothing: cheap to call often).
 *  During a frame the reading is only kept for after it. */
void buttons_sample(void);
/** An app's frame starts: act on what was read so far, then keep the
 *  readings until buttons_frame_end(). */
void buttons_frame_begin(void);
void buttons_frame_end(void);
/** The next event, oldest first; false if none. */
bool buttons_next(btn_event_t *out);
/** A screen touch held in place (down, not slid, not ignored): true, with
 *  where it went down. For showing what is pressed. */
bool buttons_pressing(int *x, int *y);
/** A screen touch in progress (not ignored): true, with where the finger
 *  is (on the screen's edge if it drifted onto the strip). Like the events,
 *  it lasts until the finger has been gone TOUCH_UP_MS. */
bool buttons_touch(int *x, int *y);
/** 'A', 'B' or 'C' while a touch that went down on that button is in
 *  progress (not slid off it, not ignored), else 0. Ends with its event. */
char buttons_held(void);
/** A remote finger: down at (x, y) in the touch area (y >= LCD height: the
 *  button strip), or let go. Queued for the next readings. */
void buttons_remote(bool down, int x, int y);
/** A remote short press of the power key (home). */
void buttons_remote_power(void);
