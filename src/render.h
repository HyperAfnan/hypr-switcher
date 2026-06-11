#pragma once
#include <wayland-client.h>
#include <stddef.h>

#include "ipc.h"

void render_draw(struct wl_surface *surface, int width, int height);
void render_draw_clients(struct wl_surface *surface, int width, int height, HyprClientInfo *clients, size_t count, int focused_index);
