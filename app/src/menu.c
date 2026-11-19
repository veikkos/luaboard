#include "menu.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "app_config.h"
#include "app_update.h"
#include "appdata.h"
#include "appmeta.h"
#include "sound.h"
#include "board.h"
#include "buttons.h"
#include "clock.h"
#include "gfx.h"
#include "hw.h"
#include "net.h"
#include "ota.h"
#include "protocol.h"
#include "screens.h"
#include "script.h"
#include "settings.h"
#include "text.h"
#include "sleep.h"
#include "update.h"
#include "vfs.h"
#include "esp_log.h"
#include "qrcodegen.h"

static const char *TAG = "menu";

/* The screens. What each one has (title, drawing, events, rows) is a switch
   over view_t with a case for every view and no default, so the compiler
   names a view a switch leaves out. */
typedef enum { V_HOME, V_SETTINGS, V_OPTIONS, V_PICK, V_UPDATE, V_APPUPD, V_STORE, V_STORE_APP, V_APP } view_t;
/* What a touch event means to the menu (menu_tick turns buttons.c's events into these). */
typedef enum { M_NONE, M_UP, M_DOWN, M_OK, M_BACK, M_HOME, M_TAP, M_HOLD,
               M_DRAG, M_DRAG_END } act_t;
/* Settings: what can be changed, then what there is to know (not selectable
   with the buttons), then Back. */
enum { S_BRIGHTNESS, S_VOLUME, S_SCREEN_OFF, S_TURN_OFF, S_SLEEP, S_AUTOSTART, S_UPDATES, S_CHECK, S_TIME, S_VERSION, S_STORAGE, S_WIFI, S_BACK, S_COUNT };
#define S_INFO(i) ((i) >= S_VERSION && (i) <= S_WIFI)
static const int s_off_choices[] = { 0, 1, 5, 15, 30, 60 };   /* Sleep after: minutes, 0 never */
#define OFF_N ((int)(sizeof s_off_choices / sizeof s_off_choices[0]))
static const int s_turn_choices[] = { 0, 5, 15, 30, 60 };     /* Turn off after: minutes asleep, 0 never */
#define TURN_N ((int)(sizeof s_turn_choices / sizeof s_turn_choices[0]))
enum { O_OPEN, O_AUTOSTART, O_CLEAR, O_UNINSTALL, O_BACK, O_COUNT };   /* app options (hold B on an app) */
enum { U_INSTALL, U_BACK, U_COUNT };                              /* the firmware update's confirm screen */

/* ------------------------------------------------------------ layout
   Every screen of the menu is laid out from these: one row height, one
   margin, one line height. Change one and all the screens follow. The e2e
   tests tap rows by the same numbers (pc-tool/tests/e2e/harness.rs, `ui`).
   The app options screen is the tightest: its info has to fit below five
   rows (the e2e tests check that it does), and so does the QR code on a
   Marketplace app's screen. */
#define HEAD_H     30       /* the header: title, time, Wi-Fi, battery */
#define FOOT_H     22       /* the hints above the buttons */
#define ROW_H      26       /* a list row, on every screen */
#define ROW_GAP    2        /* between two rows' backgrounds */
#define INSET      4        /* a row's background from the screen's edges */
#define PAD        14       /* text from the screen's edges, in rows, below them and in the header */
#define LINE_H     12       /* a line of small text */
#define GAP        8        /* between a list and the text below it */
#define PARA       4        /* between paragraphs of that text */
#define DIVIDER_H  4        /* home: the room for the line above the fixed rows */

#define FONT_H     7                                 /* the 5x7 font at scale 1 ... */
#define CHAR_W     6                                 /* ... and a character with its space */
#define LIST_Y     (HEAD_H + 2)                      /* a list's first row */
#define FOOT_Y     (BOARD_LCD_H - FOOT_H)
#define ROW_BG     (ROW_H - ROW_GAP)                 /* a row's background, its height */
#define ROW_TEXT(y)  ((y) + (ROW_BG - 2 * FONT_H) / 2)    /* a row's label (scale 2), its top */
#define ROW_SMALL(y) ((y) + (ROW_BG - FONT_H + 1) / 2)    /* a row's value (scale 1), its top */
#define ROWS       ((FOOT_Y - LIST_Y) / ROW_H)       /* the rows a screen shows */
#define HOME_ROWS  ((FOOT_Y - LIST_Y - DIVIDER_H) / ROW_H)   /* ... home, with its divider */
#define BELOW(n)   (LIST_Y + (n) * ROW_H + GAP)     /* the text under a list of n rows */

static view_t s_view = V_APP;
/* The home list's apps, sorted by the name shown. */
typedef struct {
    vfs_app_t app;
    char title[sizeof ((app_meta_t *)0)->title];   /* from app.json, "" = none */
    int lua_api;                                   /* from app.json, as app_meta_t has it */
    bool api_bad;                                  /* asks for a Lua API this firmware doesn't run */
} menu_app_t;
static menu_app_t s_apps[VFS_MAX_APPS];
static int s_napps;
static app_meta_t s_ometa;             /* options: the app's metadata */
static int s_sel, s_top;               /* home: selected row, first visible row */
static int s_ssel, s_stop;             /* settings: selected row, first visible row */
static bool s_editing;                 /* settings: changing a bar (brightness, volume) with the buttons */
static bool s_dragging;                /* settings: ... or with a finger along the bar */
static int s_edit_val;                 /* ... the value meanwhile */
static int s_pick[VFS_MAX_APPS + 2];   /* autostart picker: -1 off, app indexes, PICK_BACK */
static int s_npick, s_psel, s_ptop;
#define PICK_BACK (-2)
#define BAR_X     160                  /* settings: the brightness and volume bars */
#define BAR_W     100
#define BAR_H     10
static char s_oapp[VFS_APP_NAME_MAX];  /* options: the app */
static int s_orows[O_COUNT], s_norows, s_osel;
static bool s_armed;                   /* options: Clear data / Uninstall pressed once, waiting for the confirm */
static bool s_checking;                /* Settings > Check now: waiting for the checks */
static uint32_t s_sync_until;          /* Settings > Time: waiting for the time server until */
static unsigned s_sync_from;           /* ...and the sync count when we asked */
static uint32_t s_gen;                 /* vfs generation the list was built from */
static uint32_t s_header_at;           /* last header refresh */
static int s_press = -1;               /* the row under a finger (as the view counts rows), -1 none */
static view_t s_press_view;            /* ... in this view */
static char s_toast[64];
static uint32_t s_toast_until;
static char s_upd_ver[sizeof ((update_manifest_t *)0)->version];   /* home: the release its update row offers, "" none */
static int s_usel;                     /* update: the selected row */
static int s_appupd_n;                 /* home: the apps its App updates row offers, 0 none */
static int s_asel, s_atop;             /* app updates: selected row, first shown */
/* App updates being installed, one after the other (the App updates screen). */
static char s_queue[VFS_MAX_APPS][VFS_APP_NAME_MAX];
static int s_qn, s_qi, s_qok, s_qfail;
static bool s_updating;
static int s_shown_pct = -1;           /* ... what the progress screen shows */
static int s_shown_i = -1;
/* Apps whose update failed since the App updates screen was opened, and why. */
static struct { char app[VFS_APP_NAME_MAX]; char why[40]; } s_failed[VFS_MAX_APPS];
static int s_nfailed;
static view_t s_after = V_APPUPD;      /* where the screen goes when the installs are done */
/* Marketplace: the index's apps by title; the app whose screen is open. */
static int s_store[APPS_INDEX_MAX_APPS];
static int s_nstore, s_tsel, s_ttop;
static const char *s_store_text;       /* the index text the list was built from */
static bool s_store_checking;          /* ... and whether a check was under way */
static char s_sapp[VFS_APP_NAME_MAX];
static int s_dsel;                     /* its rows: the action, Back */
static char s_picked[VFS_APP_NAME_MAX];   /* installed from the store: selected on the way home */

#define C_BG       gfx_rgb(12, 18, 28)
#define C_HEAD     gfx_rgb(20, 30, 46)
#define C_ACCENT   gfx_rgb(80, 180, 255)
#define C_SEL      gfx_rgb(36, 84, 140)
#define C_PRESS    gfx_rgb(88, 96, 110)     /* a row under a finger, before it's let go */
#define C_TEXT     gfx_rgb(235, 238, 242)
#define C_DIM      gfx_rgb(130, 142, 158)
#define C_WARN     gfx_rgb(255, 190, 90)
#define C_DANGER   gfx_rgb(150, 40, 40)
#define C_KEY      gfx_rgb(255, 120, 80)    /* the orange of the printed button marks */
#define C_TINT     gfx_rgb(24, 44, 66)      /* the home's Marketplace row: an action, not an app */

/* ------------------------------------------------------------ drawing */

static void text_right(int xr, int y, const char *s, gfx_color_t c, int scale)
{
    gfx_text(xr - gfx_text_width(s, scale), y, s, c, scale, NULL);
}

static void text_center(int xc, int y, const char *s, gfx_color_t c, int scale)
{
    gfx_text(xc - gfx_text_width(s, scale) / 2, y, s, c, scale, NULL);
}

/* `s` cut (ending in "..") to at most `maxw` pixels at `scale`. */
static void fit(char *out, size_t cap, const char *s, int maxw, int scale)
{
    snprintf(out, cap, "%s", s);
    if (gfx_text_width(out, scale) <= maxw) return;
    for (size_t n = strlen(out); n > 0; n--) {
        if (n > cap - 3 || ((unsigned char)s[n] & 0xC0) == 0x80) continue;   /* room for "..", whole characters */
        snprintf(out + n, 3, "..");
        if (gfx_text_width(out, scale) <= maxw) return;
        snprintf(out, cap, "%s", s);
    }
}

/* ------------------------------------------------- status bar icons */

#define ICON_MID (HEAD_H / 2)                  /* the icons' (and texts') vertical middle in the header */

/* Wi-Fi as phones show it: a dot and three arcs, lit by signal strength
   (bars 0..3; the unlit ones dim). bars < 0: it can't join the network:
   all dim, with a small red cross. Returns its width. */
static int draw_wifi_icon(int x, int bars)
{
    const int cx = x + 7, by = ICON_MID + 5;   /* the dot, bottom middle */
    for (int dy = 0; dy <= 11; dy++) {
        for (int dx = -8; dx <= 8; dx++) {
            int ady = dy, adx = dx < 0 ? -dx : dx;
            if (adx > ady) continue;           /* a 90-degree fan, pointing up */
            int r2 = dx * dx + dy * dy, level;
            if (r2 <= 2) level = 0;                                /* the dot */
            else if (r2 >= 12 && r2 <= 25) level = 1;
            else if (r2 >= 42 && r2 <= 64) level = 2;
            else if (r2 >= 90 && r2 <= 121) level = 3;
            else continue;
            gfx_pixel(cx + dx, by - dy, level <= bars ? C_TEXT : gfx_rgb(60, 72, 90));
        }
    }
    if (bars < 0) {                            /* the cross, at the fan's lower right */
        const gfx_color_t red = gfx_rgb(235, 80, 70);
        for (int i = 0; i < 5; i++) {
            gfx_pixel(cx + 4 + i, by - 4 + i, red);
            gfx_pixel(cx + 8 - i, by - 4 + i, red);
            gfx_pixel(cx + 5 + i, by - 4 + i, red);  /* two pixels wide, so it reads at a glance */
            gfx_pixel(cx + 9 - i, by - 4 + i, red);
        }
    }
    return 17;
}

