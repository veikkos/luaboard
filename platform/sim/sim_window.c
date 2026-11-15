/*
 * The simulator's window. The board draws itself on an offscreen canvas at
 * an integer scale (LCD pixel -> canvas pixels), which is shown in a
 * resizable, Retina-aware window, letterboxed so the shape stays the
 * device's. Two ways to scale:
 *
 *   Fit window   any size; the canvas is drawn at the next integer scale
 *                up and shrunk smoothly to fit.
 *   Pixel exact  every LCD pixel is a whole number of screen pixels,
 *                the canvas shown 1:1.
 *
 * After a resize the window snaps to the device's shape (in Pixel exact,
 * to a whole scale). The settings (S or Cmd+,) hold these and the board's
 * inputs (sim_board_options). The view is kept in a file between runs.
 */
#include "sim_window.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_board.h"
#include "sim_ui.h"

#define MAX_SCALE   8           /* canvas scale; beyond it Fit stretches the canvas */
#define SNAP_MS     250         /* a resize is over when no other came for this long */
#define BG          0x16191d    /* around the letterboxed canvas */

typedef struct {
    SDL_Window *win;
    SDL_Renderer *r;
    sim_ui_t ui;
    SDL_Texture *canvas;
    int cw, ch;                 /* canvas size */
    SDL_Rect dst;               /* the canvas in the window, output pixels */
    int base_w, base_h;         /* board at scale 1 */
    bool fit;                   /* Fit window, else Pixel exact */
    int start_scale;            /* window points per LCD pixel at start and on reset */
    char path[600];             /* where the view is kept; "" = nowhere */
    Uint32 resized_at;          /* a resize by the user, waiting to snap; 0 = none */
    int last_w, last_h;         /* window size (points) before it */
    int want_w, want_h;         /* size we asked for: its resize event isn't the user's */
} view_t;

static float dpi(view_t *v)
{
    int ww, wh, ow, oh;
    SDL_GetWindowSize(v->win, &ww, &wh);
    SDL_GetRendererOutputSize(v->r, &ow, &oh);
    return ww > 0 ? (float)ow / (float)ww : 1.0f;
}

static bool fullscreen(view_t *v)
{
    return SDL_GetWindowFlags(v->win) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP);
}

/* ------------------------------------------------------------ keeping it */

static void view_save(view_t *v)
{
    FILE *f = *v->path ? fopen(v->path, "w") : NULL;
    if (!f) return;
    int w = v->last_w, h = v->last_h;
    if (!fullscreen(v)) SDL_GetWindowSize(v->win, &w, &h);
    fprintf(f, "fit=%d\ngrid=%d\nsize=%dx%d\n", v->fit, v->ui.grid, w, h);
    fclose(f);
}

/* The saved size, if any, in *w x *h (points). */
static void view_load(view_t *v, int *w, int *h)
{
    FILE *f = *v->path ? fopen(v->path, "r") : NULL;
    if (!f) return;
    char line[64];
    int a, b;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "fit=%d", &a) == 1) v->fit = a != 0;
        else if (sscanf(line, "grid=%d", &a) == 1) v->ui.grid = a != 0;
        else if (sscanf(line, "size=%dx%d", &a, &b) == 2 && a > 0 && b > 0) { *w = a; *h = b; }
    }
    fclose(f);
}

/* ------------------------------------------------------------- sizing */

/* Window size (points) for a scale f (output pixels per LCD pixel), made
   to fit on its display. */
static void size_for(view_t *v, float f, float k, int *w, int *h)
{
    SDL_Rect ub;
    if (SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(v->win), &ub) == 0) {
        float fmax = fminf((float)ub.w * k / (float)v->base_w, (float)(ub.h - 32) * k / (float)v->base_h);  /* title bar */
        if (f > fmax) f = v->fit ? fmax : floorf(fmax);
    }
    float fmin = v->fit ? 0.5f : 1.0f;
    if (f < fmin) f = fmin;
    *w = (int)lroundf((float)v->base_w * f / k);
    *h = (int)lroundf((float)v->base_h * f / k);
}

