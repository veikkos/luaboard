#include "buttons.h"

#include "app_config.h"
#include "board.h"
#include "hw.h"
#include "sleep.h"

enum { Z_NONE, Z_SCREEN, Z_A, Z_B, Z_C };

/* The last reading acted on. */
static bool s_down;
static int s_rx, s_ry;
static uint32_t s_read_at;      /* when the controller was last read */
static bool s_read_once;

/* Readings taken during an app's frame, acted on once it's over (in order,
   each at its own time), so the frame sees one moment. */
#define KEPT_N 64
typedef struct { bool down; int x, y; uint32_t at; } kept_t;
static kept_t s_kept[KEPT_N];
static int s_nkept;
static bool s_in_frame;

/* The touch in progress (from finger down to finger up). */
static int s_zone;              /* where it went down; Z_NONE: no touch */
static uint32_t s_since;        /* when */
static int s_x0, s_y0;          /* where */
static int s_x, s_y;            /* where the finger is now */
static bool s_moved;            /* went further than TOUCH_SLOP_PX: a drag (or a cancelled press) */
static bool s_long_done;        /* LONG / HOLD / back sent: nothing on release */
static uint32_t s_next_repeat;
static bool s_ignore;           /* down before buttons_reset(): ignore until let go */
static bool s_up;               /* no finger since s_up_at (during a touch, or while ignoring) */
static uint32_t s_up_at;
static unsigned s_touches;      /* touches so far: the id of the one in progress */

static btn_sys_t s_sys;         /* for the next buttons_poll() */
static uint32_t s_key_at;       /* last power key poll */

/* Events, oldest first. */
#define QUEUE_N 16
static btn_event_t s_q[QUEUE_N];
static int s_qhead, s_qn;

static void push(btn_kind_t kind, char what)
{
    btn_event_t e = { .kind = kind, .what = what, .x = s_x, .y = s_y, .x0 = s_x0, .y0 = s_y0, .touch = s_touches };
    if (kind == BTN_TAP || kind == BTN_HOLD || kind == BTN_HOLD_UP) { e.x = s_x0; e.y = s_y0; }
    if (e.y >= BOARD_LCD_H) e.y = BOARD_LCD_H - 1;        /* a screen touch drifted onto the strip */
    if (kind == BTN_DRAG && what == 'm' && s_qn) {        /* the newest position is enough */
        btn_event_t *last = &s_q[(s_qhead + s_qn - 1) % QUEUE_N];
        if (last->kind == BTN_DRAG && last->what == 'm') { *last = e; return; }
    }
    if (s_qn == QUEUE_N) { s_qhead = (s_qhead + 1) % QUEUE_N; s_qn--; }   /* drop the oldest */
    s_q[(s_qhead + s_qn++) % QUEUE_N] = e;
}

bool buttons_next(btn_event_t *out)
{
    if (!s_qn) return false;
    *out = s_q[s_qhead];
    s_qhead = (s_qhead + 1) % QUEUE_N;
    s_qn--;
    return true;
}

/* Remote touch: readings from the PC, oldest first, and the one in effect. */
#define REMOTE_N 16
typedef struct { bool down; int x, y; } reading_t;
static reading_t s_rq[REMOTE_N];
static int s_rqhead, s_rqn;
static reading_t s_remote;      /* the last one taken */
static uint32_t s_remote_at;    /* when the PC last said something */

void buttons_remote(bool down, int x, int y)
{
    s_remote_at = hw_millis();
    reading_t r = { .down = down, .x = x, .y = y };
    if (down && s_rqn >= 2) {                   /* a move after a move: the newest position is enough */
        reading_t *last = &s_rq[(s_rqhead + s_rqn - 1) % REMOTE_N], *prev = &s_rq[(s_rqhead + s_rqn - 2) % REMOTE_N];
        if (last->down && prev->down) { *last = r; return; }
    }
    if (s_rqn == REMOTE_N) { s_rqhead = (s_rqhead + 1) % REMOTE_N; s_rqn--; }   /* drop the oldest */
    s_rq[(s_rqhead + s_rqn++) % REMOTE_N] = r;
}

void buttons_remote_power(void)
{
    if (!sleep_activity(hw_millis())) s_sys = BTN_SYS_HOME;   /* the screen was off: it only wakes it */
}

/* The remote finger at this reading: the next queued reading, else the last
   one, let go once the PC has been quiet for REMOTE_TOUCH_MS. */