/* Battery: an outline with a nub, filled to the level (green while
   charging, red when low), with a bolt while charging. Returns its width. */
static int draw_battery_icon(int x, int pct, bool charging)
{
    const int w = 22, h = 11, y = ICON_MID - h / 2;
    gfx_color_t fill = charging ? gfx_rgb(80, 200, 120) : pct <= 15 ? gfx_rgb(235, 80, 70) : C_TEXT;
    gfx_rect(x, y, w, h, C_DIM, false);
    gfx_fill_rect(x + w, y + 3, 2, h - 6, C_DIM);                  /* the nub */
    int inner = (w - 4) * (pct < 0 ? 0 : pct > 100 ? 100 : pct) / 100;
    if (pct > 0 && inner < 1) inner = 1;
    gfx_fill_rect(x + 2, y + 2, inner, h - 4, fill);
    if (charging) {                                                  /* a bolt across the middle */
        int bx = x + w / 2, by = y + 1;
        gfx_line(bx + 2, by, bx - 2, by + 4, C_TEXT);      /* inside the outline */
        gfx_line(bx - 2, by + 4, bx + 2, by + 4, C_TEXT);
        gfx_line(bx + 2, by + 4, bx - 2, by + 8, C_TEXT);
    }
    return w + 2;
}

static const char *view_title(void);

/* The view's title on the left; on the right, like a phone's status bar: the
   time, Wi-Fi (while it's on) and the battery with its level. */
static void draw_header(void)
{
    int w = gfx_width();
    gfx_fill_rect(0, 0, w, HEAD_H, C_HEAD);
    gfx_fill_rect(0, HEAD_H - 2, w, 2, C_ACCENT);

    int batt = hw_battery_percent();
    const char *wifi = net_state();
    int bars = -9;                             /* -9: no icon (Wi-Fi off) */
    if (!strcmp(wifi, "connected")) {
        int rssi = net_rssi();
        bars = rssi >= -60 ? 3 : rssi >= -70 ? 2 : rssi >= -80 ? 1 : 0;
    } else if (!strcmp(wifi, "connecting")) {
        bars = net_wifi_failing() ? -1 : 0;    /* a try failed: the cross; else joining: the dot */
    }
    char hm[8] = "", pct[16] = "";
    if (clock_valid()) {
        struct tm tm;
        clock_local(clock_now(), &tm, NULL);
        snprintf(hm, sizeof hm, "%02d:%02d", tm.tm_hour, tm.tm_min);
    }
    if (batt >= 0) snprintf(pct, sizeof pct, "%d%%", batt);

    /* Laid out from the right edge: battery %, battery, Wi-Fi, time. */
    const int gap = 8, ty = ICON_MID - FONT_H / 2;
    int x = w - PAD;
    if (batt >= 0) {
        x -= gfx_text_width(pct, 1);
        gfx_text(x, ty, pct, C_TEXT, 1, NULL);
        x -= 4 + 24;
        draw_battery_icon(x, batt, hw_charging());
        x -= gap;
    }
    if (bars > -9) {
        x -= 17;
        draw_wifi_icon(x, bars);
        x -= gap;
    }
    if (*hm) {
        x -= gfx_text_width(hm, 1);
        gfx_text(x, ty, hm, C_TEXT, 1, NULL);
        x -= gap;
    }
    char t[48];
    fit(t, sizeof t, view_title(), x - PAD - 6, 2);
    gfx_text(PAD, ICON_MID - FONT_H, t, C_TEXT, 2, NULL);
    s_header_at = hw_millis();
}

/* A list's Back row: a left chevron and "Back", in the text colour like the
   rows that do something (dim is for information you can't select). The
   row's top is at y. */
static void back_label(int y)
{
    int x = PAD, ty = ROW_TEXT(y), cy = ty + FONT_H;
    for (int t = 0; t < 2; t++) {
        gfx_line(x + 7 + t, cy - 6, x + 1 + t, cy, C_TEXT);
        gfx_line(x + 1 + t, cy, x + 7 + t, cy + 6, C_TEXT);
    }
    gfx_text(x + 16, ty, "Back", C_TEXT, 2, NULL);
}

/* Hints above the three buttons: A (left), B (middle), C (right). */
static void draw_footer(const char *a, const char *b, const char *c)
{
    int w = gfx_width(), ty = FOOT_Y + (FOOT_H - FONT_H) / 2;
    gfx_fill_rect(0, FOOT_Y, w, FOOT_H, C_BG);
    gfx_fill_rect(0, FOOT_Y, w, 1, gfx_rgb(40, 52, 68));
    if (s_toast_until && (int32_t)(hw_millis() - s_toast_until) < 0) {
        text_center(w / 2, ty, s_toast, C_WARN, 1);
        return;
    }
    text_center(w / 6, ty, a ? a : "", C_KEY, 1);
    text_center(w / 2, ty, b ? b : "", C_KEY, 1);
    text_center(w * 5 / 6, ty, c ? c : "", C_KEY, 1);
}

/* A row's label on the left and its value (small, dim or coloured) on the
   right, the label cut so that the two don't meet. */
static void row_label(int y, const char *label, gfx_color_t c)
{
    gfx_text(PAD, ROW_TEXT(y), label, c, 2, NULL);
}

static void row_value(int y, const char *value, gfx_color_t c)
{
    text_right(gfx_width() - PAD, ROW_SMALL(y), value, c, 1);
}

/* A list row's background: gray while pressed, else `selc` if selected. */
static void row_bg(int i, int y, bool sel, gfx_color_t selc)
{
    bool pressed = s_press == i && s_press_view == s_view;
    if (pressed || sel) gfx_fill_rect(INSET, y, gfx_width() - 2 * INSET, ROW_BG, pressed ? C_PRESS : selc);
}

/* A list under the header: rows top..top+rows-1 of n, each drawn by `text`
   over its background, with scroll marks when there are more. */
typedef struct {
    int n, rows, top;
    int sel;                    /* the selected row, -1 none */
    gfx_color_t selc;           /* ... its background */
    void (*text)(int i, int y, bool sel);
} list_t;

/* Scroll so that sel (if in the list) is shown, and no further than the end. */
static void list_scroll(int *top, int sel, int n, int rows)
{
    if (sel >= 0 && sel < n) {
        if (sel < *top) *top = sel;
        if (sel >= *top + rows) *top = sel - rows + 1;
    }
    if (*top > n - rows) *top = n > rows ? n - rows : 0;
}

static void list_marks(const list_t *l)
{
    int w = gfx_width();
    if (l->top > 0) text_right(w - INSET, LIST_Y, "^", C_DIM, 1);
    if (l->top + l->rows < l->n) text_right(w - INSET, LIST_Y + l->rows * ROW_H - ROW_GAP - FONT_H, "v", C_DIM, 1);
}

static void list_row(const list_t *l, int i)
{
    int r = i - l->top;
    if (r < 0 || r >= l->rows || i >= l->n) return;
    int y = LIST_Y + r * ROW_H;
    row_bg(i, y, i == l->sel, l->selc);
    l->text(i, y, i == l->sel);
}

static void list_draw(const list_t *l)
{
    for (int i = l->top; i < l->top + l->rows && i < l->n; i++) list_row(l, i);
    list_marks(l);
}

/* One row again (pressed or not any more), over what was there. */
static void list_redraw_row(const list_t *l, int i)
{
    int r = i - l->top;
    if (r < 0 || r >= l->rows || i >= l->n) return;
    gfx_fill_rect(0, LIST_Y + r * ROW_H, gfx_width(), ROW_BG, C_BG);
    list_row(l, i);
    list_marks(l);
}

/* The row (index into the list) at y, -1 for none. */
static int list_row_at(list_t l, int y)
{
    if (y < LIST_Y) return -1;
    int r = (y - LIST_Y) / ROW_H;
    return r < l.rows && l.top + r < l.n ? l.top + r : -1;
}

/* Lists: scroll n rows (`rows` of them shown) with a finger dragging, a
   row for every ROW_H it has moved since it went down; the selection (if
   sel) stays in view. False if nothing moved. */
static unsigned s_scroll_touch;        /* the drag being followed */
static int s_scroll_top0;              /* ... and the first row shown when it started */

static bool list_drag(int *top, int *sel, int n, int rows, const btn_event_t *e)
{
    if (e->touch != s_scroll_touch) { s_scroll_touch = e->touch; s_scroll_top0 = *top; }
    int d = e->y0 - e->y;                                     /* up: further down the list */
    int steps = d >= 0 ? (d + ROW_H / 2) / ROW_H : -((-d + ROW_H / 2) / ROW_H);
    int t = s_scroll_top0 + steps;
    if (t > n - rows) t = n - rows;
    if (t < 0) t = 0;
    if (t == *top) return false;
    *top = t;
    if (sel && *sel < t) *sel = t;
    if (sel && *sel >= t + rows) *sel = t + rows - 1;
    return true;
}

/* Home: the apps, then the fixed rows below a divider: App updates (when
   installed apps have updates), the firmware's update row (when a newer
   release is known), and Settings, at the bottom. The apps scroll above. */
static bool has_update(void) { return s_upd_ver[0] != 0; }
static bool has_appupd(void) { return s_appupd_n > 0; }
static int list_n(void) { return s_napps + 1; }   /* the apps and Marketplace */
static int market_index(void) { return s_napps; }
static int appupd_index(void) { return has_appupd() ? list_n() : -1; }
static int upd_index(void) { return has_update() ? list_n() + has_appupd() : -1; }
static int set_index(void) { return list_n() + has_appupd() + has_update(); }
static int home_count(void) { return set_index() + 1; }
static int fixed_rows(void) { return 1 + has_appupd() + has_update(); }
static int app_rows(void) { return HOME_ROWS - fixed_rows(); }
/* The fixed rows sit on the footer, Settings the lowest; a fixed row's top. */
static int fixed_y(int i) { return FOOT_Y - (set_index() - i + 1) * ROW_H; }
static int divider_y(void) { return FOOT_Y - fixed_rows() * ROW_H - (DIVIDER_H + ROW_GAP) / 2; }

/* The name the menu shows an app by, an installed one or the marketplace's:
   the title from app.json, else the folder name. */
