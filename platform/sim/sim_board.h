/*
 * Interface between the generic simulator (sim_main.c, sim_ui.c) and one
 * board's simulation (platform/sim/boards/<board>/). Exactly one board is
 * compiled in, selected with `-DBOARD=...` (CMake).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sim_ui.h"

/** Board variant, before sim_board_init(). Returns false if unknown. */
bool sim_board_set_model(const char *name);
/** The variants sim_board_set_model() accepts, for --help. */
const char *sim_board_models(void);
/** Where the board keeps state across runs (e.g. "<nvs>.rtc"): the base path. */
void sim_board_set_state_path(const char *base);
/** Create the device models and attach them to the mocked buses. */
void sim_board_init(void);
/** Board-specific option from `--set key=value`. Returns false if unknown. */
bool sim_board_set(const char *key, const char *value);
/** One-line help for the board's --set keys. */
const char *sim_board_set_help(void);
/** The inputs that stay through a reset of the chip (battery, units, the
 *  access point, ...), as space-separated KEY=VALUE --set inputs: a reboot
 *  starts with these instead of the start's --set (no finger at boot). */
void sim_board_state(char *out, size_t cap);
/** The board is powered down and something turns it on now: the power key,
 *  USB power plugged in, or the RTC chip's timer or alarm. */
bool sim_board_wake_due(void);
/** Is this sensor of the board ("imu", "light") switched on and measuring? */
bool sim_board_sensor_on(const char *name);
/** Current visible LCD image as RGB888; returns false if not available. */
bool sim_board_screen_rgb(uint8_t *rgb, int *w, int *h);

/* Inputs offered in the window's settings, each one a --set key, so the
   settings need no board code: a range of numbers, or a choice of values. */
typedef struct {
    const char *label;      /* "Port B analog (G9)" */
    const char *key;        /* the --set key it changes */
    const char *choices;    /* "none|env": these values, as buttons; NULL for a range */
    const char *names;      /* the buttons' names, "None|ENV IV" */
    int min, max;           /* the range */
    const char *unit;       /* shown after the number, e.g. " %" */
} sim_board_opt_t;

/** The settings' inputs; *n of them. */
const sim_board_opt_t *sim_board_options(int *n);
/** An input's current value: the number, or the index of the choice. */
int sim_board_get(const char *key);

/* Window: drawn on a canvas at an integer scale (LCD pixel -> canvas
   pixels). Its size must be scale times its size at scale 1, so the
   window keeps the same aspect ratio at every scale. Events come in
   canvas coordinates. */
void sim_board_ui_size(int scale, int *w, int *h);
void sim_board_ui_draw(sim_ui_t *ui);
void sim_board_ui_event(sim_ui_t *ui, const SDL_Event *e);