static bool remote_read(uint32_t now, int *x, int *y)
{
    if (s_rqn) {
        s_remote = s_rq[s_rqhead];
        s_rqhead = (s_rqhead + 1) % REMOTE_N;
        s_rqn--;
    } else if (s_remote.down && now - s_remote_at >= REMOTE_TOUCH_MS) {
        s_remote.down = false;
    }
    if (!s_remote.down) return false;
    *x = s_remote.x;
    *y = s_remote.y;
    return true;
}

static int zone_at(int x, int y)
{
    if (y < BOARD_LCD_H) return Z_SCREEN;
    return x < BOARD_BTN_B_X0 ? Z_A : x < BOARD_BTN_C_X0 ? Z_B : Z_C;
}

static char button_of(int z) { return z == Z_A ? 'A' : z == Z_B ? 'B' : 'C'; }

/* Read the controller (or the remote finger) now. */
static kept_t read_touch(uint32_t now)
{
    kept_t r = { .at = now };
    r.down = remote_read(now, &r.x, &r.y) || hw_touch_read(&r.x, &r.y);
    s_read_at = now;
    s_read_once = true;
    return r;
}

/* During a frame: keep a reading for later. One that says nothing new isn't
   kept (the times that matter are where something changed); when full, a
   move replaces the move before it, so no finger down or up is lost. */
static void keep(kept_t r)
{
    if (s_nkept) {
        kept_t *last = &s_kept[s_nkept - 1];
        if (last->down == r.down && (!r.down || (last->x == r.x && last->y == r.y))) return;
        if (s_nkept == KEPT_N) {
            if (last->down && r.down) *last = r;
            return;
        }
    }
    s_kept[s_nkept++] = r;
}

/* No finger at this reading: true once there has been none for TOUCH_UP_MS
   (a reading or two without one can be a glitch in the middle of a touch). */
static bool let_go(uint32_t now)
{
    if (!s_up) { s_up = true; s_up_at = now; }
    return now - s_up_at >= TOUCH_UP_MS;
}

void buttons_reset(void)
{
    s_qn = 0;
    s_sys = BTN_SYS_NONE;
    s_nkept = 0;
    kept_t r = read_touch(hw_millis());
    s_down = r.down;
    if (r.down) { s_rx = r.x; s_ry = r.y; }
    s_ignore = s_down;
    s_zone = Z_NONE;
    s_up = false;
}

void buttons_init(void) { buttons_reset(); }

void buttons_ignore_touch(void)
{
    if (s_zone == Z_NONE) return;
    s_ignore = true;
    s_zone = Z_NONE;
    s_up = false;
}

bool buttons_pressing(int *x, int *y)
{
    if (s_ignore || s_zone != Z_SCREEN || s_moved) return false;
    *x = s_x0;
    *y = s_y0;
    return true;
}

/* Both from the touch in progress, not the raw reading: it lasts until the
   finger has been gone TOUCH_UP_MS, the moment its event is queued, so the
   two never disagree. */
bool buttons_touch(int *x, int *y)
{
    if (s_ignore || s_zone != Z_SCREEN) return false;
    *x = s_x;
    *y = s_y < BOARD_LCD_H ? s_y : BOARD_LCD_H - 1;   /* drifted onto the strip */
    return true;
}

char buttons_held(void)
{
    if (s_ignore || s_zone == Z_NONE || s_zone == Z_SCREEN || s_moved) return 0;   /* moved: slid off, cancelled */
    return button_of(s_zone);
}

/* Held BUTTON_LONG_MS: a hold on the screen, a long press of A/C, back for B. */
static void went_long(uint32_t now)
{
    s_long_done = true;
    s_next_repeat = now + BUTTON_REPEAT_EVERY;
    if (s_zone == Z_SCREEN) push(BTN_HOLD, 0);
    else if (s_zone == Z_B) s_sys = BTN_SYS_BACK;   /* the system's: leave the app / back */
    else push(BTN_LONG, button_of(s_zone));
}

/* The touch ended at `up` (when the finger was last seen gone): what it
   was, unless it already counted as a long one. A touch that lasted long
   enough is a long one even when no reading saw it held that long. */