static const char *shown_as(const char *title, const char *name) { return title[0] ? title : name; }
static const char *app_title(int i) { return shown_as(s_apps[i].title, s_apps[i].app.name); }

/* By the shown name, then the folder name (the same order every time). */
static int cmp_apps(const void *pa, const void *pb)
{
    const menu_app_t *a = pa, *b = pb;
    int c = strcasecmp(shown_as(a->title, a->app.name), shown_as(b->title, b->app.name));
    return c ? c : strcmp(a->app.name, b->app.name);
}

static void reload_apps(void)
{
    static vfs_app_t apps[VFS_MAX_APPS];
    s_napps = vfs_apps(apps, VFS_MAX_APPS);
    for (int i = 0; i < s_napps; i++) {
        menu_app_t *a = &s_apps[i];
        app_meta_t m;
        char why[80];
        app_meta_load(apps[i].name, &m);
        a->app = apps[i];
        snprintf(a->title, sizeof a->title, "%s", m.title);
        a->lua_api = m.lua_api;
        a->api_bad = app_meta_api_problem(&m, why, sizeof why) != NULL;
    }
    qsort(s_apps, (size_t)s_napps, sizeof *s_apps, cmp_apps);
    s_gen = vfs_generation();
    if (s_sel >= home_count()) s_sel = home_count() - 1;
    if (s_sel < 0) s_sel = 0;
}

/* The Marketplace row: a plus and the word in the accent colour, on a tint,
   so it doesn't read as an app. */
static void market_text(int i, int y, bool sel)
{
    int w = gfx_width(), cx = PAD + 6, cy = y + ROW_BG / 2;   /* the plus: 13 px wide, from PAD */
    bool pressed = s_press == i && s_press_view == s_view;
    if (!sel && !pressed) gfx_fill_rect(INSET, y, w - 2 * INSET, ROW_BG, C_TINT);
    gfx_fill_rect(cx - 6, cy - 1, 13, 3, C_ACCENT);
    gfx_fill_rect(cx - 1, cy - 6, 3, 13, C_ACCENT);
    gfx_text(PAD + 22, ROW_TEXT(y), "Marketplace", C_ACCENT, 2, NULL);
}

static void home_text(int i, int y, bool sel)
{
    if (i == market_index()) { market_text(i, y, sel); return; }
    int w = gfx_width();
    vfs_app_t *a = &s_apps[i].app;
    const char *tag = a->state == VFS_RAM ? "not installed" : a->state == VFS_MODIFIED ? "modified" : "";
    if (!strcmp(a->name, settings_autostart())) tag = a->state == VFS_INSTALLED ? "autostart" : tag;
    bool warn = a->state != VFS_INSTALLED;
    if (s_apps[i].api_bad) { tag = "won't run"; warn = true; }
    if (app_update_for(a->name)) { tag = "update"; warn = false; }   /* (an update fixes a "won't run") */
    if (*tag) row_value(y, tag, warn ? C_WARN : C_ACCENT);
    char t[48];
    fit(t, sizeof t, app_title(i), w - 2 * PAD - 8 - (*tag ? gfx_text_width(tag, 1) + 8 : 0), 2);
    row_label(y, t, s_apps[i].api_bad ? C_DIM : C_TEXT);   /* greyed: kept, but off */
}

static list_t home_list(void)
{
    return (list_t){ .n = list_n(), .rows = app_rows(), .top = s_top,
                     .sel = s_sel < list_n() ? s_sel : -1, .selc = C_SEL, .text = home_text };
}

/* A fixed row at home: its background, and its label in colour c. */
static void home_fixed_row(int i, const char *label, gfx_color_t c)
{
    int y = fixed_y(i);
    gfx_fill_rect(0, y, gfx_width(), ROW_BG, C_BG);
    row_bg(i, y, s_sel == i, C_SEL);
    row_label(y, label, c);
}

static void home_settings_row(void)
{
    home_fixed_row(set_index(), "Settings", s_sel == set_index() ? C_TEXT : C_DIM);
}

static void home_update_row(void)
{
    if (update_available()) home_fixed_row(upd_index(), "Update available", C_ACCENT);   /* the version is on its screen */
    else home_fixed_row(upd_index(), "Manual update needed", C_WARN);
}

static void home_appupd_row(void)
{
    char t[32];
    snprintf(t, sizeof t, "App updates (%d)", s_appupd_n);
    home_fixed_row(appupd_index(), t, C_ACCENT);
}

static int text_wrap(int y, const char *s, int lines, gfx_color_t c);

static void draw_home(void)
{
    int w = gfx_width();
    gfx_fill(C_BG);
    draw_header();
    list_scroll(&s_top, s_sel, list_n(), app_rows());
    list_t l = home_list();
    list_draw(&l);
    if (s_napps == 0)
        text_wrap(BELOW(1), "No apps installed yet. Get some from the Marketplace above, or install one from a "
                            "computer: luaboard install DIR", 4, C_DIM);
    gfx_fill_rect(PAD, divider_y(), w - 2 * PAD, 1, gfx_rgb(52, 66, 86));   /* divider, then the fixed rows */
    if (has_appupd()) home_appupd_row();
    if (has_update()) home_update_row();
    home_settings_row();

    draw_footer("up", s_sel < s_napps ? "open  (hold: options)" : "open", "down");
    gfx_flush();
}

static void autostart_label(int choice, char *out, size_t cap)
{
    if (choice < 0 || choice >= s_napps) snprintf(out, cap, "off");
    else snprintf(out, cap, "%s", app_title(choice));
}

static int autostart_index(void)
{
    for (int i = 0; i < s_napps; i++) if (!strcmp(s_apps[i].app.name, settings_autostart())) return i;
    return -1;
}

/* ---- the bar: a setting from 0..100 % shown as a bar -------------------
   One component for every such row (Brightness, Volume): a row in s_bars
   says how to read and keep its value and how a value being chosen shows
   (the light, a beep). The drawing, touch (tap, drag) and the buttons'
   edit mode below are the same for all of them. */
typedef struct {
    int row;                                   /* its Settings row */
    int min;                                   /* the lowest value (100 is the highest) */
    int (*get)(void);
    esp_err_t (*save)(int);
    void (*preview)(int);                      /* a value being chosen, before it's saved */
    void (*cancel)(void);                      /* a change not saved: back as it was (NULL: nothing to undo) */
    bool live_drag;                            /* preview at every step of a drag, else when let go */
} bar_t;

static void brightness_cancel(void) { hw_backlight(settings_brightness()); }
static void volume_preview(int v) { sound_preview(v); }
static void brightness_preview(int v) { hw_backlight(v); }

static const bar_t s_bars[] = {
    { S_BRIGHTNESS, BRIGHTNESS_MIN, settings_brightness, settings_set_brightness, brightness_preview,
      brightness_cancel, true },
    { S_VOLUME, 0, settings_volume, settings_set_volume, volume_preview, NULL, false },
};

/* The bar of Settings row r, or NULL if it has none. */
static const bar_t *bar_of(int r)
{
    for (size_t i = 0; i < sizeof s_bars / sizeof s_bars[0]; i++)
        if (s_bars[i].row == r) return &s_bars[i];
    return NULL;
}

static void bar_cancel(const bar_t *b)
{
    if (b && b->cancel) b->cancel();
}

/* A bar row's bar and value at y: the value being chosen if it's this row's. */
static void bar_draw(const bar_t *b, int y, bool sel, char *v, size_t cap)
{
    int value = sel && (s_editing || s_dragging) ? s_edit_val : b->get(), by = y + (ROW_BG - BAR_H) / 2;
    gfx_rect(BAR_X, by, BAR_W, BAR_H, C_DIM, false);
    gfx_fill_rect(BAR_X + 2, by + 2, (BAR_W - 4) * value / 100, BAR_H - 4, C_ACCENT);
    if (value) snprintf(v, cap, "%d%%", value);
    else snprintf(v, cap, "off");
}

static void settings_text(int i, int y, bool sel)
{
    int w = gfx_width();
    static const char *labels[S_COUNT] = {
        [S_BRIGHTNESS] = "Brightness", [S_SCREEN_OFF] = "Sleep after", [S_VOLUME] = "Volume",
        [S_TURN_OFF] = "Turn off after", [S_SLEEP] = "Sleep now", [S_AUTOSTART] = "Autostart app", [S_UPDATES] = "Check for updates", [S_CHECK] = "Check now",
        [S_TIME] = "Time", [S_VERSION] = "Version",
        [S_STORAGE] = "Storage", [S_WIFI] = "Wi-Fi", [S_BACK] = "Back",
    };
    if (i == S_BACK) back_label(y);
    else row_label(y, labels[i], S_INFO(i) ? C_DIM : C_TEXT);
    char v[48] = "";
    gfx_color_t c = C_TEXT;
    if (bar_of(i)) bar_draw(bar_of(i), y, sel, v, sizeof v);
    else switch (i) {
    case S_SCREEN_OFF:
    case S_TURN_OFF: {
        int m = i == S_SCREEN_OFF ? settings_screen_off() : settings_turn_off();
        const char *of = i == S_TURN_OFF ? " asleep" : "";   /* what Turn off after counts */
        if (!m) snprintf(v, sizeof v, "never");
        else if (m % 60 == 0) snprintf(v, sizeof v, "%d h%s", m / 60, of);
        else snprintf(v, sizeof v, "%d min%s", m, of);
        break;
    }
    case S_AUTOSTART: autostart_label(autostart_index(), v, sizeof v); break;
    case S_UPDATES: snprintf(v, sizeof v, "%s", settings_update_check() ? "on" : "off"); break;
    case S_CHECK: snprintf(v, sizeof v, "%s", s_checking ? "checking..." : ""); break;
    case S_TIME:
        if (s_sync_until) { snprintf(v, sizeof v, "asking..."); c = C_WARN; }
        else if (clock_last_sync()) {
            struct tm tm;
            clock_local(clock_last_sync(), &tm, NULL);
            snprintf(v, sizeof v, "synced %02d:%02d", tm.tm_hour, tm.tm_min);
        } else if (!clock_valid()) { snprintf(v, sizeof v, "not set"); c = C_WARN; }
        break;
    case S_VERSION: snprintf(v, sizeof v, "%s, %s", fw_version(), hw_model_name()); break;
    case S_STORAGE: {
        size_t ram, used, total;
        vfs_usage(&ram, &used, &total);
        snprintf(v, sizeof v, "%zu / %zu KB", used / 1024, total / 1024);
        break;
    }
    case S_WIFI: {
        const char *ip = net_ip();
        snprintf(v, sizeof v, "%s", ip && strcmp(ip, "-") ? ip : "off");
        break;
    }
    default: break;
    }
    char f[48];
    int label_end = PAD + (i == S_BACK ? 0 : gfx_text_width(labels[i], 2)) + 10;   /* the value stays clear of it */
    fit(f, sizeof f, v, w - PAD - label_end, 1);
    if (*f) row_value(y, f, S_INFO(i) ? C_DIM : c);
}

