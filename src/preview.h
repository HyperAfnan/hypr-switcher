#pragma once
#include "ipc.h"
#include <cairo/cairo.h>
#include <stdbool.h>

/* Initiate capture for a specific client if not already captured/capturing */
void preview_capture_window(const char *address);

/* Get the cairo surface for a client address, or NULL if not ready/failed */
cairo_surface_t *preview_get_surface(const char *address);

/* Free all cached previews */
void preview_cleanup_all(void);