static void released(uint32_t up)
{
    if (!s_long_done && !s_moved && up - s_since >= BUTTON_LONG_MS) went_long(up);
    if (s_zone != Z_SCREEN) {
        if (!s_long_done && !s_moved) push(BTN_PRESS, button_of(s_zone));
        return;
    }
    if (s_moved) {                              /* a drag: and a swipe if let go away from its start */
        push(BTN_DRAG, 'e');
        int dx = s_x - s_x0, dy = s_y - s_y0;
        int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
        if (dx * dx + dy * dy > TOUCH_SLOP_PX * TOUCH_SLOP_PX)
            push(BTN_SWIPE, adx > ady ? (dx < 0 ? 'l' : 'r') : (dy < 0 ? 'u' : 'd'));
    } else {
        push(s_long_done ? BTN_HOLD_UP : BTN_TAP, 0);
    }
}

/* Move the touch along by one reading, taken at `now`. */
static void step(kept_t r)
{
    uint32_t now = r.at;
    s_down = r.down;
    if (r.down) { s_rx = r.x; s_ry = r.y; }

    /* A finger after one gone TOUCH_UP_MS is a new touch, even with no
       reading in that time saying so (kept readings skip repeats). */
    if (s_down && s_up && now - s_up_at >= TOUCH_UP_MS) {
        if (s_ignore) s_ignore = false;
        else if (s_zone != Z_NONE) { released(s_up_at); s_zone = Z_NONE; }
        s_up = false;
    }
    if (s_ignore) {                             /* until this finger is let go */
        if (s_down) s_up = false;
        else if (let_go(now)) { s_ignore = false; s_up = false; }
        return;
    }
    if (!s_down) {
        if (s_zone == Z_NONE || !let_go(now)) return;
        released(s_up_at);
        s_zone = Z_NONE;
        s_up = false;
        return;
    }
    s_up = false;
    if (s_zone == Z_NONE) {                     /* a new touch */
        s_zone = zone_at(s_rx, s_ry);
        s_touches++;
        s_since = now;
        s_x = s_x0 = s_rx;
        s_y = s_y0 = s_ry;
        s_moved = false;
        s_long_done = false;
        return;
    }

    bool moved_now = s_rx != s_x || s_ry != s_y;
    s_x = s_rx;
    s_y = s_ry;
    int dx = s_x - s_x0, dy = s_y - s_y0;
    if (s_zone == Z_SCREEN) {
        if (dx * dx + dy * dy > TOUCH_SLOP_PX * TOUCH_SLOP_PX) s_moved = true;
        if (s_moved && moved_now) push(BTN_DRAG, 'm');
    } else if (zone_at(s_x, s_y) != s_zone) {
        s_moved = true;                         /* slid off the button: cancelled */
    }
    if (s_moved && s_zone != Z_SCREEN) return;

    if (!s_long_done && !s_moved && now - s_since >= BUTTON_LONG_MS) {
        went_long(now);
        return;
    }
    if (s_long_done && (s_zone == Z_A || s_zone == Z_C) && (int32_t)(now - s_next_repeat) >= 0) {
        s_next_repeat = now + BUTTON_REPEAT_EVERY;
        push(BTN_REPEAT, button_of(s_zone));
    }
}

void buttons_sample(void)
{
    if (!s_in_frame && s_nkept) {               /* the last frame's readings, in order */
        for (int i = 0; i < s_nkept; i++) step(s_kept[i]);
        s_nkept = 0;
    }
    uint32_t now = hw_millis();
    if (s_read_once && now - s_read_at < TOUCH_READ_MS) return;
    if (now - s_key_at >= POWER_KEY_POLL_MS) {
        s_key_at = now;
        int key = hw_power_key();
        if (key == 2 && !sleep_active()) sleep_now();         /* long press: screen off */
        else if (key) {
            bool woke = sleep_activity(now);                  /* the screen was off: it only wakes it */
            if (!woke && key == 1) s_sys = BTN_SYS_HOME;
        }
    }
    kept_t r = read_touch(now);
    if (r.down && sleep_activity(now)) {        /* woke the screen: this touch is ignored until let go */
        s_nkept = 0;
        s_down = true;
        s_ignore = true;
        s_zone = Z_NONE;
        s_up = false;
        return;
    }
    sleep_tick(now);
    if (sleep_active()) return;                 /* dark: nothing to act on (no finger yet) */
    if (s_in_frame) keep(r);
    else step(r);
}

void buttons_frame_begin(void)
{
    buttons_sample();
    s_in_frame = true;
}

void buttons_frame_end(void) { s_in_frame = false; }

btn_sys_t buttons_poll(void)
{
    buttons_sample();
    btn_sys_t s = s_sys;
    s_sys = BTN_SYS_NONE;
    return s;
}