static list_t settings_list(void)
{
    return (list_t){ .n = S_COUNT, .rows = ROWS, .top = s_stop, .sel = s_ssel,
                     .selc = s_editing ? gfx_rgb(90, 60, 20) : C_SEL, .text = settings_text };
}

static void draw_settings(void)
{
    gfx_fill(C_BG);
    draw_header();
    list_scroll(&s_stop, s_ssel, S_COUNT, ROWS);
    list_t l = settings_list();
    list_draw(&l);
    static const char *ok[S_COUNT] = {   /* by row: a row added to the enum can't shift these */
        [S_BRIGHTNESS] = "change", [S_SCREEN_OFF] = "change", [S_TURN_OFF] = "change", [S_VOLUME] = "change", [S_SLEEP] = "sleep",
        [S_AUTOSTART] = "change", [S_UPDATES] = "change", [S_CHECK] = "check", [S_TIME] = "sync now", [S_BACK] = "back",
    };
    if (s_editing) draw_footer("less", "done", "more");
    else draw_footer("up", ok[s_ssel], "down");
    gfx_flush();
}

/* ------------------------------------------------- autostart picker */

static void build_pick(void)
{
    s_npick = 0;
    s_pick[s_npick++] = -1;                           /* off */
    for (int i = 0; i < s_napps; i++) if (s_apps[i].app.state != VFS_RAM) s_pick[s_npick++] = i;
    s_pick[s_npick++] = PICK_BACK;
    if (s_psel >= s_npick) s_psel = s_npick - 1;
}

static void pick_text(int k, int y, bool sel)
{
    int w = gfx_width(), c = s_pick[k];
    char t[48], f[48];
    if (c == PICK_BACK) snprintf(t, sizeof t, "Back");
    else autostart_label(c, t, sizeof t);
    fit(f, sizeof f, t, w - 2 * PAD - gfx_text_width("chosen", 1) - 8, 2);
    if (c == PICK_BACK) back_label(y);
    else row_label(y, f, C_TEXT);
    if (c == autostart_index()) row_value(y, "chosen", C_ACCENT);
}

static list_t pick_list(void)
{
    return (list_t){ .n = s_npick, .rows = ROWS, .top = s_ptop, .sel = s_psel, .selc = C_SEL, .text = pick_text };
}

static void draw_pick(void)
{
    gfx_fill(C_BG);
    draw_header();
    list_scroll(&s_ptop, s_psel, s_npick, ROWS);
    list_t l = pick_list();
    list_draw(&l);
    draw_footer("up", "choose  (hold: back)", "down");
    gfx_flush();
}

static void open_pick(void)
{
    s_view = V_PICK;
    s_ptop = 0;
    build_pick();
    s_psel = 0;
    int cur = autostart_index();
    for (int k = 0; k < s_npick; k++) if (s_pick[k] == cur) s_psel = k;
    draw_pick();
}

static void back_to_settings(void)
{
    s_view = V_SETTINGS;
    s_ssel = S_AUTOSTART;
    draw_settings();
}

/* The view handlers: an action, with the touch event behind it (NULL for
   the system's back and home). */
static void pick_event(act_t ev, const btn_event_t *e)
{
    build_pick();
    if (ev == M_DRAG || ev == M_DRAG_END) {                   /* the list follows the finger */
        if (list_drag(&s_ptop, &s_psel, s_npick, ROWS, e)) draw_pick();
        return;
    }
    if (ev == M_TAP) {
        int k = list_row_at(pick_list(), e->y);
        if (k < 0) return;
        s_psel = k;
        ev = M_OK;
    }
    switch (ev) {
    case M_UP: s_psel = (s_psel + s_npick - 1) % s_npick; draw_pick(); break;
    case M_DOWN: s_psel = (s_psel + 1) % s_npick; draw_pick(); break;
    case M_OK: {
        int c = s_pick[s_psel];
        if (c != PICK_BACK) settings_set_autostart(c >= 0 ? s_apps[c].app.name : "");
        back_to_settings();
        break;
    }
    case M_BACK: back_to_settings(); break;
    default: break;
    }
}

static int app_index(const char *name)
{
    for (int i = 0; i < s_napps; i++) if (!strcmp(s_apps[i].app.name, name)) return i;
    return -1;
}

/* Rows for the app in s_oapp: Autostart only for installed apps, Open only
   for one this firmware runs. */
static void build_options(void)
{
    int i = app_index(s_oapp);
    s_norows = 0;
    if (i < 0 || !s_apps[i].api_bad) s_orows[s_norows++] = O_OPEN;
    if (i >= 0 && s_apps[i].app.state != VFS_RAM) s_orows[s_norows++] = O_AUTOSTART;
    s_orows[s_norows++] = O_CLEAR;
    s_orows[s_norows++] = O_UNINSTALL;
    s_orows[s_norows++] = O_BACK;
    if (s_osel >= s_norows) s_osel = s_norows - 1;
}

static bool options_armed(int r)
{
    return r == s_osel && s_armed && (s_orows[r] == O_UNINSTALL || s_orows[r] == O_CLEAR);
}

/* `s` word-wrapped at scale 1 from y, `cols` characters wide, in at most
   `lines` lines (the last cut with ".." if there's more). Returns the y
   below it. */
static int text_wrap_cols(int y, const char *s, int lines, gfx_color_t c, int cols)
{
    char line[200];
    for (int l = 0; l < lines && *s; l++) {
        const char *e = text_skip(s, cols), *b = e;     /* e: past `cols` characters */
        if (*e) while (b > s && *b != ' ') b--;
        int n = (int)((b > s ? b : e) - s);
        if (l == lines - 1 && *e) snprintf(line, sizeof line, "%.*s..", (int)(text_skip(s, cols - 2) - s), s);
        else snprintf(line, sizeof line, "%.*s", n, s);
        gfx_text(PAD, y, line, c, 1, NULL);
        y += LINE_H;
        s += n;
        while (*s == ' ') s++;
    }
    return y;
}

_Static_assert((BOARD_LCD_W - 2 * PAD) / CHAR_W >= APP_DESC_COLS,
               "an app's options show APP_DESC_COLS characters of its description a line");

/* The same, as wide as the screen. */
static int text_wrap(int y, const char *s, int lines, gfx_color_t c)
{
    return text_wrap_cols(y, s, lines, c, (gfx_width() - 2 * PAD) / CHAR_W);
}

/* One line of small text below a list, cut to the screen. Returns the y below it. */
static int text_line(int y, const char *s, gfx_color_t c)
{
    char shown[160];
    fit(shown, sizeof shown, s, gfx_width() - 2 * PAD, 1);
    gfx_text(PAD, y, shown, c, 1, NULL);
    return y + LINE_H;
}

static void options_text(int r, int y, bool sel)
{
    int id = s_orows[r];
    bool armed = options_armed(r), autostart = !strcmp(settings_autostart(), s_oapp);
    const char *label = id == O_OPEN ? "Open" : id == O_AUTOSTART ? "Autostart"
                      : id == O_CLEAR ? (armed ? "Clear data?" : "Clear data")
                      : id == O_UNINSTALL ? (armed ? "Uninstall?" : "Uninstall") : "Back";
    if (id == O_BACK) back_label(y);
    else row_label(y, label, C_TEXT);
    if (id == O_AUTOSTART) row_value(y, autostart ? "on" : "off", autostart ? C_ACCENT : C_TEXT);
    if (armed) row_value(y, "tap again", C_TEXT);
}

static list_t options_list(void)
{
    return (list_t){ .n = s_norows, .rows = s_norows, .sel = s_osel,
                     .selc = options_armed(s_osel) ? C_DANGER : C_SEL, .text = options_text };
}

static void draw_options(void)
{
    int ai = app_index(s_oapp);
    if (ai < 0) return;
    app_meta_load(s_oapp, &s_ometa);                  /* may have been reinstalled */
    build_options();
    gfx_fill(C_BG);
    draw_header();
    list_t l = options_list();
    list_draw(&l);
    /* About the app: what it does (3 lines), version and author, and why it won't run */
    char line[160];
    int y = text_wrap(BELOW(s_norows), s_ometa.description, APP_DESC_LINES, C_TEXT);
    if (s_ometa.version[0] || s_ometa.author[0]) {
        snprintf(line, sizeof line, "%s%s%s%s", s_ometa.version[0] ? "version " : "", s_ometa.version,
                 s_ometa.version[0] && s_ometa.author[0] ? ", by " : s_ometa.author[0] ? "by " : "", s_ometa.author);
        y = text_line(y, line, C_DIM);                          /* a dev build's version is long */
    }
    char why[80];
    if (app_meta_api_problem(&s_ometa, why, sizeof why)) text_line(y, why, C_WARN);
    if (s_armed) draw_footer("cancel", s_orows[s_osel] == O_CLEAR ? "clear data" : "uninstall", "cancel");
    else draw_footer("up", "select  (hold: back)", "down");
    gfx_flush();
}

/* ------------------------------------------------- firmware update */

static void list_add(char *buf, size_t cap, const char *sep, const char *s)
{
    size_t n = strlen(buf);
    if (n + 1 < cap) snprintf(buf + n, cap - n, "%s%s", n ? sep : "", s);
}

/* The installed apps release u won't run (their lua_api outside its range):
   folder names ("a,b") and titles ("A, B"), "" for none. RAM-only apps
   don't count, the restart takes them anyway. */
static void wont_run(const update_manifest_t *u, char *names, size_t ncap, char *titles, size_t tcap)
{
    names[0] = titles[0] = 0;
    for (int i = 0; i < s_napps; i++) {
        if (s_apps[i].app.state == VFS_RAM) continue;
        app_meta_t m = { .lua_api = s_apps[i].lua_api };
        if (app_meta_runs_on(&m, u->lua_api_min, u->lua_api)) continue;
        list_add(names, ncap, ",", s_apps[i].app.name);
        list_add(titles, tcap, ", ", app_title(i));
    }
}

/* The update screen's rows: Install and Back, or only Back when the release
   can't be installed from here (update_manual). */
static int update_rows(void) { return update_available() ? U_COUNT : 1; }
static int update_row_id(int r) { return update_available() ? r : U_BACK; }

static void update_text(int r, int y, bool sel)
{
    (void)sel;
    if (update_row_id(r) == U_BACK) back_label(y);
    else row_label(y, "Install", C_TEXT);
}

static list_t update_list(void)
{
    return (list_t){ .n = update_rows(), .rows = update_rows(), .sel = s_usel, .selc = C_SEL, .text = update_text };
}

/* Install and Back; below them the release: version and size, its notes,
   and the apps it won't run. Or Back, and what to do about a release this
   firmware can't install. */
