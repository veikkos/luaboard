/* The simulator's window: scaling, the settings, and its event loop. */
#pragma once

/**
 * Open the window and run until it's closed or should_quit() says so,
 * calling tick() once a frame. scale: window points per LCD pixel, 0 for
 * the size kept in view_file (or 2). view_file keeps the view between runs
 * (NULL: not kept). shot: save the board's canvas as BMP on exit.
 */
int sim_window_run(const char *title, const char *tty, int scale, const char *view_file,
                   int (*should_quit)(void), void (*tick)(void), const char *shot);
