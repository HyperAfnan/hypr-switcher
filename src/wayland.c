#include "wayland.h"
#include "logger/logger.h"
#include "render.h"
#include "util.h"
#include "ipc.h"
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_surface *surface;
static struct zwlr_layer_surface_v1 *layer_surface;

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

    char **titles = NULL;
    size_t count = 0;
    int minimumHeight = 40;

    if (hypr_ipc_get_client_titles(&titles, &count) == 0) {

        uint32_t desired_height = (uint32_t)(count * minimumHeight);
        if (desired_height < (uint32_t)minimumHeight) desired_height = minimumHeight;

        current_height = desired_height;

        zwlr_layer_surface_v1_set_size(lsurf, current_width, current_height);
        LOG_DEBUG("Dynamic height: %u (titles: %zu)", current_height, count);

        render_draw_titles(surface, current_width, current_height,
                           (const char **)titles, count);

        hypr_ipc_free_titles(titles, count);

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

    zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, 0);

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
    while (display && wl_display_dispatch(display) != -1) {}
}

void wayland_shutdown() {
    if (layer_surface) { zwlr_layer_surface_v1_destroy(layer_surface); layer_surface = NULL; }
    if (surface) { wl_surface_destroy(surface); surface = NULL; }
    if (layer_shell) { zwlr_layer_shell_v1_destroy(layer_shell); layer_shell = NULL; }
    if (compositor) { wl_compositor_destroy(compositor); compositor = NULL; }
    if (shm) { wl_shm_destroy(shm); shm = NULL; }
    if (display) { wl_display_disconnect(display); display = NULL; }
}
