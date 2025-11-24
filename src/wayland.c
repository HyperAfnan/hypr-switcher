#include "wayland.h"
#include "logger/logger.h"
#include "render.h"
#include "util.h"
#include "ipc.h"
#include "input.h" /* input API for ESC and Alt+Tab */
#include <stdint.h>
#include <string.h>
#include <stdio.h>

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_surface *surface;
static struct zwlr_layer_surface_v1 *layer_surface;
static struct wl_seat *seat; /* seat to attach keyboard */

static int initial_width = 600;
static int initial_height = 120;

static uint32_t current_width  = 600;
static uint32_t current_height = 120;

static int configured = 0;

static void registry_add(void *data, struct wl_registry *registry,
        uint32_t name, const char *interface, uint32_t version)
{
    (void)data;
    (void)version;

    if (strcmp(interface, "wl_compositor") == 0) {
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    }
    else if (strcmp(interface, "wl_shm") == 0) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    }
    else if (strcmp(interface, "zwlr_layer_shell_v1") == 0) {
        layer_shell = wl_registry_bind(registry, name,
            &zwlr_layer_shell_v1_interface, 3);
    }
    else if (strcmp(interface, "wl_seat") == 0) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 7);
        input_handle_seat(seat);
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_add,
    .global_remove = registry_remove,
};

void wayland_shutdown();


static void layer_surface_configure(void *data,
    struct zwlr_layer_surface_v1 *lsurf,
    uint32_t serial, uint32_t width, uint32_t height)
{
    (void)data;

    /* Compositor may send 0; fall back to initial size */
    if (width == 0)  width  = (uint32_t)initial_width;
    if (height == 0) height = (uint32_t)initial_height;

    /* Save current compositor-approved size */
    current_width = width;
    current_height = height;

    zwlr_layer_surface_v1_ack_configure(lsurf, serial);
    configured = 1;

    HyprClientInfo *clients = NULL;
    size_t count = 0;
    int minimumHeight = 50;
    int focused_index = -1;

    if (hypr_ipc_get_clients_basic(&clients, &count) == 0) {

        for (size_t i = 0; i < count; ++i) {
            if (clients[i].focused || clients[i].focusHistoryID == 0) {
                focused_index = (int)i;
                break;
            }
        }

        uint32_t desired_height = (uint32_t)(count * minimumHeight);
        if (desired_height < (uint32_t)minimumHeight) desired_height = minimumHeight;

        current_height = desired_height;

        zwlr_layer_surface_v1_set_size(lsurf, current_width, current_height);
        LOG_DEBUG("Dynamic height: %u (clients: %zu, focus=%d)", current_height, count, focused_index);

        const char **titles = calloc(count, sizeof(char *));
        if (titles) {
            for (size_t i = 0; i < count; ++i) {
                titles[i] = clients[i].app_class ? clients[i].app_class :
                            (clients[i].title ? clients[i].title : "(untitled)");
            }
            render_draw_titles_focus(surface, current_width, current_height, titles, count, focused_index);
            free(titles);
        } else {
            render_draw_titles_focus(surface, current_width, current_height, NULL, 0, -1);
        }

        hypr_ipc_free_client_infos(clients, count);

    } else {
        render_draw(surface, current_width, current_height);
    }
}

static void layer_surface_closed(void *data,
    struct zwlr_layer_surface_v1 *lsurf)
{
    (void)data; (void)lsurf;
    fprintf(stderr, "Layer surface closed by compositor.\n");
    wayland_shutdown();
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

struct wl_shm *get_shm() { return shm; }

struct wl_display *init_wayland() {
    display = wl_display_connect(NULL);
    if (!display) DIE("Failed to connect to Wayland.\n");

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor || !layer_shell || !shm)
        DIE("Missing Wayland globals.\n");

    return display;
}

void create_layer_surface() {
    surface = wl_compositor_create_surface(compositor);

    layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, surface, NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "hyprswitcher");

    zwlr_layer_surface_v1_set_size(layer_surface, current_width, current_height);
    zwlr_layer_surface_v1_set_anchor(layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);

    /* Enable keyboard interactivity so we receive key events (ESC, Alt+Tab) */
    input_enable_layer_keyboard(layer_surface);

    zwlr_layer_surface_v1_add_listener(layer_surface,
                                       &layer_surface_listener, NULL);

    wl_surface_commit(surface);
}

void wayland_render() {
    if (configured) {
        render_draw(surface, current_width, current_height);
    }
}

void wayland_loop() {
    while (display && wl_display_dispatch(display) != -1) {
        /* Auto-close if we lost keyboard focus (overlay becomes inert) */
        if (input_focus_lost()) {
            LOG_INFO("[INPUT] Focus lost; closing overlay.");
            wayland_shutdown();
            break;
        }
        if (input_escape_pressed()) {
            LOG_INFO("[INPUT] Escape pressed, shutting down.");
            wayland_shutdown();
            break;
        }
        if (input_alt_tab_triggered()) {
            LOG_INFO("[INPUT] Alt+Tab detected, closing overlay.");
            wayland_shutdown();
            break;
        }
    }
}

void wayland_shutdown() {
    input_shutdown();
    if (layer_surface) { zwlr_layer_surface_v1_destroy(layer_surface); layer_surface = NULL; }
    if (surface) { wl_surface_destroy(surface); surface = NULL; }
    if (layer_shell) { zwlr_layer_shell_v1_destroy(layer_shell); layer_shell = NULL; }
    if (compositor) { wl_compositor_destroy(compositor); compositor = NULL; }
    if (shm) { wl_shm_destroy(shm); shm = NULL; }
    if (seat) { wl_seat_destroy(seat); seat = NULL; }
    if (display) { wl_display_disconnect(display); display = NULL; }
}
