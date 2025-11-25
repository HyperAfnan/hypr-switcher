#define _XOPEN_SOURCE 700  /* Ensure POSIX prototype for usleep is visible */

#include "wayland.h"
#include "logger/logger.h"
#include "render.h"
#include "util.h"
#include "ipc.h"
#include "input.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h> /* for nanosleep delay after successful focus */
#include <poll.h> /* for poll-based non-blocking event loop */

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_surface *surface;
static struct zwlr_layer_surface_v1 *layer_surface;
static struct wl_seat *seat;

static int initial_width = 600;
static int initial_height = 120;

static uint32_t current_width  = 600;
static uint32_t current_height = 120;

static int configured = 0;

static HyprClientInfo *g_clients = NULL;
static size_t g_client_count = 0;
static const char **g_titles = NULL;
static int g_selection_index = -1;

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

    /* Free any previous snapshot */
    if (g_titles) { free(g_titles); g_titles = NULL; }
    if (g_clients) { hypr_ipc_free_client_infos(g_clients, g_client_count); g_clients = NULL; g_client_count = 0; }

    int minimumHeight = 50;
    g_selection_index = -1;

    if (hypr_ipc_get_clients_basic(&g_clients, &g_client_count) == 0) {

        for (size_t i = 0; i < g_client_count; ++i) {
            if (g_clients[i].focused || g_clients[i].focusHistoryID == 0) {
                g_selection_index = (int)i;
                break;
            }
        }
        if (g_selection_index < 0 && g_client_count > 0) g_selection_index = 0;

        uint32_t desired_height = (uint32_t)(g_client_count * minimumHeight);
        if (desired_height < (uint32_t)minimumHeight) desired_height = minimumHeight;

        current_height = desired_height;

        zwlr_layer_surface_v1_set_size(lsurf, current_width, current_height);
        LOG_DEBUG("Dynamic height: %u (clients: %zu, focus=%d)", current_height, g_client_count, g_selection_index);

        g_titles = NULL;
        if (g_client_count > 0) {
            g_titles = calloc(g_client_count, sizeof(char *));
        }

        if (g_titles) {
            for (size_t i = 0; i < g_client_count; ++i) {
                g_titles[i] = g_clients[i].app_class ? g_clients[i].app_class :
                              (g_clients[i].title ? g_clients[i].title : "(untitled)");
            }
            render_draw_titles_focus(surface, current_width, current_height, g_titles, g_client_count, g_selection_index);
        } else {
            render_draw_titles_focus(surface, current_width, current_height, NULL, 0, -1);
        }

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

    /* Enable keyboard interactivity so we receive key events  */
    input_enable_layer_keyboard(layer_surface);
    zwlr_layer_surface_v1_add_listener(layer_surface, &layer_surface_listener, NULL);

    wl_surface_commit(surface);
}

/* wayland_render removed (unused) */

/* Helper: attempt focusing the currently selected client with unified logging and delay. */
static void wayland_focus_selected(const char *tag) {
    if (g_clients && g_client_count > 0 &&
        g_selection_index >= 0 &&
        g_selection_index < (int)g_client_count) {
        HyprClientInfo *sel = &g_clients[g_selection_index];
        LOG_INFO("[FOCUS] %s Selected index=%d address=%s class=%s title=%s",
                 tag ? tag : "",
                 g_selection_index,
                 sel->address ? sel->address : "(null)",
                 sel->app_class ? sel->app_class : "(null)",
                 sel->title ? sel->title : "(null)");
        int frc = hypr_ipc_focus_client(sel);
        if (frc == 0) {
            LOG_INFO("[FOCUS] %s Focus attempt succeeded.", tag ? tag : "");
            /* Removed blocking nanosleep; focus confirmation handled by non-blocking loop */
        } else {
            LOG_WARN("[FOCUS] %s Focus attempt failed.", tag ? tag : "");
        }
    } else {
        LOG_WARN("[FOCUS] %s No valid selection (index=%d count=%zu).",
                 tag ? tag : "",
                 g_selection_index, g_client_count);
    }
}

void wayland_loop() {
    if (!display) return;
    int fd = wl_display_get_fd(display);
    struct pollfd pfd = { .fd = fd, .events = POLLIN };

    while (display) {
        /* Process any already queued (non-blocking) Wayland events */
        wl_display_dispatch_pending(display);

        /* Input / lifecycle checks */
        if (input_focus_lost()) {
            LOG_INFO("[INPUT] Focus lost; attempting focus then closing overlay.");
            wayland_focus_selected("(focus-lost)");
            wayland_shutdown();
            break;
        }
        if (input_escape_pressed()) {
            LOG_INFO("[INPUT] Escape pressed, shutting down.");
            wayland_shutdown();
            break;
        }
        if (input_alt_tab_triggered()) {
            if (g_client_count > 0 && g_titles) {
                g_selection_index = (g_selection_index + 1) % (int)g_client_count;
                LOG_DEBUG("[INPUT] Alt+Tab triggered; new selection index: %d", g_selection_index);
                render_draw_titles_focus(surface, current_width, current_height, g_titles, g_client_count, g_selection_index);
            }
        }
        if (input_alt_released()) {
            LOG_INFO("[INPUT] Alt released; attempting to focus selected client.");
            wayland_focus_selected("");
            wayland_shutdown();
            break;
        }

        /* Prepare to block for new events with timeout, allowing periodic logic */
        if (wl_display_prepare_read(display) != 0) {
            /* If prepare_read fails (events queued), loop will dispatch them next iteration */
            continue;
        }

        wl_display_flush(display);

        int timeout_ms = 50; /* wake 20 times per second to remain responsive */
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            /* Read and queue events then dispatch them */
            if (wl_display_read_events(display) == 0) {
                wl_display_dispatch_pending(display);
            } else {
                LOG_WARN("[WAYLAND] read_events failed; shutting down.");
                wayland_shutdown();
                break;
            }
        } else {
            /* Timeout or interrupted; cancel read so we can inspect state again */
            wl_display_cancel_read(display);
            /* Continue loop; this gives us non-blocking periodic ticks */
        }
    }
}

void wayland_shutdown() {
    /* Destroy layer surface early to let compositor reclaim resources */
    if (layer_surface) { zwlr_layer_surface_v1_destroy(layer_surface); layer_surface = NULL; }

    /* Free snapshot state */
    if (g_titles) { free(g_titles); g_titles = NULL; }
    if (g_clients) { hypr_ipc_free_client_infos(g_clients, g_client_count); g_clients = NULL; g_client_count = 0; }

    /* Input after layer surface so no more events target destroyed surface */
    input_shutdown();

    if (surface) { wl_surface_destroy(surface); surface = NULL; }
    if (layer_shell) { zwlr_layer_shell_v1_destroy(layer_shell); layer_shell = NULL; }
    if (compositor) { wl_compositor_destroy(compositor); compositor = NULL; }
    if (shm) { wl_shm_destroy(shm); shm = NULL; }
    if (seat) { wl_seat_destroy(seat); seat = NULL; }
    if (display) { wl_display_disconnect(display); display = NULL; }
}
