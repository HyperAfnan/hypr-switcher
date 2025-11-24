#pragma once
#include <wayland-client.h>
#include <stddef.h>

void render_draw(struct wl_surface *surface, int width, int height);
void render_draw_titles(struct wl_surface *surface, int width, int height, const char **titles, size_t count);