static void draw_update(void)
{
    const update_manifest_t *u = update_available();
    if (!u && !update_manual()) return;
    if (s_usel >= update_rows()) s_usel = 0;
    gfx_fill(C_BG);
    draw_header();
    list_t l = update_list();
    list_draw(&l);
    int y = BELOW(update_rows());
    if (!u) {
        y = text_wrap(y, "There is a newer firmware, but this version can't install it by itself.", 2, C_TEXT);
        text_wrap(y + PARA, "Install it from a computer. How: " UPDATE_HELP_URL, 3, C_DIM);
        draw_footer("up", "select  (hold: back)", "down");
        gfx_flush();
        return;
    }
    char line[300], names[200], titles[240];
    unsigned tenths = (unsigned)((u->size * 10 + 512 * 1024) / (1024 * 1024));   /* MB, one decimal */
    snprintf(line, sizeof line, "%s, %u.%u MB (now %s)", u->version, tenths / 10, tenths % 10, fw_version());
    y = text_line(y, line, C_TEXT);
    y = text_wrap(y, u->notes, 2, C_DIM);
    wont_run(u, names, sizeof names, titles, sizeof titles);
    if (titles[0]) {
        snprintf(line, sizeof line, "Won't run after it: %s", titles);
        text_wrap(y + PARA, line, 2, C_WARN);
    }
    draw_footer("up", "select  (hold: back)", "down");
    gfx_flush();
}

static void open_update(void)
{
    const update_manifest_t *u = update_available();
    if (!u && update_manual()) {
        s_view = V_UPDATE;
        s_usel = 0;
        draw_update();
        proto_eventf("update offer manual");
        return;
    }
    if (!u) return;
    char names[200], titles[240];
    ESP_LOGI(TAG, "firmware update offered: %s", u->version);
    wont_run(u, names, sizeof names, titles, sizeof titles);
    s_view = V_UPDATE;
    s_usel = U_INSTALL;
    draw_update();
    proto_eventf("update offer ver=%s wont-run=%s", u->version, names[0] ? names : "-");
}

/* ------------------------------------------------------------ app updates */

/* The App updates screen: Update all, the apps with an update (title, and
   installed -> new version, or why its update failed), Back. Or only Back
   once there are none. */
static int appupd_rows(void) { return app_update_count() ? app_update_count() + 2 : 1; }
static bool appupd_is_back(int r) { return r == appupd_rows() - 1; }

static const char *failed_why(const char *app)
{
    for (int i = 0; i < s_nfailed; i++)
        if (!strcmp(s_failed[i].app, app)) return s_failed[i].why;
    return NULL;
}

static void set_failed(const char *app, const char *why)
{
    int i = 0;
    while (i < s_nfailed && strcmp(s_failed[i].app, app)) i++;
    if (i == VFS_MAX_APPS) return;
    if (i == s_nfailed) s_nfailed++;
    snprintf(s_failed[i].app, sizeof s_failed[i].app, "%s", app);
    snprintf(s_failed[i].why, sizeof s_failed[i].why, "%s", why);
}

static void appupd_text(int r, int y, bool sel)
{
    (void)sel;
    int w = gfx_width();
    if (appupd_is_back(r)) { back_label(y); return; }
    if (r == 0) { row_label(y, "Update all", C_TEXT); return; }
    const apps_entry_t *e = app_update_at(r - 1);
    if (!e) return;
    char right[64], shown[64], t[48];
    const char *why = failed_why(e->name);
    if (why) {
        snprintf(right, sizeof right, "failed: %s", why);
    } else {
        app_meta_t m;
        app_meta_load(e->name, &m);
        snprintf(right, sizeof right, "%s -> %s", m.version[0] ? m.version : "?", e->version);
    }
    fit(shown, sizeof shown, right, w / 2, 1);
    row_value(y, shown, why ? C_WARN : C_DIM);
    fit(t, sizeof t, shown_as(e->title, e->name), w - 2 * PAD - 8 - gfx_text_width(shown, 1) - 8, 2);
    row_label(y, t, C_TEXT);
}

static list_t appupd_list(void)
{
    return (list_t){ .n = appupd_rows(), .rows = ROWS, .top = s_atop, .sel = s_asel, .selc = C_SEL,
                     .text = appupd_text };
}

static void draw_appupd(void)
{
    if (s_asel >= appupd_rows()) s_asel = appupd_rows() - 1;
    list_scroll(&s_atop, s_asel, appupd_rows(), ROWS);
    gfx_fill(C_BG);
    draw_header();
    list_t l = appupd_list();
    list_draw(&l);
    if (!app_update_count()) text_wrap(BELOW(1), "Every app from the marketplace is up to date.", 2, C_DIM);
    draw_footer("up", "select  (hold: back)", "down");
    gfx_flush();
}

static void open_appupd(void)
{
    s_view = V_APPUPD;
    s_asel = s_atop = 0;
    s_nfailed = 0;
    draw_appupd();
    ESP_LOGI(TAG, "app updates offered: %d", app_update_count());
}

/* The progress of the install under way, when it moved. */
static void appupd_progress(void)
{
    int pct = s_qn ? (s_qi * 100 + app_install_pct()) / s_qn : 0;
    if (pct == s_shown_pct && s_qi == s_shown_i) return;
    s_shown_pct = pct;
    s_shown_i = s_qi;
    const apps_index_t *ix = app_update_index();
    const apps_entry_t *e = ix ? apps_index_find(ix, s_queue[s_qi]) : NULL;
    char line[80];
    snprintf(line, sizeof line, "%s: %s", shown_as(e ? e->title : "", s_queue[s_qi]), app_install_file());
    screen_progress(s_qn > 1 ? "Updating apps" : "Updating", line, pct, "Keep the power on.");
}

/* Start the next app in the queue; when none is left, say how it went. */
static void appupd_next(void)
{
    char err[64];
    while (s_qi < s_qn) {
        if (app_install_start(s_queue[s_qi], err, sizeof err)) {
            s_shown_pct = s_shown_i = -1;
            appupd_progress();
            return;
        }
        set_failed(s_queue[s_qi], err);
        s_qfail++;
        s_qi++;
    }
    s_updating = false;
    char msg[64];
    if (s_after == V_STORE_APP) {                             /* one app, from the Marketplace */
        const char *why = failed_why(s_queue[0]);
        ESP_LOGI(TAG, "store install %s: %s", s_queue[0], why ? why : "ok");
        if (why) snprintf(msg, sizeof msg, "failed: %s", why);
        else snprintf(msg, sizeof msg, "installed");
        if (!why) snprintf(s_picked, sizeof s_picked, "%s", s_queue[0]);
    } else {
        ESP_LOGI(TAG, "app updates done: %d updated, %d failed", s_qok, s_qfail);
        if (s_qfail) snprintf(msg, sizeof msg, "%d updated, %d failed", s_qok, s_qfail);
        else snprintf(msg, sizeof msg, "%d updated", s_qok);
        s_asel = 0;
    }
    reload_apps();
    menu_toast(msg);
}

bool menu_installing(void) { return s_updating; }

/* Settings > Check now is over: what it found, as a toast (and in the log). */
static void check_answer(void)
{
    char msg[64] = "", part[40];
    s_checking = false;
    const update_manifest_t *u = update_available();
    int n = app_update_count();
    if (u) snprintf(msg, sizeof msg, "firmware %s", u->version);
    if (n) {
        snprintf(part, sizeof part, "%s%d app update%s", msg[0] ? ", " : "", n, n == 1 ? "" : "s");
        strncat(msg, part, sizeof msg - strlen(msg) - 1);
    }
    const char *err = update_error()[0] ? update_error() : app_update_error();
    if (!msg[0] && err[0]) snprintf(msg, sizeof msg, "check failed: %.40s", err);
    else if (!msg[0]) snprintf(msg, sizeof msg, "everything is up to date");
    ESP_LOGI(TAG, "check now: %s", msg);
    menu_toast(msg);
}

/* Each tick while updating: the install under way, then the next. */
static void appupd_tick(void)
{
    if (app_install_busy()) { appupd_progress(); return; }
    const char *why = app_install_result();
    if (why) { set_failed(s_queue[s_qi], why); s_qfail++; }
    else s_qok++;
    s_qi++;
    appupd_next();
}

/* Update the app of row r (0: all of them). */
static void appupd_start(int r)
{
    s_qn = s_qi = s_qok = s_qfail = 0;
    int n = app_update_count();
    for (int i = 0; i < n; i++) {
        if (r != 0 && i != r - 1) continue;
        snprintf(s_queue[s_qn++], sizeof s_queue[0], "%s", app_update_at(i)->name);
    }
    if (!s_qn) return;
    s_after = V_APPUPD;
    s_updating = true;
    appupd_next();
}

static void appupd_event(act_t ev, const btn_event_t *e)
{
    int n = appupd_rows();
    if (ev == M_DRAG || ev == M_DRAG_END) {
        if (list_drag(&s_atop, &s_asel, n, ROWS, e)) draw_appupd();
        return;
    }
    if (ev == M_TAP) {
        int r = list_row_at(appupd_list(), e->y);
        if (r < 0) return;
        s_asel = r;
        ev = M_OK;
    }
    switch (ev) {
    case M_UP: s_asel = (s_asel + n - 1) % n; draw_appupd(); break;
    case M_DOWN: s_asel = (s_asel + 1) % n; draw_appupd(); break;
    case M_OK:
        if (appupd_is_back(s_asel)) { s_view = V_HOME; draw_home(); }
        else appupd_start(s_asel);
        break;
    case M_BACK: s_view = V_HOME; draw_home(); break;
    default: break;
    }
}

/* ------------------------------------------------------------ marketplace */

static void open_app(const char *name);
static void redraw(void);

static const apps_index_t *s_sort_ix;  /* for cmp_store */

static int cmp_store(const void *pa, const void *pb)
{
    const apps_entry_t *a = &s_sort_ix->app[*(const int *)pa], *b = &s_sort_ix->app[*(const int *)pb];
    int c = strcasecmp(shown_as(a->title, a->name), shown_as(b->title, b->name));
    return c ? c : strcmp(a->name, b->name);
}

/* The index's apps by title, from the index known now. */
static void build_store(void)
{
    const apps_index_t *ix = app_update_index();
    s_nstore = ix && !ix->newer_format ? ix->n : 0;
    for (int i = 0; i < s_nstore; i++) s_store[i] = i;
    s_sort_ix = ix;
    if (s_nstore) qsort(s_store, (size_t)s_nstore, sizeof *s_store, cmp_store);
    s_store_text = app_update_text(NULL);
    s_store_checking = app_update_checking();
}

static const apps_entry_t *store_at(int r)
{
    const apps_index_t *ix = app_update_index();
    return ix && r >= 0 && r < s_nstore ? &ix->app[s_store[r]] : NULL;
}

