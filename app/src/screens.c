#include "screens.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "board.h"
#include "gfx.h"
#include "hw.h"
#include "net.h"
#include "ota.h"

static bool s_error_active, s_notice_active;

void screen_splash(const char *status)
{
    const gfx_color_t bg = gfx_rgb(12, 18, 28), accent = gfx_rgb(80, 180, 255);
    int w = gfx_width(), h = gfx_height();
    gfx_fill(bg);
    gfx_fill_rect(0, 0, w, 4, accent);
    gfx_text(24, 40, "Lua", gfx_rgb(255, 255, 255), 6, NULL);
    gfx_text(140, 62, "on ESP32-S3", accent, 2, NULL);
    char ver[80];
    snprintf(ver, sizeof ver, "%s %s  -  %s", FW_NAME, fw_version(), BOARD_NAME);
    gfx_text(24, 100, ver, gfx_rgb(150, 160, 175), 1, NULL);
    gfx_fill_rect(24, 120, w - 48, 1, gfx_rgb(60, 70, 85));
    gfx_text(24, h - 60, status, gfx_rgb(230, 230, 230), 2, NULL);
    gfx_flush();
    s_error_active = s_notice_active = false;
}

void screen_restarted(const char *reason, bool power, const char *cause, const char *where)
{
    const gfx_color_t bg = gfx_rgb(12, 18, 28), warn = gfx_rgb(255, 170, 60);
    const gfx_color_t white = gfx_rgb(235, 238, 242), dim = gfx_rgb(150, 160, 175);
    int w = gfx_width();
    gfx_fill(bg);
    gfx_fill_rect(0, 0, w, 4, warn);
    gfx_text(24, 34, "Restarted", warn, 3, NULL);
    char why[64];
    if (power) snprintf(why, sizeof why, "The power dipped (%s).", reason);
    else snprintf(why, sizeof why, "The firmware crashed (%s).", reason);
    gfx_text(24, 78, why, white, 1, NULL);
    int y = 92;
    if (cause && *cause) {                       /* what the crash record says */
        char line[64];
        snprintf(line, sizeof line, "%.52s", cause);
        gfx_text(24, y, line, white, 1, NULL);
        y += 12;
        if (where && *where) {
            snprintf(line, sizeof line, "at %.48s", where);
            gfx_text(24, y, line, white, 1, NULL);
            y += 12;
        }
    }
    gfx_text(24, y + 10, power ? "Check the USB cable and power supply,\nor the battery's charge."
                               : "If an app keeps doing this, keep a finger\non the screen at boot to skip autostart.",
             dim, 1, NULL);
    if (cause && *cause) gfx_text(24, y + 40, "The PC tools show the whole trace: luaboard crash", dim, 1, NULL);
    gfx_flush();
    s_error_active = s_notice_active = false;
}

void screen_restarted_countdown(int secs)
{
    const gfx_color_t bg = gfx_rgb(12, 18, 28), dim = gfx_rgb(150, 160, 175);
    char line[48];
    snprintf(line, sizeof line, "Going on in %d s, or tap the screen", secs);
    gfx_text(24, gfx_height() - 28, line, dim, 1, &bg);
    gfx_flush();
}

void screen_update(const char *title, const char *line, int pct)
{
    screen_progress(title, line, pct, "Keep the power on: it restarts by itself.");
}

void screen_progress(const char *title, const char *line, int pct, const char *note)
{
    const gfx_color_t bg = gfx_rgb(12, 18, 28), accent = gfx_rgb(80, 180, 255);
    const gfx_color_t white = gfx_rgb(255, 255, 255), dim = gfx_rgb(150, 160, 175);
    int w = gfx_width(), h = gfx_height();
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    gfx_fill(bg);
    gfx_fill_rect(0, 0, w, 4, accent);
    gfx_text(24, 40, title, white, 2, NULL);
    char ver[80];
    snprintf(ver, sizeof ver, "%s %s  -  %s", FW_NAME, fw_version(), BOARD_NAME);
    gfx_text(24, 66, ver, dim, 1, NULL);
    const int bx = 24, by = 110, bw = w - 48, bh = 22;
    gfx_rect(bx, by, bw, bh, gfx_rgb(60, 70, 85), false);
    gfx_fill_rect(bx + 2, by + 2, (bw - 4) * pct / 100, bh - 4, accent);
    char p[8];
    snprintf(p, sizeof p, "%d%%", pct);
    gfx_text(bx + bw - gfx_text_width(p, 2), by + bh + 10, p, white, 2, NULL);
    char shown[64];                                   /* under the %, as wide as the bar */
    snprintf(shown, sizeof shown, "%.*s", bw / gfx_text_width("0", 1), line);
    gfx_text(bx, by + bh + 36, shown, white, 1, NULL);
    gfx_text(24, h - 30, note, dim, 1, NULL);
    gfx_flush();
    s_error_active = s_notice_active = false;
}

