#pragma once
#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

struct wl_display *init_wayland();
void create_layer_surface();
void wayland_loop();

/* Main event loop with IPC socket integration for single-instance coordination */
void wayland_loop_with_ipc(int ipc_listen_fd);

struct wl_shm *get_shm();