static void resize_to(view_t *v, float f)
{
    int w, h;
    if (fullscreen(v)) return;
    size_for(v, f, dpi(v), &w, &h);
    int cur_w, cur_h;
    SDL_GetWindowSize(v->win, &cur_w, &cur_h);
    v->resized_at = 0;
    v->last_w = w;
    v->last_h = h;
    if (w == cur_w && h == cur_h) return;
    v->want_w = w;
    v->want_h = h;
    SDL_SetWindowSize(v->win, w, h);
}

/* The user let go of a resize: snap to the device's shape, keeping the
   side they changed most, in Pixel exact to a whole scale. */
static void snap(view_t *v)
{
    int w, h;
    SDL_GetWindowSize(v->win, &w, &h);
    v->resized_at = 0;
    if (fullscreen(v) || (SDL_GetWindowFlags(v->win) & SDL_WINDOW_MAXIMIZED)) return;
    float k = dpi(v);
    float f = abs(w - v->last_w) >= abs(h - v->last_h) ? (float)w * k / (float)v->base_w
                                                        : (float)h * k / (float)v->base_h;
    if (!v->fit) f = fmaxf(1.0f, roundf(f));
    resize_to(v, f);
    view_save(v);
}

/* Current scale, output pixels per LCD pixel, as shown. */
static float shown_scale(view_t *v) { return (float)v->dst.w / (float)v->base_w; }

static void zoom(view_t *v, int dir)
{
    float f = shown_scale(v);
    f = dir > 0 ? floorf(f + 1.01f) : ceilf(f - 1.01f);      /* the next whole scale */
    resize_to(v, f < 1.0f ? 1.0f : f);
    view_save(v);
}