/* Draw text word-wrapped to `cols` characters per line (scale 1 = 6 px per
   character). Returns the y below the last line. Stops at max_y. */
static int wrapped(int x, int y, const char *text, int cols, gfx_color_t c, int max_y)
{
    char line[64];
    const char *p = text;
    while (*p && y + 8 <= max_y) {
        while (*p == ' ') p++;
        size_t n = strlen(p);
        size_t take = n < (size_t)cols ? n : (size_t)cols;
        if (take < n) {                        /* break at the last space if there is one */
            size_t sp = take;
            while (sp > 0 && p[sp] != ' ') sp--;
            if (sp > (size_t)cols / 3) take = sp;
        }
        if (take >= sizeof line) take = sizeof line - 1;
        memcpy(line, p, take);
        line[take] = 0;
        gfx_text(x, y, line, c, 1, NULL);
        p += take;
        y += 11;
    }
    return y;
}

void screen_error(const char *script, const char *message)
{
    const gfx_color_t bg = gfx_rgb(24, 8, 10), red = gfx_rgb(220, 50, 50);
    const gfx_color_t white = gfx_rgb(255, 255, 255), dim = gfx_rgb(170, 160, 165);
    int w = gfx_width(), h = gfx_height();

    gfx_fill(bg);
    gfx_fill_rect(0, 0, w, 30, red);
    gfx_text(10, 8, "Lua error", white, 2, NULL);
    if (script && *script) gfx_text(w - 10 - gfx_text_width(script, 1), 12, script, white, 1, NULL);

    /* Lua messages look like "main.lua:11: attempt to ...": show the
       location big and the rest wrapped below it. */
    const char *msg = message ? message : "";
    char where[48] = "";
    const char *colon = strstr(msg, ".lua:");
    if (colon) {
        const char *num = colon + 5;
        char *end;
        long line = strtol(num, &end, 10);
        if (end != num && *end == ':') {
            int file_len = (int)(colon + 4 - msg);
            snprintf(where, sizeof where, "%.*s line %ld", file_len < 32 ? file_len : 32, msg, line);
            msg = end + 1;
        }
    }
    int y = 42;
    if (where[0]) {
        gfx_text(10, y, where, gfx_rgb(255, 200, 80), 2, NULL);
        y += 24;
    }
    y = wrapped(10, y, msg, (w - 20) / 6, white, h - 52);

    gfx_fill_rect(10, h - 46, w - 20, 1, gfx_rgb(90, 40, 45));
    char hint[96];
    const char *ip = net_ip();
    if (ip && strcmp(ip, "-") != 0)
        snprintf(hint, sizeof hint, "The script stopped. Upload a fix (USB or Wi-Fi %s),", ip);
    else
        snprintf(hint, sizeof hint, "The script stopped. Upload a fix over USB,");
    gfx_text(10, h - 38, hint, dim, 1, NULL);
    gfx_text(10, h - 24, hw_has_buttons() ? "Tap or press o: retry.  Hold (screen or o): menu." : "Tap: retry.  Hold: menu.",
             dim, 1, NULL);
    gfx_flush();
    s_error_active = true;
    s_notice_active = false;
}

void screen_notice(const char *title, const char *message)
{
    const gfx_color_t bg = gfx_rgb(12, 18, 28), accent = gfx_rgb(80, 180, 255);
    const gfx_color_t white = gfx_rgb(255, 255, 255), dim = gfx_rgb(150, 160, 175);
    int w = gfx_width(), h = gfx_height();
    gfx_fill(bg);
    gfx_fill_rect(0, 0, w, 4, accent);
    gfx_text(10, 16, title, white, 2, NULL);
    wrapped(10, 46, message, (w - 20) / 6, white, h - 40);
    gfx_fill_rect(10, h - 32, w - 20, 1, gfx_rgb(60, 70, 85));
    gfx_text(10, h - 22, "print() output: studio log or 'luaboard monitor'", dim, 1, NULL);
    gfx_flush();
    s_notice_active = true;
    s_error_active = false;
}

bool screen_notice_active(void) { return s_notice_active; }
void screen_notice_clear(void) { s_notice_active = false; }

bool screen_error_active(void) { return s_error_active; }
void screen_error_clear(void) { s_error_active = false; }
