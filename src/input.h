#pragma once
/*
 * Minimal keyboard input API for hyprswitcher.
 *
 * Purpose:
 *   - Detect Escape key press to close the program.
 *   - Detect Alt+Tab chord (Alt held while Tab pressed) to trigger window cycle logic.
 *
 * Notes:
 *   - Wayland only delivers key events when this client has keyboard focus.
 *   - We enable keyboard interactivity on the layer surface to receive events.
 *   - This minimal API does NOT use xkbcommon; it relies on raw evdev keycodes.
 *     Common codes used:
 *       ESC      = 1
 *       TAB      = 15
 *       LEFT ALT = 56
 *       RIGHT ALT= 100
 *
 * Usage flow:
 *   1. In the registry global handler: when "wl_seat" appears, bind it and call input_handle_seat(seat).
 *   2. After creating the layer surface (before or just after initial commit), call input_enable_layer_keyboard(layer_surface).
 *   3. In the Wayland dispatch loop (each iteration after wl_display_dispatch):
 *        if (input_escape_pressed()) { shutdown / exit }
 *        if (input_alt_tab_triggered()) { cycle windows }
 *   4. On program shutdown call input_shutdown().
 *
 * All query functions (input_escape_pressed, input_alt_tab_triggered) are one-shot:
 * they clear their internal flag after returning true once.
 */

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* Attach keyboard listener to a seat. Call once when wl_seat is discovered. */
void input_handle_seat(struct wl_seat *seat);

/* Enable keyboard interactivity for the layer surface (must be called so we receive key events). */
void input_enable_layer_keyboard(struct zwlr_layer_surface_v1 *layer_surface);
/* Query whether the input system currently believes it has keyboard focus. */
bool input_has_focus(void);
bool input_focus_lost(void);

/* Returns true exactly once per Escape key press (press event). */
bool input_escape_pressed(void);

/* Returns true exactly once per Alt+Tab chord (Alt is down when Tab pressed). */
bool input_alt_tab_triggered(void);

/* Clear both Escape and Alt+Tab flags manually (optional). */
void input_clear_flags(void);

/* Release resources (keyboard object) and clear internal state. */
void input_shutdown(void);