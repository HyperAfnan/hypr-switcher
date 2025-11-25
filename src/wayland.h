#pragma once
#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

struct wl_display *init_wayland();
void create_layer_surface();
void wayland_loop();

struct wl_shm *get_shm();