/* Where the store is at, for the log (and the tests): "<n> apps", or why none. */
static const char *store_state(char *buf, size_t cap)
{
    const apps_index_t *ix = app_update_index();
    if (ix && ix->newer_format) return "later format";
    if (ix) { snprintf(buf, cap, "%d apps", s_nstore); return buf; }
    if (app_update_checking()) return "checking";
    if (!app_update_configured()) return "no marketplace address";
    if (!strcmp(net_ip(), "-")) return "no Wi-Fi";
    if (app_update_error()[0]) { snprintf(buf, cap, "unreachable: %s", app_update_error()); return buf; }
    return "checking";
}

/* What the screen says instead of apps, or NULL when it has some. */
static const char *store_message(char *buf, size_t cap)
{
    const apps_index_t *ix = app_update_index();
    if (ix && ix->newer_format) return "This firmware is too old for the marketplace: update it.";
    if (ix) return s_nstore ? NULL : "The marketplace has no apps for this firmware.";
    if (app_update_checking()) return "Looking for apps...";
    if (!app_update_configured()) return "No marketplace address.";
    if (!strcmp(net_ip(), "-")) return "No Wi-Fi. Set it up from a computer: luaboard wifi set, or the studio.";
    if (app_update_error()[0]) {
        snprintf(buf, cap, "Couldn't reach the marketplace: %s", app_update_error());
        return buf;
    }
    return "Looking for apps...";
}

static int store_rows(void) { return s_nstore + 1; }   /* the apps and Back */

static void store_text(int r, int y, bool sel)
{
    (void)sel;
    int w = gfx_width();
    const apps_entry_t *e = store_at(r);
    if (!e) { back_label(y); return; }
    const char *tag = "";
    gfx_color_t c = C_DIM;
    if (app_update_for(e->name)) { tag = "update"; c = C_ACCENT; }
    else if (vfs_app_exists(e->name)) tag = "installed";
    else if (e->needs_setup) { tag = "(i)"; c = C_WARN; }
    if (*tag) row_value(y, tag, c);
    char t[48];
    fit(t, sizeof t, shown_as(e->title, e->name), w - 2 * PAD - 8 - (*tag ? gfx_text_width(tag, 1) + 8 : 0), 2);
    row_label(y, t, C_TEXT);
}

static list_t store_list(void)
{
    return (list_t){ .n = store_rows(), .rows = ROWS, .top = s_ttop, .sel = s_tsel, .selc = C_SEL, .text = store_text };
}

static void draw_store(void)
{
    char buf[96];
    if (s_tsel >= store_rows()) s_tsel = store_rows() - 1;
    list_scroll(&s_ttop, s_tsel, store_rows(), ROWS);
    gfx_fill(C_BG);
    draw_header();
    list_t l = store_list();
    list_draw(&l);
    const char *msg = store_message(buf, sizeof buf);
    if (msg) text_wrap(BELOW(1), msg, 4, C_DIM);
    draw_footer("up", "select  (hold: back)", "down");
    gfx_flush();
}

static void open_store(void)
{
    char buf[64];
    app_update_check_now();
    build_store();
    s_view = V_STORE;
    s_tsel = s_ttop = 0;
    draw_store();
    ESP_LOGI(TAG, "store opened: %s", store_state(buf, sizeof buf));
}

/* Back home from the store, on the app just installed if there is one. */
static void store_home(void)
{
    s_view = V_HOME;
    if (s_picked[0]) {
        reload_apps();
        for (int i = 0; i < s_napps; i++)
            if (!strcmp(s_apps[i].app.name, s_picked)) s_sel = i;
        s_picked[0] = 0;
    }
    draw_home();
}

/* An app's screen: Install, Update or Open; Back. Below them who made it,
   its version and license, what it does, and for an app that needs keys or
   a login, where its setup steps are, as text and as a QR code. */
#define QR_SCALE 3
#define QR_QUIET 2

/* An app's README address fits a version 5 QR code (106 bytes at the low
   error correction), 37 modules a side: the description beside it keeps
   APP_DESC_QR_COLS characters, as the marketplace's checks expect. */
_Static_assert(sizeof APPS_README_URL - 1 + VFS_APP_NAME_MAX - 1 <= 106, "a README address in a version 5 QR code");
_Static_assert((BOARD_LCD_W - 2 * PAD - (37 + 2 * QR_QUIET) * QR_SCALE - GAP) / CHAR_W >= APP_DESC_QR_COLS,
               "the description beside the QR code keeps APP_DESC_QR_COLS characters");

static const apps_entry_t *store_entry(void)
{
    const apps_index_t *ix = app_update_index();
    return ix ? apps_index_find(ix, s_sapp) : NULL;
}

static const char *store_action(const apps_entry_t *e)
{
    return app_update_for(e->name) ? "Update" : vfs_app_exists(e->name) ? "Open" : "Install";
}

static void store_app_text(int r, int y, bool sel)
{
    (void)sel;
    const apps_entry_t *e = store_entry();
    if (r == 1 || !e) back_label(y);
    else row_label(y, store_action(e), C_TEXT);
}

static list_t store_app_list(void)
{
    return (list_t){ .n = 2, .rows = 2, .sel = s_dsel, .selc = C_SEL, .text = store_app_text };
}

/* A QR code of text with its quiet zone, its right edge at xr, top at y.
   Returns its left edge, or xr if it couldn't be made. */
static int draw_qr(int xr, int y, const char *text)
{
    uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(6)], tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
    if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_LOW, 1, 6, qrcodegen_Mask_AUTO, true)) return xr;
    int n = qrcodegen_getSize(qr), side = (n + 2 * QR_QUIET) * QR_SCALE, x = xr - side;
    gfx_fill_rect(x, y, side, side, gfx_rgb(255, 255, 255));
    for (int my = 0; my < n; my++)
        for (int mx = 0; mx < n; mx++)
            if (qrcodegen_getModule(qr, mx, my))
                gfx_fill_rect(x + (QR_QUIET + mx) * QR_SCALE, y + (QR_QUIET + my) * QR_SCALE, QR_SCALE, QR_SCALE,
                              gfx_rgb(0, 0, 0));
    return x;
}

static void draw_store_app(void)
{
    const apps_entry_t *e = store_entry();
    if (!e) { s_view = V_STORE; build_store(); draw_store(); return; }   /* gone from the index meanwhile */
    int w = gfx_width(), y = BELOW(2), right = w - PAD;
    gfx_fill(C_BG);
    draw_header();
    list_t l = store_app_list();
    list_draw(&l);
    char url[160], line[160], shown[160];
    snprintf(url, sizeof url, APPS_README_URL "%s", e->name);
    if (e->needs_setup) right = draw_qr(w - PAD, y, url) - GAP;   /* the text on its left */
    int cols = (right - PAD) / CHAR_W;
    snprintf(line, sizeof line, "by %s, %s, %s", e->author[0] ? e->author : "?", e->version,
             e->license[0] ? e->license : "no license");
    fit(shown, sizeof shown, line, cols * CHAR_W, 1);
    gfx_text(PAD, y, shown, C_DIM, 1, NULL);
    y = text_wrap_cols(y + LINE_H + PARA, e->description, e->needs_setup ? APP_DESC_QR_LINES : 8, C_TEXT, cols);
    if (e->needs_setup) {
        y = text_wrap_cols(y + PARA, "Setup needed: your own keys or a login. The steps:", 3, C_WARN, cols);
        text_wrap_cols(y, url + strlen("https://"), 3, C_DIM, cols);
    }
    draw_footer("up", "select  (hold: back)", "down");
    gfx_flush();
}

static void open_store_app(const apps_entry_t *e)
{
    snprintf(s_sapp, sizeof s_sapp, "%s", e->name);
    s_view = V_STORE_APP;
    s_dsel = 0;
    draw_store_app();
    ESP_LOGI(TAG, "store app %s: %s%s", e->name, store_action(e), e->needs_setup ? ", needs setup" : "");
}

static void store_event(act_t ev, const btn_event_t *e)
{
    int n = store_rows();
    if (ev == M_DRAG || ev == M_DRAG_END) {
        if (list_drag(&s_ttop, &s_tsel, n, ROWS, e)) draw_store();
        return;
    }
    if (ev == M_TAP) {
        int r = list_row_at(store_list(), e->y);
        if (r < 0) return;
        s_tsel = r;
        ev = M_OK;
    }
    switch (ev) {
    case M_UP: s_tsel = (s_tsel + n - 1) % n; draw_store(); break;
    case M_DOWN: s_tsel = (s_tsel + 1) % n; draw_store(); break;
    case M_OK:
        if (store_at(s_tsel)) open_store_app(store_at(s_tsel));
        else store_home();
        break;
    case M_BACK: store_home(); break;
    default: break;
    }
}

static void store_app_event(act_t ev, const btn_event_t *e)
{
    if (ev == M_TAP) {
        int r = list_row_at(store_app_list(), e->y);
        if (r < 0) return;
        s_dsel = r;
        ev = M_OK;
    }
    const apps_entry_t *a = store_entry();
    switch (ev) {
    case M_UP: case M_DOWN: s_dsel = !s_dsel; draw_store_app(); break;
    case M_OK:
        if (s_dsel == 1 || !a) { s_view = V_STORE; draw_store(); break; }
        if (!strcmp(store_action(a), "Open")) { open_app(a->name); break; }
        s_qn = 1;
        s_qi = s_qok = s_qfail = 0;
        s_nfailed = 0;
        snprintf(s_queue[0], sizeof s_queue[0], "%s", a->name);
        s_after = V_STORE_APP;
        s_updating = true;
        appupd_next();
        break;
    case M_BACK: s_view = V_STORE; draw_store(); break;
    default: break;
    }
}

/* The store's list again when the index it shows changed (a check ended). */
static void store_sync(void)
{
    if (s_view != V_STORE && s_view != V_STORE_APP) return;
    if (app_update_text(NULL) == s_store_text && app_update_checking() == s_store_checking) return;
    char buf[64];
    build_store();
    if (!s_store_checking) ESP_LOGI(TAG, "store: %s", store_state(buf, sizeof buf));
    redraw();
}

/* The header's title. */
static const char *view_title(void)
{
    const apps_entry_t *e;
    switch (s_view) {
    case V_HOME: case V_APP: return FW_NAME;
    case V_SETTINGS: return "Settings";
    case V_PICK: return "Autostart app";
    case V_UPDATE: return "Firmware update";
    case V_APPUPD: return "App updates";
    case V_STORE: return "Marketplace";
    case V_STORE_APP: return (e = store_entry()) ? shown_as(e->title, e->name) : "Marketplace";
    case V_OPTIONS: return shown_as(s_ometa.title, s_oapp);
    }
    return FW_NAME;
}

