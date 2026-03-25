/**
 * @file tui.h
 * @brief Terminal User Interface for the Just Weather Client
 *
 * Provides an ncurses-based TUI with hierarchical menus, keyboard navigation
 * (arrow keys), and results display with scrolling support.
 */
#ifndef TUI_H
#define TUI_H

#include "../api/weather_client.h"

/**
 * @brief Opaque TUI context.  All ncurses state lives inside this struct.
 */
typedef struct TuiContext TuiContext;

/**
 * @brief Allocate and initialise ncurses + TUI windows.
 *
 * Calls initscr(), cbreak(), noecho(), keypad(), start_color().
 * Returns NULL if the terminal is too small (< 10 rows / 50 cols) or on
 * any allocation failure.  The terminal is restored to a usable state
 * before returning NULL.
 *
 * @param client  Initialised WeatherClient to use for all API calls.
 * @return New TuiContext or NULL on failure.
 */
TuiContext* tui_create(WeatherClient* client);

/**
 * @brief Tear down ncurses and free all TUI memory.
 * Safe to call with NULL.
 */
void tui_destroy(TuiContext* ctx);

/**
 * @brief Enter the main event loop.  Blocks until the user quits.
 */
void tui_run(TuiContext* ctx);

#endif /* TUI_H */