static void set_fullscreen(view_t *v, bool on)
{
    if (on == fullscreen(v)) return;
    if (on) SDL_GetWindowSize(v->win, &v->last_w, &v->last_h);
    SDL_SetWindowFullscreen(v->win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

static void reset(view_t *v)
{
    v->fit = true;
    v->ui.grid = true;
    set_fullscreen(v, false);
    resize_to(v, (float)v->start_scale * dpi(v));
    SDL_SetWindowPosition(v->win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    view_save(v);
}

/* Canvas scale and where it lands, for the window's current size. */
static void layout_canvas(view_t *v)
{
    int ow, oh;
    SDL_GetRendererOutputSize(v->r, &ow, &oh);
    float f = fminf((float)ow / (float)v->base_w, (float)oh / (float)v->base_h);
    bool exact = !v->fit && f >= 1.0f;
    int s = exact ? (int)(f + 0.01f) : (int)ceilf(f - 0.01f);   /* float noise of an exact size */
    if (s < 1) s = 1;
    if (s > MAX_SCALE) s = MAX_SCALE;
    int cw, ch;
    sim_board_ui_size(s, &cw, &ch);
    if (!v->canvas || cw != v->cw || ch != v->ch) {
        if (v->canvas) SDL_DestroyTexture(v->canvas);
        v->canvas = SDL_CreateTexture(v->r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, cw, ch);
        v->cw = cw;
        v->ch = ch;
    }
    SDL_SetTextureScaleMode(v->canvas, exact ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
    v->ui.scale = s;
    int dw = cw, dh = ch;
    if (!exact) {
        float g = fminf((float)ow / (float)cw, (float)oh / (float)ch);
        dw = (int)lroundf((float)cw * g);
        dh = (int)lroundf((float)ch * g);
    }
    v->dst = (SDL_Rect){ (ow - dw) / 2, (oh - dh) / 2, dw, dh };
}

/* Window point -> canvas pixel. */
static void to_canvas(view_t *v, Sint32 *x, Sint32 *y)
{
    float k = dpi(v);
    *x = (Sint32)floorf(((float)*x * k - (float)v->dst.x) * (float)v->cw / (float)v->dst.w);
    *y = (Sint32)floorf(((float)*y * k - (float)v->dst.y) * (float)v->ch / (float)v->dst.h);
}

/* ----------------------------------------------------------- settings */

enum { W_CHOICE, W_SLIDER, W_ZOOM_OUT, W_ZOOM_IN, W_RESET, W_CLOSE };
enum { ROW_FIT = -1, ROW_GRID = -2, ROW_FULL = -3 };   /* the view's; >= 0: a board option */

typedef struct { ui_rect_t r; int kind, row, val; } widget_t;

static widget_t s_w[64];
static int s_nw;
static ui_rect_t s_panel;
static int s_drag = -1;          /* widget being dragged */

static void add(ui_rect_t r, int kind, int row, int val)
{
    if (s_nw < (int)(sizeof s_w / sizeof s_w[0])) s_w[s_nw++] = (widget_t){ r, kind, row, val };
}

/* The i-th of the '|' separated words of s, in buf. */
static const char *word(const char *s, int i, char *buf, size_t cap)
{
    for (; i > 0 && s; i--) { s = strchr(s, '|'); if (s) s++; }
    if (!s) { *buf = 0; return buf; }
    size_t n = strcspn(s, "|");
    if (n >= cap) n = cap - 1;
    memcpy(buf, s, n);
    buf[n] = 0;
    return buf;
}

static int count_words(const char *s)
{
    int n = 1;
    for (; *s; s++) n += *s == '|';
    return n;
}

static int row_value(view_t *v, int row)
{
    if (row == ROW_FIT) return v->fit;
    if (row == ROW_GRID) return v->ui.grid;
    if (row == ROW_FULL) return fullscreen(v);
    int n;
    return sim_board_get(sim_board_options(&n)[row].key);
}

static void row_set(view_t *v, int row, int val)
{
    if (row == ROW_FIT) {
        v->fit = val != 0;
        if (!v->fit) resize_to(v, fmaxf(1.0f, roundf(shown_scale(v))));
        view_save(v);
        return;
    }
    if (row == ROW_GRID) { v->ui.grid = val != 0; view_save(v); return; }
    if (row == ROW_FULL) { set_fullscreen(v, val != 0); return; }
    int n;
    const sim_board_opt_t *o = &sim_board_options(&n)[row];
    char buf[32];
    if (o->choices) word(o->choices, val, buf, sizeof buf);
    else snprintf(buf, sizeof buf, "%d", val);
    sim_board_set(o->key, buf);
}

/* Buttons for a choice: names in "a|b", the row's value lit. */
static void choice(view_t *v, ui_rect_t area, int row, const char *names)
{
    sim_ui_t *ui = &v->ui;
    int s = ui->scale, n = count_words(names), cur = row_value(v, row), gap = 2 * s;
    int bw = (area.w - (n - 1) * gap) / n;
    for (int i = 0; i < n; i++) {
        char name[32];
        ui_rect_t b = { area.x + i * (bw + gap), area.y, bw, area.h };
        ui_button(ui, b, word(names, i, name, sizeof name), s, i == cur ? 0x2f86c9 : 0x2a3038);
        add(b, W_CHOICE, row, i);
    }
}

static void settings_draw(view_t *v)
{
    sim_ui_t *ui = &v->ui;
    int s = ui->scale, n;
    const sim_board_opt_t *opt = sim_board_options(&n);
    int rh = 16 * s, bh = 11 * s, pad = 10 * s, small = (s + 1) / 2;
    int pw = 300 * s, ph = pad + 16 * s + 14 * s + 4 * rh + 14 * s + n * rh + 14 * s + pad;
    if (pw > v->cw - 2 * s) pw = v->cw - 2 * s;
    int px = (v->cw - pw) / 2, py = (v->ch - ph) / 2;
    if (py < s) py = s;
    s_nw = 0;
    s_panel = (ui_rect_t){ px, py, pw, ph };

    SDL_SetRenderDrawBlendMode(ui->r, SDL_BLENDMODE_BLEND);
    ui_color(ui, 0x000000, 150);
    SDL_RenderFillRect(ui->r, NULL);
    SDL_SetRenderDrawBlendMode(ui->r, SDL_BLENDMODE_NONE);
    ui_round_rect(ui, px, py, pw, ph, 8 * s, 0x3a4048);
    ui_round_rect(ui, px + s / 2, py + s / 2, pw - s / 2 * 2, ph - s / 2 * 2, 8 * s, 0x1e2227);

    int lx = px + pad, cx = px + 136 * s, cwid = px + pw - pad - cx, y = py + pad;
    ui_text(ui, lx, y + 2 * s, "SETTINGS", s, 0xffffff);
    ui_rect_t close = { px + pw - pad - bh, y, bh, bh };
    ui_button(ui, close, "x", s, 0x2a3038);
    add(close, W_CLOSE, 0, 0);
    y += 16 * s;

    ui_text(ui, lx, y + 3 * s, "DISPLAY", small, 0x8fa0b0);
    y += 14 * s;
    ui_text(ui, lx, y + 2 * s, "Scaling", s, 0xc8d2dc);
    choice(v, (ui_rect_t){ cx, y, cwid, bh }, ROW_FIT, "Pixel exact|Fit window");
    y += rh;
    ui_text(ui, lx, y + 2 * s, "Pixel grid", s, 0xc8d2dc);
    choice(v, (ui_rect_t){ cx, y, cwid, bh }, ROW_GRID, "Off|On");
    y += rh;
    ui_text(ui, lx, y + 2 * s, "Full screen", s, 0xc8d2dc);
    choice(v, (ui_rect_t){ cx, y, cwid, bh }, ROW_FULL, "Off|On");
    y += rh;
    ui_text(ui, lx, y + 2 * s, "Zoom", s, 0xc8d2dc);
    char buf[48];
    float f = shown_scale(v);
    if (fabsf(f - roundf(f)) < 0.01f) snprintf(buf, sizeof buf, "%dx", (int)lroundf(f));
    else snprintf(buf, sizeof buf, "%.1fx", (double)f);     /* screen pixels per LCD pixel */
    ui_rect_t minus = { cx, y, 14 * s, bh }, plus = { cx + 60 * s, y, 14 * s, bh };
    ui_rect_t rst = { cx + 80 * s, y, cwid - 80 * s, bh };
    ui_button(ui, minus, "-", s, 0x2a3038);
    ui_text(ui, cx + 14 * s + (46 * s - ui_text_w(buf, s)) / 2, y + 2 * s, buf, s, 0xc8d2dc);
    ui_button(ui, plus, "+", s, 0x2a3038);
    ui_button(ui, rst, "Reset", s, 0x2a3038);
    add(minus, W_ZOOM_OUT, 0, 0);
    add(plus, W_ZOOM_IN, 0, 0);
    add(rst, W_RESET, 0, 0);
    y += rh;

    ui_text(ui, lx, y + 3 * s, "BOARD INPUTS", small, 0x8fa0b0);
    y += 14 * s;
    for (int i = 0; i < n; i++, y += rh) {
        const sim_board_opt_t *o = &opt[i];
        ui_text(ui, lx, y + 2 * s, o->label, s, 0xc8d2dc);
        if (o->choices) {
            choice(v, (ui_rect_t){ cx, y, cwid, bh }, i, o->names ? o->names : o->choices);
            continue;
        }
        int val = sim_board_get(o->key);
        ui_rect_t track = { cx, y + s, cwid - 40 * s, 9 * s };
        ui_slider(ui, track, val - o->min, o->max - o->min, 0x4fb3ff);
        snprintf(buf, sizeof buf, "%d%s", val, o->unit ? o->unit : "");
        ui_text(ui, track.x + track.w + 6 * s, y + 2 * s, buf, s, 0xc8d2dc);
        add(track, W_SLIDER, i, 0);
    }
#ifdef __APPLE__
    const char *keys = "Esc: close  Cmd -/+: zoom  Cmd 0: reset  Cmd Ctrl F: full screen";
#else
    const char *keys = "Esc: close  Ctrl -/+: zoom  Ctrl 0: reset  F11: full screen";
#endif
    ui_text(ui, lx, y + 4 * s, keys, ui_text_fit(keys, small, pw - 2 * pad), 0x66737f);
}

static void slider_to(view_t *v, const widget_t *w, int mx)
{
    int n;
    const sim_board_opt_t *o = &sim_board_options(&n)[w->row];
    row_set(v, w->row, o->min + ui_slider_value(w->r, mx, o->max - o->min));
}

/* A mouse event on the settings (canvas coordinates). */
static void settings_mouse(view_t *v, const SDL_Event *e)
{
    int s = v->ui.scale;
    if (e->type == SDL_MOUSEBUTTONUP) { s_drag = -1; return; }
    if (e->type == SDL_MOUSEMOTION) {
        if (s_drag >= 0) slider_to(v, &s_w[s_drag], e->motion.x);
        return;
    }
    if (e->type != SDL_MOUSEBUTTONDOWN) return;
    int x = e->button.x, y = e->button.y;
    for (int i = 0; i < s_nw; i++) {
        widget_t *w = &s_w[i];
        ui_rect_t r = w->r;
        if (w->kind == W_SLIDER) r = (ui_rect_t){ r.x - 4 * s, r.y - 3 * s, r.w + 8 * s, r.h + 6 * s };
        if (!ui_hit(r, x, y)) continue;
        switch (w->kind) {
        case W_CHOICE: row_set(v, w->row, w->val); break;
        case W_SLIDER: s_drag = i; slider_to(v, w, x); break;
        case W_ZOOM_OUT: zoom(v, -1); break;
        case W_ZOOM_IN: zoom(v, 1); break;
        case W_RESET: reset(v); break;
        case W_CLOSE: v->ui.settings_open = false; break;
        }
        return;
    }
    if (!ui_hit(s_panel, x, y)) v->ui.settings_open = false;   /* a click outside closes it */
}

/* --------------------------------------------------------------- frames */

static void frame(view_t *v, const char *shot)
{
    layout_canvas(v);
    SDL_SetRenderTarget(v->r, v->canvas);
    sim_board_ui_draw(&v->ui);
    if (v->ui.settings_open) settings_draw(v);
    if (shot) {                       /* the canvas, whatever the window's size */
        SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, v->cw, v->ch, 32, SDL_PIXELFORMAT_ARGB8888);
        if (!surf || SDL_RenderReadPixels(v->r, NULL, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch) ||
            SDL_SaveBMP(surf, shot))
            fprintf(stderr, "screenshot failed: %s\n", SDL_GetError());
        if (surf) SDL_FreeSurface(surf);
    }
    SDL_SetRenderTarget(v->r, NULL);
    SDL_SetRenderDrawColor(v->r, (BG >> 16) & 0xff, (BG >> 8) & 0xff, BG & 0xff, 255);
    SDL_RenderClear(v->r);
    SDL_RenderCopy(v->r, v->canvas, NULL, &v->dst);
    SDL_RenderPresent(v->r);
}

/* macOS runs its own loop while a window edge is dragged: draw from the
   resize events themselves, so the window isn't stretched meanwhile. */
static int live_resize(void *arg, SDL_Event *e)
{
    if (e->type == SDL_WINDOWEVENT && e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) frame(arg, NULL);
    return 1;
}

/* Keys for the window itself; true if it took the key. */
static bool view_key(view_t *v, const SDL_KeyboardEvent *k, bool *quit)
{
    if (k->type != SDL_KEYDOWN) return false;
    SDL_Keycode sym = k->keysym.sym;
    /* The window's shortcuts: Cmd on a Mac (full screen Cmd Ctrl F), Ctrl
       elsewhere (full screen F11). */
    bool ctrl = k->keysym.mod & KMOD_CTRL;
#ifdef __APPLE__
    bool cmd = k->keysym.mod & KMOD_GUI;
    bool full = sym == SDLK_f && cmd && ctrl;
#else
    bool cmd = ctrl;
    bool full = sym == SDLK_F11;
#endif
    if (full) {
        set_fullscreen(v, !fullscreen(v));
        return true;
    }
    if (sym == SDLK_ESCAPE) {
        if (v->ui.settings_open) v->ui.settings_open = false;
        else if (fullscreen(v)) set_fullscreen(v, false);
        else *quit = true;
        return true;
    }
    if ((sym == SDLK_s && !cmd && !ctrl) || (sym == SDLK_COMMA && cmd)) {
        if (!k->repeat) v->ui.settings_open = !v->ui.settings_open;
        return true;
    }
    if (!cmd) return false;
    if (sym == SDLK_0 || sym == SDLK_KP_0) reset(v);
    else if (sym == SDLK_PLUS || sym == SDLK_EQUALS || sym == SDLK_KP_PLUS) zoom(v, 1);
    else if (sym == SDLK_MINUS || sym == SDLK_KP_MINUS) zoom(v, -1);
    return true;   /* other shortcut keys don't reach the board either */
}

int sim_window_run(const char *title, const char *tty, int scale, const char *view_file,
                   int (*should_quit)(void), void (*tick)(void), const char *shot)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    view_t v = { .fit = true, .start_scale = scale > 0 ? scale : 2 };
    v.ui.grid = true;
    snprintf(v.ui.tty_path, sizeof v.ui.tty_path, "%s", tty);
    if (view_file) snprintf(v.path, sizeof v.path, "%s", view_file);
    sim_board_ui_size(1, &v.base_w, &v.base_h);
    int w = v.base_w * v.start_scale, h = v.base_h * v.start_scale, saved_w = 0, saved_h = 0;
    view_load(&v, &saved_w, &saved_h);
    if (scale <= 0 && saved_w > 0) { w = saved_w; h = saved_h; }   /* --scale wins over the saved size */

    v.win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    v.r = v.win ? SDL_CreateRenderer(v.win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC |
                                                     SDL_RENDERER_TARGETTEXTURE) : NULL;
    if (!v.r) {
        fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        return 1;
    }
    v.ui.r = v.r;
    float k = dpi(&v);
    SDL_SetWindowMinimumSize(v.win, (int)ceilf((float)v.base_w / k), (int)ceilf((float)v.base_h / k));
    /* The asked-for size, made to fit this display (and whole in Pixel exact). */
    float f = (float)w * k / (float)v.base_w;
    if (!v.fit) f = fmaxf(1.0f, roundf(f));
    resize_to(&v, f);
    SDL_SetWindowPosition(v.win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_GetWindowSize(v.win, &v.last_w, &v.last_h);
    SDL_AddEventWatch(live_resize, &v);

    bool quit = false;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: quit = true; break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED && !fullscreen(&v)) {
                    if (e.window.data1 == v.want_w && e.window.data2 == v.want_h) {
                        v.want_w = v.want_h = 0;          /* our own resize */
                        v.last_w = e.window.data1;
                        v.last_h = e.window.data2;
                    } else {
                        v.resized_at = SDL_GetTicks() | 1;   /* the user's: snap once it's over */
                    }
                }
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                if (!view_key(&v, &e.key, &quit)) sim_board_ui_event(&v.ui, &e);
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                to_canvas(&v, &e.button.x, &e.button.y);
                if (v.ui.settings_open) settings_mouse(&v, &e);
                /* A release always reaches the board too: nothing stays held there. */
                if (!v.ui.settings_open || e.type == SDL_MOUSEBUTTONUP) sim_board_ui_event(&v.ui, &e);
                break;
            case SDL_MOUSEMOTION:
                to_canvas(&v, &e.motion.x, &e.motion.y);
                if (v.ui.settings_open) settings_mouse(&v, &e);
                else sim_board_ui_event(&v.ui, &e);
                break;
            default:
                break;
            }
        }
        if (v.resized_at && SDL_GetTicks() - v.resized_at >= SNAP_MS) snap(&v);
        if (tick) tick();
        if (should_quit && should_quit()) quit = true;
        frame(&v, quit ? shot : NULL);
        SDL_Delay(15);
    }
    view_save(&v);
    SDL_DelEventWatch(live_resize, &v);
    SDL_DestroyRenderer(v.r);
    SDL_DestroyWindow(v.win);
    SDL_Quit();
    return 0;
}