static void redraw(void)
{
    if (s_view == V_OPTIONS && app_index(s_oapp) < 0) s_view = V_HOME;   /* removed meanwhile */
    if (s_view == V_UPDATE && !update_available() && !update_manual()) s_view = V_HOME;   /* forgotten meanwhile */
    switch (s_view) {
    case V_HOME: draw_home(); break;
    case V_SETTINGS: draw_settings(); break;
    case V_PICK: build_pick(); draw_pick(); break;
    case V_OPTIONS: draw_options(); break;
    case V_UPDATE: draw_update(); break;
    case V_APPUPD: draw_appupd(); break;
    case V_STORE: draw_store(); break;
    case V_STORE_APP: draw_store_app(); break;
    case V_APP: break;                                        /* the app's own screen */
    }
}

/* ------------------------------------------------------------ actions */

/* Which fixed row is selected, so the selection stays on it when another
   one comes or goes (the apps and Marketplace above them keep their indexes). */
enum { SEL_LIST, SEL_APPUPD, SEL_UPD, SEL_SETTINGS };

static int sel_fixed(void)
{
    return s_sel == set_index() ? SEL_SETTINGS : s_sel == upd_index() ? SEL_UPD
         : s_sel == appupd_index() ? SEL_APPUPD : SEL_LIST;
}

/* Back on that row, or Settings if it went. */
static void sel_restore(int which)
{
    if (which == SEL_UPD && upd_index() >= 0) s_sel = upd_index();
    else if (which == SEL_APPUPD && appupd_index() >= 0) s_sel = appupd_index();
    else if (which != SEL_LIST || s_sel >= home_count()) s_sel = set_index();
}

/* Take the release update_available() knows now for the home's update row.
   True if it changed. */
static bool update_sync(void)
{
    const update_manifest_t *u = update_available();
    const char *v = u ? u->version : update_manual() ? "manual" : "";   /* (a version starts with v) */
    if (!strcmp(v, s_upd_ver)) return false;
    int which = sel_fixed();
    snprintf(s_upd_ver, sizeof s_upd_ver, "%s", v);
    sel_restore(which);
    return true;
}

/* The same for the App updates row: how many apps it offers. Not while
   they are being updated (the count moves as each one is done). */
static bool appupd_sync(void)
{
    int n = app_update_count();
    if (s_updating || n == s_appupd_n) return false;
    int which = sel_fixed();
    s_appupd_n = n;
    sel_restore(which);
    return true;
}

void menu_show(void)
{
    if (script_running()) script_stop();
    screen_error_clear();
    screen_notice_clear();
    hw_backlight(settings_brightness());
    s_view = V_HOME;
    s_editing = s_dragging = false;
    s_armed = false;
    reload_apps();
    update_sync();
    redraw();
    proto_eventf("menu");
}

void menu_toast(const char *msg)
{
    snprintf(s_toast, sizeof s_toast, "%s", msg);
    s_toast_until = (hw_millis() + 4000) | 1;   /* 0 means no toast */
    redraw();
}

static void open_app(const char *name)
{
    char err[200];
    s_view = V_APP;
    if (script_run(name, err, sizeof err)) proto_eventf("started %s", script_current());
    else proto_eventf("error %s", err);          /* the error screen is up */
}

/* The home row at y (as home_count counts them), -1 for none. */
static int home_row_at(int y)
{
    if (y >= divider_y()) {
        for (int i = list_n(); i <= set_index(); i++)
            if (y < fixed_y(i) + ROW_H) return i;
        return -1;
    }
    return list_row_at(home_list(), y);
}

/* The options of the selected app. */
static void open_options(void)
{
    snprintf(s_oapp, sizeof s_oapp, "%.*s", (int)sizeof s_oapp - 1, s_apps[s_sel].app.name);
    app_meta_load(s_oapp, &s_ometa);
    s_view = V_OPTIONS; s_osel = 0; s_armed = false;
    draw_options();
}

static void home_event(act_t ev, const btn_event_t *e)
{
    int n = home_count();
    if (ev == M_DRAG || ev == M_DRAG_END) {                   /* the apps follow the finger (Settings stays) */
        if (e->y0 < divider_y() && list_drag(&s_top, s_sel < list_n() ? &s_sel : NULL, list_n(), app_rows(), e))
            draw_home();
        return;
    }
    if (ev == M_TAP || ev == M_HOLD) {                /* hold an app: its options */
        int i = home_row_at(e->y);
        if (i < 0 || (ev == M_HOLD && i >= s_napps)) return;
        if (ev == M_HOLD) buttons_ignore_touch();         /* opens the options: letting go isn't a tap */
        s_sel = i;
        ev = ev == M_TAP ? M_OK : M_BACK;
    }
    switch (ev) {
    case M_UP: s_sel = (s_sel + n - 1) % n; draw_home(); break;
    case M_DOWN: s_sel = (s_sel + 1) % n; draw_home(); break;
    case M_OK:
        if (s_sel == set_index()) { s_view = V_SETTINGS; s_ssel = s_stop = 0; s_editing = s_dragging = false; draw_settings(); }
        else if (s_sel == upd_index()) open_update();
        else if (s_sel == appupd_index()) open_appupd();
        else if (s_sel == market_index()) open_store();
        else if (s_apps[s_sel].api_bad && app_update_for(s_apps[s_sel].app.name)) open_appupd();   /* the fix for it */
        else if (!s_apps[s_sel].api_bad) open_app(s_apps[s_sel].app.name);
        else open_options();                            /* one this firmware won't run: why, and Uninstall */
        break;
    case M_BACK:                                        /* hold on an app: its options */
        if (s_sel < s_napps) open_options();
        break;
    default: break;
    }
}

static void uninstall_app(void)
{
    char name[VFS_APP_NAME_MAX], msg[64];
    snprintf(name, sizeof name, "%s", s_oapp);
    const char *shown = shown_as(s_ometa.title, name);
    esp_err_t err = app_remove(name);                     /* its data goes with it */
    s_armed = false;
    s_view = V_HOME;
    reload_apps();
    if (err == ESP_OK) {
        proto_eventf("uninstalled %s", name);
        snprintf(msg, sizeof msg, "%s uninstalled", shown);
    } else {
        snprintf(msg, sizeof msg, "couldn't uninstall %s", shown);
    }
    menu_toast(msg);
}

/* Clear data: delete the app's secrets (logins, keys) and stored values; the app stays. */
static void clear_app_data(void)
{
    char msg[64];
    const char *shown = shown_as(s_ometa.title, s_oapp);
    s_armed = false;
    if (appdata_clear(s_oapp) == ESP_OK) {
        proto_eventf("cleared %s", s_oapp);
        snprintf(msg, sizeof msg, "%s: data cleared", shown);
    } else {
        snprintf(msg, sizeof msg, "couldn't clear %s", shown);
    }
    menu_toast(msg);
}

static void options_event(act_t ev, const btn_event_t *e)
{
    build_options();
    if (ev == M_TAP) {
        int r = list_row_at(options_list(), e->y);
        if (r < 0) return;
        if (r != s_osel) s_armed = false;
        s_osel = r;
        ev = M_OK;
    }
    switch (ev) {
    case M_UP: case M_DOWN:
        if (s_armed) s_armed = false;
        else s_osel = (s_osel + (ev == M_DOWN ? 1 : s_norows - 1)) % s_norows;
        draw_options();
        break;
    case M_OK:
        switch (s_orows[s_osel]) {
        case O_OPEN: open_app(s_oapp); break;
        case O_AUTOSTART:
            settings_set_autostart(strcmp(settings_autostart(), s_oapp) ? s_oapp : "");
            draw_options();
            break;
        case O_CLEAR:
            if (s_armed) clear_app_data();
            else { s_armed = true; draw_options(); }
            break;
        case O_UNINSTALL:
            if (s_armed) uninstall_app();
            else { s_armed = true; draw_options(); }
            break;
        default: s_view = V_HOME; draw_home(); break;
        }
        break;
    case M_BACK:
        if (s_armed) { s_armed = false; draw_options(); }
        else { s_view = V_HOME; draw_home(); }
        break;
    default: break;
    }
}

/* Leaving it (Back, hold B, the power key) is going home, like any way out of an app. */
static void update_event(act_t ev, const btn_event_t *e)
{
    if (ev == M_TAP) {
        int r = list_row_at(update_list(), e->y);
        if (r < 0) return;
        s_usel = r;
        ev = M_OK;
    }
    switch (ev) {
    case M_UP: case M_DOWN: s_usel = (s_usel + 1) % update_rows(); draw_update(); break;
    case M_OK:
        if (update_row_id(s_usel) == U_INSTALL) {
            char err[96];
            if (!update_install(err, sizeof err)) menu_toast(err);
        } else {
            menu_show();
        }
        break;
    case M_BACK: menu_show(); break;
    default: break;
    }
}

/* Settings: the bar row (x, y) is on (a little past its ends, not its %
   label), or -1. And the value for x: the fill is drawn over the BAR_W - 4
   pixels from BAR_X + 2, so a touch lands where the fill ends. */
static const bar_t *bar_under(int x, int y)
{
    const bar_t *b = bar_of(list_row_at(settings_list(), y));
    return b && x >= BAR_X - 12 && x < BAR_X + BAR_W + 12 ? b : NULL;
}

static int bar_clamp(const bar_t *b, int v) { return v < b->min ? b->min : v > 100 ? 100 : v; }

static int bar_at(const bar_t *b, int x)
{
    int v = ((x - BAR_X - 2) * 100 + (BAR_W - 4) / 2) / (BAR_W - 4);
    return bar_clamp(b, (v + 2) / 5 * 5);                    /* steps of 5 */
}

/* Settings: the next row up or down that does something. */
static int settings_step(int from, int dir)
{
    int i = from;
    do i = (i + dir + S_COUNT) % S_COUNT; while (S_INFO(i));
    return i;
}

/* The choice after cur in choices[n], round. */
static int next_choice(const int *choices, int n, int cur)
{
    int k = 0;
    while (k < n && choices[k] != cur) k++;
    return choices[(k + 1) % n];
}

static void settings_event(act_t ev, const btn_event_t *e)
{
    if (ev == M_DRAG || ev == M_DRAG_END) {                   /* a finger slid along a bar, or the list */
        const bar_t *b = bar_under(e->x0, e->y0);
        if (!b) {
            if (list_drag(&s_stop, &s_ssel, S_COUNT, ROWS, e)) draw_settings();
            return;
        }
        if (s_editing && s_ssel != b->row) bar_cancel(bar_of(s_ssel));
        s_ssel = b->row;
        s_editing = false;                                    /* touch sets it directly, no edit mode */
        int v = bar_at(b, e->x);
        if (ev == M_DRAG && s_dragging && v == s_edit_val) return;   /* the same step */
        s_dragging = ev == M_DRAG;                            /* live while it moves ... */
        s_edit_val = v;
        if (b->live_drag || ev == M_DRAG_END) b->preview(v);
        if (ev == M_DRAG_END) b->save(v);                     /* ... saved when let go */
        draw_settings();
        return;
    }
    if (ev == M_TAP) {
        int r = list_row_at(settings_list(), e->y);
        if (r >= 0 && S_INFO(r)) return;
        const bar_t *b = bar_under(e->x, e->y);
        if (b) {                                              /* set it right there */
            if (s_editing && s_ssel != b->row) bar_cancel(bar_of(s_ssel));
            int v = bar_at(b, e->x);
            b->preview(v);
            b->save(v);
            s_editing = false;
            s_ssel = b->row;
            draw_settings();
            return;
        }
        if (s_editing) ev = M_OK;                            /* elsewhere: done */
        else if (bar_of(r)) { s_ssel = r; draw_settings(); return; }   /* its name: the bar does the rest */
        else if (r >= 0) { s_ssel = r; ev = M_OK; }
        else return;
    }
    if (s_editing) {
        const bar_t *b = bar_of(s_ssel);
        if (ev == M_UP || ev == M_DOWN) {
            s_edit_val = bar_clamp(b, s_edit_val + (ev == M_DOWN ? 1 : -1) * 10);
            b->preview(s_edit_val);                           /* live: the light, or a beep */
        } else if (ev == M_OK) {                             /* done: save */
            b->save(s_edit_val);
            s_editing = false;
        } else if (ev == M_BACK) {                          /* cancel */
            bar_cancel(b);
            s_editing = false;
        } else {
            return;
        }
        draw_settings();
        return;
    }
    switch (ev) {
    case M_UP: s_ssel = settings_step(s_ssel, -1); draw_settings(); break;
    case M_DOWN: s_ssel = settings_step(s_ssel, 1); draw_settings(); break;
    case M_OK:
        if (S_INFO(s_ssel)) break;
        if (s_ssel == S_BACK) { s_view = V_HOME; draw_home(); break; }
        if (s_ssel == S_SCREEN_OFF) {
            settings_set_screen_off(next_choice(s_off_choices, OFF_N, settings_screen_off()));
            draw_settings();
            break;
        }
        if (s_ssel == S_TURN_OFF) {
            settings_set_turn_off(next_choice(s_turn_choices, TURN_N, settings_turn_off()));
            draw_settings();
            break;
        }
        if (s_ssel == S_SLEEP) { sleep_now(); break; }
        if (s_ssel == S_AUTOSTART) { open_pick(); break; }
        if (s_ssel == S_CHECK) {                      /* both checks now; the answer comes as a toast */
            if (!strcmp(net_ip(), "-")) { menu_toast("no Wi-Fi"); break; }
            update_check_now();
            app_update_check_force();
            s_checking = update_checking() || app_update_checking();
            if (!s_checking && (update_error()[0] || app_update_error()[0])) check_answer();   /* failed at once: why */
            else if (!s_checking) menu_toast("nothing to check: no update addresses");
            draw_settings();
            break;
        }
        if (s_ssel == S_UPDATES) {                    /* off forgets the release, on checks a minute later */
            settings_set_update_check(!settings_update_check());
            update_setting_changed();
            app_update_setting_changed();
            draw_settings();
            break;
        }
        if (s_ssel == S_TIME) {                       /* ask the time server now */
            const char *ip = net_ip();
            if (!ip || !strcmp(ip, "-")) { menu_toast("the time comes from the network: needs Wi-Fi"); break; }
            if (clock_sync_now() != ESP_OK) { menu_toast("couldn't ask the time server"); break; }
            s_sync_from = clock_syncs();
            s_sync_until = hw_millis() + 10000;
            draw_settings();
            break;
        }
        if (!bar_of(s_ssel)) break;
        s_editing = true;                                     /* a bar, with the buttons */
        s_edit_val = bar_of(s_ssel)->get();
        draw_settings();
        break;
    case M_BACK: s_view = V_HOME; draw_home(); break;
    default: break;
    }
}

void menu_init(void)
{
    buttons_init();
    s_view = V_APP;
}

/* A touch event as the menu sees it. A and C step through lists: on a
   press, and while held (the long press, then its repeats). */
static act_t action(const btn_event_t *e)
{
    switch (e->kind) {
    case BTN_PRESS: return e->what == 'A' ? M_UP : e->what == 'C' ? M_DOWN : M_OK;
    case BTN_LONG: case BTN_REPEAT: return e->what == 'A' ? M_UP : M_DOWN;
    case BTN_TAP: return M_TAP;
    case BTN_HOLD: return M_HOLD;
    case BTN_HOLD_UP: return M_TAP;                       /* a hold that did nothing: a tap after all */
    case BTN_SWIPE: return M_NONE;                        /* lists scroll with the drag instead */
    case BTN_DRAG: return e->what == 'e' ? M_DRAG_END : M_DRAG;
    }
    return M_NONE;
}

/* The row of the view on the screen at (x, y), as row_bg counts them; -1 none. */
static int row_under(int x, int y)
{
    switch (s_view) {
    case V_HOME: return home_row_at(y);
    case V_SETTINGS: {                                       /* the bar sets, it isn't a row press; info isn't one */
        int r = bar_under(x, y) ? -1 : list_row_at(settings_list(), y);
        return r >= 0 && S_INFO(r) ? -1 : r;
    }
    case V_PICK: return list_row_at(pick_list(), y);
    case V_OPTIONS: return list_row_at(options_list(), y);
    case V_UPDATE: return list_row_at(update_list(), y);
    case V_APPUPD: return list_row_at(appupd_list(), y);
    case V_STORE: return list_row_at(store_list(), y);
    case V_STORE_APP: return list_row_at(store_app_list(), y);
    case V_APP: return -1;
    }
    return -1;
}

/* One action for the view on the screen. */
static void menu_act(act_t ev, const btn_event_t *e)
{
    if (s_view == V_APP) {                                    /* the error screen of a failed script */
        if (ev == M_HOLD) buttons_ignore_touch();
        if (ev == M_BACK || ev == M_HOME || ev == M_HOLD) menu_show();
        else if ((ev == M_OK || ev == M_TAP) && script_last()[0]) {
            char name[VFS_MAX_NAME];
            snprintf(name, sizeof name, "%s", script_last());
            open_app(name);
        }
        return;
    }
    if (ev == M_HOME) {                                       /* power key: straight home */
        if (s_view == V_SETTINGS && (s_editing || s_dragging)) bar_cancel(bar_of(s_ssel));
        s_editing = s_dragging = false;
        s_armed = false;
        if (s_view == V_UPDATE) menu_show();
        else if (s_view != V_HOME) { s_view = V_HOME; draw_home(); }
        return;
    }
    switch (s_view) {
    case V_HOME: home_event(ev, e); break;
    case V_SETTINGS: settings_event(ev, e); break;
    case V_PICK: pick_event(ev, e); break;
    case V_OPTIONS: options_event(ev, e); break;
    case V_UPDATE: update_event(ev, e); break;
    case V_APPUPD: appupd_event(ev, e); break;
    case V_STORE: store_event(ev, e); break;
    case V_STORE_APP: store_app_event(ev, e); break;
    case V_APP: break;                                        /* above */
    }
}

/* Row i of the view on the screen again, e.g. now that it's (not) pressed. */
static void redraw_row(int i)
{
    list_t l = { 0 };                                         /* no rows: nothing to draw */
    switch (s_view) {
    case V_HOME:
        if (i == set_index()) { home_settings_row(); return; }
        if (i == upd_index()) { home_update_row(); return; }
        if (i == appupd_index()) { home_appupd_row(); return; }
        l = home_list();
        break;
    case V_SETTINGS: l = settings_list(); break;
    case V_PICK: build_pick(); l = pick_list(); break;
    case V_OPTIONS: build_options(); l = options_list(); break;
    case V_UPDATE: l = update_list(); break;
    case V_APPUPD: l = appupd_list(); break;
    case V_STORE: l = store_list(); break;
    case V_STORE_APP: l = store_app_list(); break;
    case V_APP: return;
    }
    list_redraw_row(&l, i);
}

void menu_tick(void)
{
    btn_sys_t sys = buttons_poll();

    if (screen_error_active()) s_view = V_APP;              /* e.g. a RUN over USB that failed */
    if (script_running()) {                                   /* the app has the screen and the events */
        s_view = V_APP;                                       /* also when started over USB/Wi-Fi */
        if (sys != BTN_SYS_NONE) {
            proto_eventf("stopped %s (%s)", script_current(), sys == BTN_SYS_HOME ? "power key" : "back button");
            menu_show();
        }
        return;
    }
    if (s_view == V_APP && !screen_error_active()) {
        menu_show();                                          /* stopped, exited, or nothing ran */
        return;
    }

    if (s_updating) {                                         /* app updates: the screen is theirs, input waits */
        btn_event_t ignored;
        while (buttons_next(&ignored)) {}
        appupd_tick();
        if (s_updating) return;
        s_view = s_after;
        appupd_sync();
        redraw();
    }

    if (s_view != V_APP) {
        bool upd = update_sync() && (s_view == V_HOME || s_view == V_UPDATE);   /* the update row came, went or changed */
        if (appupd_sync() && (s_view == V_HOME || s_view == V_APPUPD)) upd = true;
        store_sync();
        if (vfs_generation() != s_gen || upd) { reload_apps(); redraw(); }
        else if (hw_millis() - s_header_at > 5000) {
            draw_header();                                    /* battery, Wi-Fi */
            gfx_flush();
        }
        if (s_toast_until && (int32_t)(hw_millis() - s_toast_until) >= 0) { s_toast_until = 0; redraw(); }
        if (s_checking && !update_checking() && !app_update_checking()) check_answer();
        if (s_sync_until) {                                   /* Settings > Time: the answer */
            if (clock_syncs() != s_sync_from) { s_sync_until = 0; menu_toast("time synced"); }
            else if ((int32_t)(hw_millis() - s_sync_until) >= 0) { s_sync_until = 0; menu_toast("no answer from the time server"); }
        }
    }

    if (sys != BTN_SYS_NONE) menu_act(sys == BTN_SYS_HOME ? M_HOME : M_BACK, NULL);
    btn_event_t e;
    /* An app opened, or an update started (Install): the rest isn't the menu's. */
    while (!script_running() && !ota_active() && buttons_next(&e)) {
        act_t ev = action(&e);
        if (ev != M_NONE) menu_act(ev, &e);
    }

    /* The row under a finger shows pressed: repaint the rows that changed
       (whatever the events drew had the old one). Not over the update's screen. */
    if (script_running() || s_view == V_APP || ota_active()) { s_press = -1; return; }
    int px, py, press = -1;
    if (buttons_pressing(&px, &py)) press = row_under(px, py);
    int old = s_press_view == s_view ? s_press : -1;
    s_press = press;
    s_press_view = s_view;
    if (press == old) return;
    if (old >= 0) redraw_row(old);
    if (press >= 0) redraw_row(press);
    gfx_flush();
}
